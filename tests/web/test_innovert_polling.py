"""Страницы innovert опрашивают регистры сами (сервер не повторяет запросы:
RDL в протоколе нет). Скрипт страницы запускается в node
(tests/web/page_harness.js: DOM — заглушка, WebSocket и таймеры — подделки,
ответы — как у шлюза: SID "0x…", RA 0, в RD — номера регистров). Нет node —
пропуск.

Регрессии:
- опрос уходил один раз при подключении, таблица застывала;
- значения раскладывались по RA ответа, а в ответе RTU на чтение адреса нет
  (шлюз отдавал 0): каждый диапазон писался в регистры с нуля — «прыгало»;
- все 28 диапазонов уходили разом при очереди порта шлюза в 8: 20 каждый раз
  получали «занято», часть регистров не читалась никогда.

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
WINDOW = 4          # POLL_WINDOW страниц


def run(page, steps):
    inp = {"page": str(ROOT / page), "values": {"uart-sel": "uart2.modbus", "mb-addr": "1"},
           "steps": steps}
    r = subprocess.run([NODE, str(HARNESS)], input=json.dumps(inp), capture_output=True,
                       text=True, timeout=60)
    assert r.returncode == 0, r.stderr
    out = {}
    for m in json.loads(r.stdout):
        out[m["mark"]] = m.get("sent", m.get("value"))
    return out


def reads(sent):
    return [m for m in sent if m["FID"] == "uart2.modbus"]


def all_ranges(page):
    """Все диапазоны карты страницы — по её же buildRanges."""
    out = run(page, [{"eval": "JSON.stringify(x.buildRanges())", "eval_mark": "r"}])
    return {r["s"] for r in json.loads(out["r"])}


pytestmark = pytest.mark.skipif(NODE is None, reason="нет node")


@pytest.mark.parametrize("page", PAGES)
def test_keeps_at_most_a_window_of_requests_in_flight(page):
    out = run(page, [{"open": 1}, {"mark": "first"}, {"advance": 1000}, {"mark": "later"}])
    assert len(reads(out["first"])) == WINDOW
    assert len(reads(out["later"])) == WINDOW          # без ответов новых нет


@pytest.mark.parametrize("page", PAGES)
def test_every_range_gets_polled(page):
    expected = all_ranges(page)
    steps = [{"open": 1}]
    for i in range(len(expected) // WINDOW + 3):
        steps += [{"mark": f"m{i}"}, {"reply_all": 1}, {"advance": 50}]
    out = run(page, steps)
    seen = {m["ARG"]["RA"] for k, v in out.items() for m in reads(v)}
    assert expected <= seen, f"не опрошены: {sorted(expected - seen)}"


@pytest.mark.parametrize("page", PAGES)
def test_values_land_at_the_requested_address(page):
    # ответ приходит с RA 0 — значения обязаны лечь от RA запроса
    expected = sorted(all_ranges(page))
    target = next(a for a in expected if a > 0)
    steps = [{"open": 1}]
    for _ in range(len(expected) // WINDOW + 3):
        steps += [{"reply_all": 1}, {"advance": 50}]
    steps += [{"eval": f"x._regMap.get({target}).VALUE", "eval_mark": "v"},
              {"eval": "x._regMap.get(0) ? x._regMap.get(0).VALUE : 0", "eval_mark": "v0"}]
    out = run(page, steps)
    assert out["v"] == target          # в RD подделки — номер регистра
    assert out["v0"] == 0              # регистр 0 не перезаписан чужими диапазонами


@pytest.mark.parametrize("page", PAGES)
def test_lost_requests_free_the_window_after_stale_time(page):
    out = run(page, [
        {"open": 1}, {"drain": 1},
        {"advance": 1500}, {"mark": "silent"},
        {"advance": 1000}, {"mark": "stale"},
    ])
    assert reads(out["silent"]) == []
    assert len(reads(out["stale"])) == WINDOW


@pytest.mark.parametrize("page", PAGES)
def test_polling_stops_when_connection_drops(page):
    out = run(page, [
        {"open": 1}, {"reply_all": 1}, {"close": 1},
        {"advance": 3000}, {"mark": "after_close"},
    ])
    assert reads(out["after_close"]) == []
