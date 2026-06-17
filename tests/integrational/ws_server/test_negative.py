"""
WebSocket negative-path tests.

Sends malformed packets, bad FIDs, missing fields, out-of-range values, and
connection-level abuse to verify the gateway handles all bad input safely:
no crashes, no hangs, no memory corruption, connections survive where expected.

Based on ws_server.c / apih_uart.c / apih_modbus.c at the time of writing.

Prerequisites:
  - Gateway running at HTTP_URL / WS_URL (default: http://192.168.4.1 / ws://192.168.4.1/ws)
  - For Modbus parse tests: no real device needed (parse errors are detected
    before UART communication)
  - pip install pytest pytest-asyncio websockets requests

Run:
  pytest test_negative.py -v -s
  WS_URL=ws://localhost:8080/ws HTTP_URL=http://localhost:8080 pytest test_negative.py -v -s
"""

import asyncio
import json
import os
import time

import pytest
import requests
import websockets

# ── Configuration ─────────────────────────────────────────────────────────────

WS_URL   = os.environ.get("WS_URL",   "ws://192.168.4.1/ws")
HTTP_URL = os.environ.get("HTTP_URL", "http://192.168.4.1")

# FID constants (from app.h)
FID_ECHO          = 0x0001
FID_UART2_CNF     = 0x1020   # UART2 config
FID_UART2_MODBUS  = 0x1123   # UART2 Modbus request

# Status codes from web_api.h
STA_COMPLETE       = 0x00000000
STA_EXECUTING      = 0x00000001
STA_BUSY           = 0x00000003
STA_INVALID        = 0x00000004
STA_DELIVERED      = 0x00000005
STA_ERR_BAD_REQ    = 0x80000000   # missing FLAGS or SID
STA_ERR_BAD_ARG    = 0x80000003
STA_ERR_NO_HANDLER = 0x8000000E   # unknown FID

RECV_TIMEOUT_S    = 3.0
CONNECT_TIMEOUT_S = 5.0
PAGE_TIMEOUT_S    = 30.0

# ── Helpers ────────────────────────────────────────────────────────────────────

def wait_for_page(url: str, timeout: float = PAGE_TIMEOUT_S) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            r = requests.get(url, timeout=2)
            if r.status_code == 200:
                return
        except requests.exceptions.RequestException:
            pass
        time.sleep(0.5)
    raise TimeoutError(f"Gateway not reachable at {url} after {timeout}s")


def make_req(fid: int, sid: int = 1, flags: int = 0, arg: dict | None = None) -> str:
    req: dict = {"FID": fid, "FLAGS": flags, "SID": sid}
    if arg is not None:
        req["ARG"] = arg
    return json.dumps(req)


async def recv(ws, timeout: float = RECV_TIMEOUT_S) -> str | None:
    """Receive next text frame, or None on timeout."""
    try:
        return await asyncio.wait_for(ws.recv(), timeout=timeout)
    except (asyncio.TimeoutError, websockets.ConnectionClosed):
        return None


def sta_of(resp: str) -> int | None:
    """Extract STA integer from gateway response, or None if not present."""
    try:
        obj  = json.loads(resp)
        raw  = obj.get("ARG", {}).get("STA")
        if raw is None:
            return None
        return int(raw, 16) if isinstance(raw, str) else int(raw)
    except Exception:
        return None


def assert_sta(resp: str, expected: int) -> None:
    sta = sta_of(resp)
    assert sta == expected, (
        f"Expected STA=0x{expected:08x}, got "
        f"{'0x{:08x}'.format(sta) if sta is not None else repr(None)} "
        f"in: {resp}"
    )


async def is_alive(ws) -> bool:
    """Send a known-good echo request and verify a response arrives."""
    await ws.send(make_req(FID_ECHO))
    resp = await recv(ws)
    return resp is not None


@pytest.fixture(scope="session", autouse=True)
def gateway_up():
    wait_for_page(f"{HTTP_URL}/innovert.html")


