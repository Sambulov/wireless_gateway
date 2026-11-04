"""Жизнь соединений WebSocket под нагрузкой: слоты не утекают, PONG не рвёт
поток данных, обрыв посреди отправки не роняет прибор.

Транспорт (main/ws_transport.c) на хосте не собирается — проверяем на прошивке.
Регрессии: события ESTABLISHED/CLOSED терялись при одновременном подъёме из
задач httpd и такта (утечка слотов пула), session_free_ctx освобождал tx_buf
без замка кадра, частичная запись PONG/CLOSE перемешивалась с данными.

Запуск (по умолчанию — прошивка в QEMU, run_qemu.sh: WS на 8081):
    pytest tests/integrational/web_ws
Прибор:  WG_HOST=192.168.4.1 WG_WS_PORT=81 pytest tests/integrational/web_ws
"""
import json
import os
import socket
import time

import pytest
import websocket          # websocket-client

HOST = os.environ.get("WG_HOST", "127.0.0.1")
PORT = int(os.environ.get("WG_WS_PORT", "8081"))
URL = f"ws://{HOST}:{PORT}/api"
MAX_CLIENTS = int(os.environ.get("WG_WS_MAX_CLIENTS", "10"))   # CONFIG_WEB_SOCKET_MAX_CLIENTS


def connect(timeout=10):
    # Одна повторная попытка: под частыми подключениями рукопожатие изредка
    # не отвечает ~10 с — это отдельная, более старая находка (есть и до
    # фикса транспорта, issues/2026-11-03_review_4f5017b_a53dcc8.txt, #15).
    # Утечку слотов повтор не прячет: она даёт постоянные отказы.
    try:
        return websocket.create_connection(URL, timeout=timeout)
    except websocket.WebSocketTimeoutException:
        return websocket.create_connection(URL, timeout=timeout)


def echo(ws, sid, arg):
    ws.send(json.dumps({"FID": "echo", "SID": sid, "ARG": arg}))
    deadline = time.time() + 10
    while time.time() < deadline:
        msg = json.loads(ws.recv())
        if msg.get("SID") == f"0x{sid:08x}":
            return msg
    raise AssertionError(f"нет ответа на SID {sid}")


def test_connection_churn_does_not_leak_slots():
    # Много коротких сессий: часть закрывается штатно, часть — обрывом сокета
    # посреди ответа. Слот каждой обязан вернуться в пул.
    for i in range(3 * MAX_CLIENTS):
        ws = connect()
        ws.send(json.dumps({"FID": "echo", "SID": i + 1, "ARG": {"n": i}}))
        if i % 2:
            ws.close()
        else:
            ws.sock.shutdown(socket.SHUT_RDWR)
            ws.sock.close()
    time.sleep(2)

    # Все слоты свободны: можно открыть предел одновременных клиентов.
    conns = []
    try:
        for i in range(MAX_CLIENTS):
            ws = connect()
            assert echo(ws, 100 + i, {"k": i})["ARG"] == {"k": i}
            conns.append(ws)
    finally:
        for ws in conns:
            ws.close()


def test_pong_arrives_intact_while_data_flows():
    ws = connect()
    try:
        big = {"pad": "x" * 3000}
        for sid in range(1, 21):                      # поток ответов на стороне прибора
            ws.send(json.dumps({"FID": "echo", "SID": sid, "ARG": big}))
        ws.ping(b"wg-ping")
        got_pong, answers = False, 0
        deadline = time.time() + 20
        while time.time() < deadline and not (got_pong and answers == 20):
            op, data = ws.recv_data(control_frame=True)   # битый кадр — исключение
            if op == websocket.ABNF.OPCODE_PONG:
                assert data == b"wg-ping"
                got_pong = True
            elif op == websocket.ABNF.OPCODE_TEXT:
                msg = json.loads(data)
                assert msg["ARG"] == big
                answers += 1
        assert got_pong, "PONG не пришёл"
        assert answers == 20
    finally:
        ws.close()


def test_abort_mid_transfer_keeps_device_serving():
    big = {"pad": "y" * 6000}
    for round_ in range(20):
        ws = connect()
        for sid in range(1, 6):
            ws.send(json.dumps({"FID": "echo", "SID": sid, "ARG": big}))
        ws.sock.shutdown(socket.SHUT_RDWR)            # обрыв, пока прибор отвечает
        ws.sock.close()
    time.sleep(2)
    ws = connect()
    try:
        assert echo(ws, 7, {"alive": 1})["ARG"] == {"alive": 1}
    finally:
        ws.close()


@pytest.mark.skipif("WG_HOST" not in os.environ,
                    reason="только на приборе: сеть QEMU (slirp) буферизует TCP сама, "
                           "и прошивка не видит клиента, который перестал читать")
def test_client_that_stops_reading_is_disconnected():
    """Клиент прислал PING и перестал читать: прибор не держит соединение вечно
    — через WS_OUT_DEAD_MS (20 с) без прогресса закрывает его, в том числе когда
    застрял управляющий кадр (PONG), а не кадр данных."""
    ws = websocket.create_connection(
        URL, timeout=10, sockopt=[(socket.SOL_SOCKET, socket.SO_RCVBUF, 2048)])
    big = {"pad": "z" * 3000}
    for sid in range(1, 41):
        ws.send(json.dumps({"FID": "echo", "SID": sid, "ARG": big}))
    ws.ping(b"stuck")
    time.sleep(32)                                  # > WS_OUT_DEAD_MS, не читаем

    closed = False
    ws.sock.settimeout(5)
    try:
        while True:
            if not ws.sock.recv(65536):
                closed = True
                break
    except socket.timeout:
        closed = False
    except (ConnectionResetError, BrokenPipeError):
        closed = True
    finally:
        ws.sock.close()
    assert closed, "прибор не закрыл соединение клиента, который не читает"


def test_close_is_answered_with_close():
    """На CLOSE клиента прибор отвечает CLOSE (RFC 6455 §5.5.1) — в том числе
    сразу после PONG (ответ не теряется за управляющим кадром)."""
    ws = connect()
    try:
        ws.ping(b"p")
        ws.send_close()
        deadline = time.time() + 10
        got_close = False
        while time.time() < deadline:
            op, _ = ws.recv_data(control_frame=True)
            if op == websocket.ABNF.OPCODE_CLOSE:
                got_close = True
                break
        assert got_close, "ответ CLOSE не пришёл"
    finally:
        ws.sock.close()
