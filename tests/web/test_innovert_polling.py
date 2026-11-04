"""Страницы innovert опрашивают регистры сами (сервер не повторяет запросы:
RDL в протоколе нет). Раньше опрос уходил один раз при подключении, и таблица
застывала. Скрипт страницы запускается в node (tests/web/page_harness.js:
DOM — заглушка, WebSocket и таймеры — подделки). Нет node — пропуск.

Запуск:  pytest tests/web
"""
import json
import pathlib
import shutil
import subprocess

import pytest

ROOT = pathlib.Path(__file__).resolve().parents[2]
HARNESS = ROOT / "tests" / "web" / "page_harness.js"
NODE = shutil.which("node")
PAGES = ["main/flash_data/innovert.html", "main/flash_data/innovert_isd.html"]


def run(page, steps):
    inp = {"page": str(ROOT / page), "values": {"uart-sel": "uart2.modbus", "mb-addr": "1"},
           "steps": steps}
    r = subprocess.run([NODE, str(HARNESS)], input=json.dumps(inp), capture_output=True,
                       text=True, timeout=30)
    assert r.returncode == 0, r.stderr
    return {m["mark"]: m["sent"] for m in json.loads(r.stdout)}


def reads(sent):
    return [m for m in sent if m["FID"] == "uart2.modbus"]


@pytest.mark.skipif(NODE is None, reason="нет node")
@pytest.mark.parametrize("page", PAGES)
def test_polls_again_after_answers(page):
    out = run(page, [
        {"open": 1}, {"mark": "first"}, {"reply_all": 1},
        {"advance": 400}, {"mark": "second"},
    ])
    first, second = reads(out["first"]), reads(out["second"])
    assert first, "первый круг опроса не ушёл"
    assert len(second) == len(first), "после ответов новый круг опроса не ушёл"
    assert all("RDL" not in m["ARG"] for m in first + second)


@pytest.mark.skipif(NODE is None, reason="нет node")
@pytest.mark.parametrize("page", PAGES)
def test_does_not_pile_up_requests_while_device_is_silent(page):
    out = run(page, [
        {"open": 1}, {"drain": 1},
        {"advance": 1500}, {"mark": "silent"},          # ответов нет — ждём
        {"advance": 1000}, {"mark": "stale"},           # круг потерян — повтор
    ])
    assert reads(out["silent"]) == []
    assert reads(out["stale"]), "после тишины опрос не возобновился"


@pytest.mark.skipif(NODE is None, reason="нет node")
@pytest.mark.parametrize("page", PAGES)
def test_polling_stops_when_connection_drops(page):
    out = run(page, [
        {"open": 1}, {"reply_all": 1}, {"close": 1},
        {"advance": 3000}, {"mark": "after_close"},
    ])
    assert reads(out["after_close"]) == []          # новое соединение ещё не открыто