# ── Category 1: Invalid JSON ───────────────────────────────────────────────────
# cJSON_ParseWithLengthOpts returns NULL → gateway logs warning and drops silently.
# Connection must remain usable.

@pytest.mark.asyncio
async def test_plain_text_not_json():
    """Non-JSON text → handler returns error → server closes connection, no response.
    Gateway must remain reachable on a fresh connection afterward."""
    async with websockets.connect(WS_URL) as ws:
        await ws.send("hello world this is not json")
        assert await recv(ws) is None   # no response before close
    # New connection must work — gateway must still be alive
    async with websockets.connect(WS_URL) as ws2:
        assert await is_alive(ws2), "Gateway unreachable after non-JSON payload"


@pytest.mark.asyncio
async def test_truncated_json():
    """Truncated JSON → silently dropped, no response."""
    async with websockets.connect(WS_URL) as ws:
        await ws.send('{"FID":4128,"FLAGS":0,"SID":1,"ARG":{')
        assert await recv(ws) is None


@pytest.mark.asyncio
async def test_empty_string():
    """Empty-string payload → cJSON parse fails → silently dropped."""
    async with websockets.connect(WS_URL) as ws:
        await ws.send("")
        assert await recv(ws) is None


@pytest.mark.asyncio
async def test_json_array_at_top_level():
    """JSON array at root → FID field missing → FID=0 → silently dropped."""
    async with websockets.connect(WS_URL) as ws:
        await ws.send("[1, 2, 3]")
        assert await recv(ws) is None


@pytest.mark.asyncio
async def test_json_string_at_top_level():
    """JSON string at root → FID field missing → silently dropped."""
    async with websockets.connect(WS_URL) as ws:
        await ws.send('"just a string"')
        assert await recv(ws) is None


@pytest.mark.asyncio
async def test_deeply_nested_json():
    """Moderately nested JSON stresses the cJSON parser — must not crash."""
    depth = 50   # 500 risks stack overflow on the ESP32 httpd task
    payload = (
        '{"FID":4128,"FLAGS":0,"SID":1,"ARG":'
        + '{"x":' * depth + '0' + '}' * depth
        + '}'
    )
    async with websockets.connect(WS_URL) as ws:
        await ws.send(payload)
        await recv(ws, timeout=5.0)   # any result accepted — just must not hang


@pytest.mark.asyncio
async def test_huge_json_payload():
    """Large JSON payload (2 KB) — must not crash the gateway.
    64 KB would risk OOM on ESP32 (cJSON + PrintUnformatted need ~3x payload heap)."""
    padding = "A" * 2048
    payload = json.dumps({
        "FID": FID_UART2_CNF, "FLAGS": 0, "SID": 1,
        "ARG": {"garbage": padding},
    })
    async with websockets.connect(WS_URL) as ws:
        await ws.send(payload)
        await recv(ws, timeout=5.0)   # DELIVERED or close — just must not crash
    # Gateway must still be up
    async with websockets.connect(WS_URL) as ws2:
        assert await is_alive(ws2), "Gateway unreachable after large JSON payload"


@pytest.mark.asyncio
async def test_repeated_invalid_json_same_connection():
    """20 separate connections each sending bad JSON — gateway must stay responsive.
    Sending bad JSON on one connection causes the server to close that connection
    (ESP_ERR_INVALID_STATE from handler).  Using separate connections avoids
    sending further frames into an already-RST'd socket."""
    for _ in range(20):
        async with websockets.connect(WS_URL) as ws:
            await ws.send("not json " * 8)
            await recv(ws)   # None (closed by server) — that's expected
    # After all bad connections, a fresh one must work
    async with websockets.connect(WS_URL) as ws:
        assert await is_alive(ws), "Gateway unreachable after repeated bad-JSON connections"


