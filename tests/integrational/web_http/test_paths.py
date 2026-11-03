"""Путь страницы из URI: длинный и выходящий за корень — 404, прибор жив.

Регрессия: URI длиннее буфера пути давал NULL, обработчик звал stat(NULL) —
одного GET хватало для перезагрузки прибора.

Запуск — как test_busy_port.py (по умолчанию прошивка в QEMU на 8080).
"""
import http.client
import os

HOST = os.environ.get("WG_HOST", "127.0.0.1")
PORT = int(os.environ.get("WG_HTTP_PORT", "8080"))


def status(path):
    c = http.client.HTTPConnection(HOST, PORT, timeout=10)
    try:
        c.request("GET", path)
        r = c.getresponse()
        r.read()
        return r.status
    finally:
        c.close()


def test_long_uri_is_not_found_and_device_survives():
    assert status("/" + "a" * 300) == 404
    assert status("/") == 200          # прибор не перезагрузился


def test_uri_escaping_the_root_is_not_found():
    assert status("/../index.html") == 404
    assert status("/a/../../index.html") == 404


def test_query_does_not_change_the_file():
    assert status("/?v=1") == 200
