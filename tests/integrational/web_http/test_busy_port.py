"""Порт страниц под полной нагрузкой: отказ 503 получает только лишняя сессия.

Прибор держит CONFIG_WEB_SERVER_MAX_CLIENTS сессий и ещё одну резервную — под
быстрый отказ «занято». Регрессия: 503 отдавался ЛЮБОМУ запросу, пока сессий
больше предела, а сам отказ оставлял keep-alive, поэтому одна лишняя вкладка
ломала все остальные.

Запуск (по умолчанию — прошивка в QEMU, run_qemu.sh: страницы на 8080):
    pytest tests/integrational/web_http
Прибор:  WG_HOST=192.168.4.1 WG_HTTP_PORT=80 pytest tests/integrational/web_http
(других клиентов на приборе в это время быть не должно — они занимают сессии).
"""
import http.client
import os
import socket
import time

HOST = os.environ.get("WG_HOST", "127.0.0.1")
PORT = int(os.environ.get("WG_HTTP_PORT", "8080"))
MAX_CLIENTS = int(os.environ.get("WG_HTTP_MAX_CLIENTS", "20"))   # CONFIG_WEB_SERVER_MAX_CLIENTS
SETTLE_S = 1.0          # дать прибору принять/закрыть соединение (open_fn/close_fn)


def get(conn, path="/"):
    conn.request("GET", path, headers={"Connection": "keep-alive"})
    resp = conn.getresponse()
    resp.read()
    return resp


def open_sessions(n):
    sessions = []
    for _ in range(n):
        c = http.client.HTTPConnection(HOST, PORT, timeout=10)
        assert get(c).status == 200
        sessions.append(c)
    return sessions


def raw_get_until_close(sock):
    """Запрос по сырому сокету и чтение ответа до EOF. Возвращает (ответ,
    закрыл_ли_сервер): http.client на "Connection: close" закрывает сокет сам,
    поэтому закрытие со стороны сервера видно только так."""
    sock.sendall(b"GET / HTTP/1.1\r\nHost: x\r\nConnection: keep-alive\r\n\r\n")
    sock.settimeout(5)
    data = b""
    try:
        while True:
            chunk = sock.recv(4096)
            if not chunk:
                return data, True
            data += chunk
    except socket.timeout:
        return data, False
    except (ConnectionResetError, BrokenPipeError):
        return data, True


def test_only_the_extra_session_is_refused_and_closed():
    sessions = open_sessions(MAX_CLIENTS)
    try:
        extra = http.client.HTTPConnection(HOST, PORT, timeout=10)
        resp = get(extra)
        assert resp.status == 503
        assert resp.getheader("Connection", "").lower() == "close"

        # Лишняя сессия со стороны клиента ещё открыта, а остальные продолжают
        # работать (раньше, пока она жива, — 503 на каждый их запрос).
        for c in sessions:
            assert get(c).status == 200
        extra.close()

        # Сервер сам закрывает лишнюю сессию, а не держит keep-alive в
        # резервном слоте.
        raw = socket.create_connection((HOST, PORT), timeout=10)
        try:
            data, closed = raw_get_until_close(raw)
        finally:
            raw.close()
        assert b" 503 " in data.split(b"\r\n", 1)[0]
        assert closed

        # Слот не залип: пока заняты все, новый клиент снова получает отказ, а
        # когда одна сессия закрылась — обслуживается.
        assert get(http.client.HTTPConnection(HOST, PORT, timeout=10)).status == 503
        sessions.pop().close()
        time.sleep(SETTLE_S)
        late = http.client.HTTPConnection(HOST, PORT, timeout=10)
        assert get(late).status == 200
        sessions.append(late)
    finally:
        for c in sessions:
            c.close()


def test_preconnected_session_is_served_once_there_is_room():
    """Соединение, открытое заранее без запроса (preconnect браузера), попало в
    резервный слот, но к первому запросу место освободилось — его обслуживают."""
    sessions = open_sessions(MAX_CLIENTS)
    pre = socket.create_connection((HOST, PORT), timeout=10)
    try:
        time.sleep(SETTLE_S)                 # прибор принял его в резервный слот
        sessions.pop().close()
        time.sleep(SETTLE_S)                 # и закрыл одну из обычных
        pre.sendall(b"GET / HTTP/1.1\r\nHost: x\r\nConnection: keep-alive\r\n\r\n")
        status_line = pre.recv(64).split(b"\r\n", 1)[0]
        assert b" 200 " in status_line, status_line
    finally:
        pre.close()
        for c in sessions:
            c.close()