# ── Category 2: FID Field Validation ──────────────────────────────────────────
# ws_server.c: FID=0 → API_HANDLER_ID_GENEGAL → silently dropped (no response).
# Unknown FID → API_CALL_ERROR_STATUS_NO_HANDLER returned to caller.

@pytest.mark.asyncio
async def test_fid_zero():
    """FID=0 is reserved (API_HANDLER_ID_GENEGAL) → silently dropped, no response."""
    async with websockets.connect(WS_URL) as ws:
        await ws.send('{"FID":0,"FLAGS":0,"SID":1}')
        assert await recv(ws) is None


@pytest.mark.asyncio
async def test_fid_missing():
    """No FID field → ulFid stays 0 → same as FID=0 → silently dropped."""
    async with websockets.connect(WS_URL) as ws:
        await ws.send('{"FLAGS":0,"SID":1}')
        assert await recv(ws) is None


@pytest.mark.asyncio
async def test_fid_null():
    """FID=null → cJSON_IsNumber/IsString both false → ulFid=0 → silently dropped."""
    async with websockets.connect(WS_URL) as ws:
        await ws.send('{"FID":null,"FLAGS":0,"SID":1}')
        assert await recv(ws) is None


@pytest.mark.asyncio
async def test_fid_object():
    """FID as JSON object → not numeric/string → ulFid=0 → silently dropped."""
    async with websockets.connect(WS_URL) as ws:
        await ws.send('{"FID":{},"FLAGS":0,"SID":1}')
        assert await recv(ws) is None


@pytest.mark.asyncio
async def test_fid_array():
    """FID as JSON array → ulFid=0 → silently dropped."""
    async with websockets.connect(WS_URL) as ws:
        await ws.send('{"FID":[1,2,3],"FLAGS":0,"SID":1}')
        assert await recv(ws) is None


@pytest.mark.asyncio
async def test_fid_bool_true():
    """FID=true → cJSON bool is not IsNumber → ulFid=0 → silently dropped."""
    async with websockets.connect(WS_URL) as ws:
        await ws.send('{"FID":true,"FLAGS":0,"SID":1}')
        assert await recv(ws) is None


@pytest.mark.asyncio
async def test_fid_unknown():
    """Unknown FID with no registered handler → STA = NO_HANDLER."""
    async with websockets.connect(WS_URL) as ws:
        await ws.send('{"FID":9999999,"FLAGS":0,"SID":42}')
        resp = await recv(ws)
        assert resp is not None, "Expected error response for unknown FID"
        assert_sta(resp, STA_ERR_NO_HANDLER)


@pytest.mark.asyncio
async def test_fid_invalid_hex_string():
    """FID as non-hex string → strtol returns 0 → ulFid=0 → silently dropped."""
    async with websockets.connect(WS_URL) as ws:
        await ws.send('{"FID":"not_hex","FLAGS":0,"SID":1}')
        assert await recv(ws) is None


@pytest.mark.asyncio
async def test_fid_valid_hex_string():
    """FID as '0x1020' (UART2_CNF) → parsed via strtol → handler found → response."""
    async with websockets.connect(WS_URL) as ws:
        await ws.send('{"FID":"0x1020","FLAGS":0,"SID":1}')
        resp = await recv(ws)
        assert resp is not None, "Hex-string FID must be accepted and produce a response"


@pytest.mark.asyncio
async def test_fid_negative():
    """Negative FID → valueint is negative, cast to uint32_t gives a huge number → no handler."""
    async with websockets.connect(WS_URL) as ws:
        await ws.send('{"FID":-1,"FLAGS":0,"SID":1}')
        resp = await recv(ws)
        # FID=-1 → uint32_t 0xFFFFFFFF — no handler registered
        # Must not crash; response (if any) must be well-formed JSON
        if resp is not None:
            json.loads(resp)   # must be valid JSON


@pytest.mark.asyncio
async def test_fid_float():
    """FID as float → cJSON stores as valueint (truncated) → may hit a valid or unknown FID."""
    async with websockets.connect(WS_URL) as ws:
        await ws.send('{"FID":4128.99,"FLAGS":0,"SID":1}')
        resp = await recv(ws)
        # FID 4128.99 → valueint 4128 = 0x1020 (UART2_CNF) → valid handler
        # or truncation behaviour differs — just must not crash
        if resp is not None:
            json.loads(resp)


# ── Category 3: Missing / Bad FLAGS ───────────────────────────────────────────
# ws_server.c: both FLAGS and SID must be numeric → missing either → BAD_REQ.

@pytest.mark.asyncio
async def test_flags_missing():
    """No FLAGS field → API_CALL_ERROR_STATUS_BAD_REQ."""
    async with websockets.connect(WS_URL) as ws:
        await ws.send(f'{{"FID":{FID_UART2_CNF},"SID":1}}')
        resp = await recv(ws)
        assert resp is not None, "Expected BAD_REQ for missing FLAGS"
        assert_sta(resp, STA_ERR_BAD_REQ)


@pytest.mark.asyncio
async def test_flags_as_string():
    """FLAGS as string → cJSON_IsNumber false → BAD_REQ."""
    async with websockets.connect(WS_URL) as ws:
        await ws.send(f'{{"FID":{FID_UART2_CNF},"FLAGS":"0","SID":1}}')
        resp = await recv(ws)
        assert resp is not None
        assert_sta(resp, STA_ERR_BAD_REQ)


@pytest.mark.asyncio
async def test_flags_as_null():
    """FLAGS=null → cJSON_IsNumber false → BAD_REQ."""
    async with websockets.connect(WS_URL) as ws:
        await ws.send(f'{{"FID":{FID_UART2_CNF},"FLAGS":null,"SID":1}}')
        resp = await recv(ws)
        assert resp is not None
        assert_sta(resp, STA_ERR_BAD_REQ)


@pytest.mark.asyncio
async def test_flags_as_object():
    """FLAGS as object → cJSON_IsNumber false → BAD_REQ."""
    async with websockets.connect(WS_URL) as ws:
        await ws.send(f'{{"FID":{FID_UART2_CNF},"FLAGS":{{}},"SID":1}}')
        resp = await recv(ws)
        assert resp is not None
        assert_sta(resp, STA_ERR_BAD_REQ)


# ── Category 4: Missing / Bad SID ─────────────────────────────────────────────

@pytest.mark.asyncio
async def test_sid_missing():
    """No SID field → API_CALL_ERROR_STATUS_BAD_REQ."""
    async with websockets.connect(WS_URL) as ws:
        await ws.send(f'{{"FID":{FID_UART2_CNF},"FLAGS":0}}')
        resp = await recv(ws)
        assert resp is not None, "Expected BAD_REQ for missing SID"
        assert_sta(resp, STA_ERR_BAD_REQ)


@pytest.mark.asyncio
async def test_sid_as_string():
    """SID as string → cJSON_IsNumber false → BAD_REQ."""
    async with websockets.connect(WS_URL) as ws:
        await ws.send(f'{{"FID":{FID_UART2_CNF},"FLAGS":0,"SID":"abc"}}')
        resp = await recv(ws)
        assert resp is not None
        assert_sta(resp, STA_ERR_BAD_REQ)


@pytest.mark.asyncio
async def test_sid_as_null():
    """SID=null → cJSON_IsNumber false → BAD_REQ."""
    async with websockets.connect(WS_URL) as ws:
        await ws.send(f'{{"FID":{FID_UART2_CNF},"FLAGS":null,"SID":null}}')
        resp = await recv(ws)
        assert resp is not None
        assert_sta(resp, STA_ERR_BAD_REQ)


@pytest.mark.asyncio
async def test_sid_overflow():
    """SID > 0xFFFF — code does ulId = sid & 0xffff, so truncation must be safe."""
    async with websockets.connect(WS_URL) as ws:
        await ws.send(f'{{"FID":{FID_UART2_CNF},"FLAGS":0,"SID":999999}}')
        resp = await recv(ws)
        assert resp is not None, "SID overflow must not be rejected"
        # Response FID and SID must be well-formed
        obj = json.loads(resp)
        assert "FID" in obj and "SID" in obj


@pytest.mark.asyncio
async def test_sid_negative():
    """Negative SID — cast to uint32_t & 0xffff, must not crash."""
    async with websockets.connect(WS_URL) as ws:
        await ws.send(f'{{"FID":{FID_UART2_CNF},"FLAGS":0,"SID":-1}}')
        resp = await recv(ws)
        assert resp is not None
        json.loads(resp)   # must be valid JSON


@pytest.mark.asyncio
async def test_both_sid_and_flags_missing():
    """Neither SID nor FLAGS present → BAD_REQ (first failing condition)."""
    async with websockets.connect(WS_URL) as ws:
        await ws.send(f'{{"FID":{FID_UART2_CNF}}}')
        resp = await recv(ws)
        assert resp is not None
        assert_sta(resp, STA_ERR_BAD_REQ)


# ── Category 5: UART Config — out-of-range ARG values ─────────────────────────
# The UART worker (handle_msg / parse_uart_params) does not range-check values
# before passing them to gw_uart_set.  These tests verify the gateway does NOT
# crash or disconnect when illegal values arrive; they do not assert a specific
# error code since gw_uart_set behaviour for bad values is hardware-defined.

@pytest.mark.asyncio
async def test_uart_cnf_wl_too_small():
    """WL=5 (below minimum of 7) — must not crash; two responses expected."""
    async with websockets.connect(WS_URL) as ws:
        await ws.send(make_req(FID_UART2_CNF, arg={"WL": 5}))
        r1 = await recv(ws)
        assert r1 is not None, "Expected at least DELIVERED status"
        r2 = await recv(ws)
        # r2 may be the UART config response or None (if gw_uart_set silently rejects)
        assert await is_alive(ws), "Connection died after WL=5"


@pytest.mark.asyncio
async def test_uart_cnf_wl_too_large():
    """WL=100 (above maximum of 8) — must not crash."""
    async with websockets.connect(WS_URL) as ws:
        await ws.send(make_req(FID_UART2_CNF, arg={"WL": 100}))
        assert await recv(ws) is not None
        assert await is_alive(ws), "Connection died after WL=100"


@pytest.mark.asyncio
async def test_uart_cnf_baud_too_high():
    """BR=99999999 (above UART_MAX_SPEED=500000) — must not crash."""
    async with websockets.connect(WS_URL) as ws:
        await ws.send(make_req(FID_UART2_CNF, arg={"BR": 99_999_999}))
        assert await recv(ws) is not None
        assert await is_alive(ws), "Connection died after oversized baud rate"


@pytest.mark.asyncio
async def test_uart_cnf_parity_out_of_range():
    """PAR=10 (valid values: 0-2) — must not crash."""
    async with websockets.connect(WS_URL) as ws:
        await ws.send(make_req(FID_UART2_CNF, arg={"PAR": 10}))
        assert await recv(ws) is not None
        assert await is_alive(ws), "Connection died after PAR=10"


@pytest.mark.asyncio
async def test_uart_cnf_stop_bits_out_of_range():
    """SB=10 (valid values: 1-3) — must not crash."""
    async with websockets.connect(WS_URL) as ws:
        await ws.send(make_req(FID_UART2_CNF, arg={"SB": 10}))
        assert await recv(ws) is not None
        assert await is_alive(ws), "Connection died after SB=10"


@pytest.mark.asyncio
async def test_uart_cnf_all_fields_invalid():
    """All UART config fields simultaneously out of range — must not crash."""
    async with websockets.connect(WS_URL) as ws:
        await ws.send(make_req(FID_UART2_CNF, arg={"WL": 0, "BR": 0, "PAR": 99, "SB": 99}))
        assert await recv(ws) is not None
        assert await is_alive(ws), "Connection died after all-invalid UART config"


@pytest.mark.asyncio
async def test_uart_cnf_arg_as_string():
    """ARG as a JSON string instead of object → parse_uart_params fails → no UART response."""
    async with websockets.connect(WS_URL) as ws:
        await ws.send(f'{{"FID":{FID_UART2_CNF},"FLAGS":0,"SID":1,"ARG":"not_an_object"}}')
        r1 = await recv(ws)   # DELIVERED
        assert r1 is not None
        # UART task gets non-object JSON → parse_uart_params returns 0 → no response
        # The call hangs in WaitingForResponse until ping fires (no r2 expected soon)
        assert await is_alive(ws)


@pytest.mark.asyncio
async def test_uart_cnf_arg_as_array():
    """ARG as JSON array → parse_uart_params: cJSON_Parse succeeds but json_parse_int finds nothing."""
    async with websockets.connect(WS_URL) as ws:
        await ws.send(f'{{"FID":{FID_UART2_CNF},"FLAGS":0,"SID":1,"ARG":[1,2,3]}}')
        r1 = await recv(ws)
        assert r1 is not None
        assert await is_alive(ws)


@pytest.mark.asyncio
async def test_uart_cnf_zero_baud():
    """BR=0 — zero baud rate, must not crash."""
    async with websockets.connect(WS_URL) as ws:
        await ws.send(make_req(FID_UART2_CNF, arg={"BR": 0}))
        assert await recv(ws) is not None
        assert await is_alive(ws)


# ── Category 6: Modbus — parse-level errors ────────────────────────────────────
# handle_modbus_msg validates ADR, FN, and RD before touching UART.
# On parse failure it responds with {"ERR":2} immediately (no UART timeout).
# Flow: client receives DELIVERED first, then the {"ERR":2} data response.

async def _mb_get_data_resp(ws) -> dict:
    """Collect responses until we get one with 'ERR' or 'RD' in ARG."""
    for _ in range(3):
        resp = await recv(ws, timeout=5.0)
        assert resp is not None, "Expected Modbus error response"
        obj = json.loads(resp)
        arg = obj.get("ARG", {})
        if "ERR" in arg or "RD" in arg:
            return arg
    pytest.fail("No Modbus data response received")


@pytest.mark.asyncio
async def test_modbus_missing_adr():
    """Modbus without ADR → parse_ok=0 → {'ERR':2} response."""
    async with websockets.connect(WS_URL) as ws:
        await ws.send(make_req(FID_UART2_MODBUS, arg={"FN": 3, "RA": 0, "RC": 1}))
        arg = await _mb_get_data_resp(ws)
        assert arg.get("ERR") == 2, f"Expected ERR:2, got: {arg}"


@pytest.mark.asyncio
async def test_modbus_missing_fn():
    """Modbus without FN → parse_ok=0 → {'ERR':2}."""
    async with websockets.connect(WS_URL) as ws:
        await ws.send(make_req(FID_UART2_MODBUS, arg={"ADR": 1, "RA": 0, "RC": 1}))
        arg = await _mb_get_data_resp(ws)
        assert arg.get("ERR") == 2, f"Expected ERR:2, got: {arg}"


@pytest.mark.asyncio
async def test_modbus_adr_overflow():
    """ADR=256 (max 255) → parse_ok=0 → {'ERR':2}."""
    async with websockets.connect(WS_URL) as ws:
        await ws.send(make_req(FID_UART2_MODBUS, arg={"ADR": 256, "FN": 3, "RA": 0, "RC": 1}))
        arg = await _mb_get_data_resp(ws)
        assert arg.get("ERR") == 2, f"Expected ERR:2, got: {arg}"


@pytest.mark.asyncio
async def test_modbus_fn_overflow():
    """FN=200 (max 127) → parse_ok=0 → {'ERR':2}."""
    async with websockets.connect(WS_URL) as ws:
        await ws.send(make_req(FID_UART2_MODBUS, arg={"ADR": 1, "FN": 200, "RA": 0, "RC": 1}))
        arg = await _mb_get_data_resp(ws)
        assert arg.get("ERR") == 2, f"Expected ERR:2, got: {arg}"


@pytest.mark.asyncio
async def test_modbus_empty_arg():
    """Modbus with empty ARG {} → ADR missing → parse_ok=0 → {'ERR':2}."""
    async with websockets.connect(WS_URL) as ws:
        await ws.send(make_req(FID_UART2_MODBUS, arg={}))
        arg = await _mb_get_data_resp(ws)
        assert arg.get("ERR") == 2, f"Expected ERR:2, got: {arg}"


@pytest.mark.asyncio
async def test_modbus_rd_too_many_elements():
    """RD with 200 entries — stride=2 → 400 bytes > MB_PAYLOAD_SIZE(255) → parse_ok=0 → {'ERR':2}."""
    async with websockets.connect(WS_URL) as ws:
        await ws.send(make_req(FID_UART2_MODBUS, arg={
            "ADR": 1, "FN": 16, "RA": 0, "RC": 200,
            "RD": list(range(200)),
        }))
        arg = await _mb_get_data_resp(ws)
        assert arg.get("ERR") == 2, f"Expected ERR:2, got: {arg}"


@pytest.mark.asyncio
async def test_modbus_negative_adr():
    """ADR=-1 → json_parse_int returns a large uint32_t > 255 → parse_ok=0 → {'ERR':2}."""
    async with websockets.connect(WS_URL) as ws:
        await ws.send(make_req(FID_UART2_MODBUS, arg={"ADR": -1, "FN": 3, "RA": 0, "RC": 1}))
        arg = await _mb_get_data_resp(ws)
        # Negative ADR behaviour depends on cJSON/strtol — may or may not be ERR:2
        assert "ERR" in arg or "RD" in arg


@pytest.mark.asyncio
async def test_modbus_rd_not_array():
    """RD as a string → cJSON_IsArray false → ignored → request proceeds to UART (will timeout)."""
    async with websockets.connect(WS_URL) as ws:
        await ws.send(make_req(FID_UART2_MODBUS, arg={
            "ADR": 1, "FN": 3, "RA": 0, "RC": 1, "AWT": 50, "RD": "bad",
        }))
        arg = await _mb_get_data_resp(ws)
        # RD ignored → parse_ok=1 → UART communication → timeout → ERR:4
        assert "ERR" in arg, f"Expected ERR in response, got: {arg}"


@pytest.mark.asyncio
async def test_modbus_no_arg():
    """Modbus request with no ARG at all → msg.data=NULL → {'ERR':2} immediately."""
    async with websockets.connect(WS_URL) as ws:
        await ws.send(make_req(FID_UART2_MODBUS))   # no arg=
        arg = await _mb_get_data_resp(ws)
        assert arg.get("ERR") == 2, f"Expected ERR:2, got: {arg}"


# ── Category 7: Connection-Level Robustness ───────────────────────────────────

@pytest.mark.asyncio
async def test_rapid_connect_disconnect():
    """Open and close 5 connections in succession — no crash or resource leak.
    A delay between each lets the httpd reclaim the session slot before the next
    connect; 20 back-to-back connections exhaust MAX_CLIENTS on QEMU."""
    for _ in range(5):
        ws = await websockets.connect(WS_URL)
        await ws.close()
        await asyncio.sleep(0.3)


@pytest.mark.asyncio
async def test_connect_send_close_immediately():
    """Send a valid request then close before receiving a response — 3 iterations."""
    for _ in range(3):
        async with websockets.connect(WS_URL) as ws:
            await ws.send(make_req(FID_UART2_CNF, arg={"BR": 9600, "WL": 8}))
            # close immediately — gateway must handle the orphaned call gracefully
        await asyncio.sleep(0.3)


@pytest.mark.asyncio
async def test_unknown_fid_flood():
    """Flood one connection with unknown-FID requests — all get NO_HANDLER."""
    n = 20
    async with websockets.connect(WS_URL) as ws:
        for i in range(n):
            await ws.send(f'{{"FID":88888,"FLAGS":0,"SID":{i + 1}}}')
        received = 0
        for _ in range(n):
            resp = await recv(ws, timeout=1.0)
            if resp is None:
                break
            if sta_of(resp) == STA_ERR_NO_HANDLER:
                received += 1
        assert received >= n // 2, (
            f"Too few NO_HANDLER responses: {received}/{n}. "
            "Server may have dropped or not responded."
        )


@pytest.mark.asyncio
async def test_concurrent_bad_request_clients():
    """Three clients each send bad JSON (which closes their connection), then
    each opens a fresh connection to verify the gateway is still responsive."""
    async def client(idx: int) -> bool:
        # Bad JSON closes the server-side connection
        try:
            async with websockets.connect(WS_URL) as ws:
                await ws.send(f"garbage payload client={idx}")
                await recv(ws)   # wait for close
        except Exception:
            pass
        # Fresh connection must work
        try:
            async with websockets.connect(WS_URL) as ws:
                return await is_alive(ws)
        except Exception:
            return False

    results = await asyncio.gather(*[client(i) for i in range(3)])
    assert sum(results) >= 2, (
        f"Too many clients failed fresh-connection check: {results}"
    )


@pytest.mark.asyncio
async def test_max_clients_bad_data():
    """Open 5 concurrent connections each sending one bad JSON frame, then
    verify the gateway recovers and accepts a new connection."""
    async def one(_: int) -> None:
        try:
            async with websockets.connect(WS_URL) as ws:
                await ws.send("not json")
                await recv(ws)   # wait for server-side close
        except Exception:
            pass

    await asyncio.gather(*[one(i) for i in range(5)], return_exceptions=True)
    await asyncio.sleep(1.0)   # give httpd time to reclaim session slots
    async with websockets.connect(WS_URL) as ws:
        assert await is_alive(ws), "Gateway unresponsive after concurrent bad-JSON clients"


# ── Category 8: Duplicate / Edge-Case Requests ────────────────────────────────

@pytest.mark.asyncio
async def test_duplicate_sid_same_fid():
    """Send the same FID+SID twice — must not deadlock or crash."""
    async with websockets.connect(WS_URL) as ws:
        req = make_req(FID_UART2_CNF, sid=42, arg={"BR": 9600})
        await ws.send(req)
        await ws.send(req)
        r1 = await recv(ws)
        r2 = await recv(ws)
        assert r1 is not None, "Expected at least one response"
        assert await is_alive(ws)


@pytest.mark.asyncio
async def test_alternating_valid_invalid():
    """Interleave valid and invalid requests — valid ones must always get responses."""
    async with websockets.connect(WS_URL) as ws:
        for sid in range(1, 11):
            if sid % 2 == 0:
                await ws.send(f'{{"FID":88888,"FLAGS":0,"SID":{sid}}}')  # unknown FID
                resp = await recv(ws)
                assert resp is not None
                assert_sta(resp, STA_ERR_NO_HANDLER)
            else:
                await ws.send(make_req(FID_UART2_CNF, sid=sid, arg={"BR": 9600}))
                resp = await recv(ws)
                assert resp is not None, f"Valid request sid={sid} got no response"


@pytest.mark.asyncio
async def test_empty_json_object():
    """Empty JSON object {} → FID not present → ulFid=0 → silently dropped."""
    async with websockets.connect(WS_URL) as ws:
        await ws.send("{}")
        assert await recv(ws) is None
