"""
Integration tests for ws_server.c WebSocket API.

Requires a device running at WS_URL (default ws://192.168.4.1/ws).
Override with the WS_URL environment variable.

Run:
    pip install websockets pytest pytest-asyncio
    pytest test_ws_server.py -v
"""

import asyncio
import json
import os
import pytest
import websockets

WS_URL = os.environ.get("WS_URL", "ws://192.168.4.1/ws")
RECV_TIMEOUT = 5.0

# ── Status codes (from web_api.h) ──────────────────────────────────────────
STA_COMPLETE    = 0x00000000
STA_INVALID     = 0x00000004
STA_BAD_REQ     = 0x80000000
STA_NO_HANDLER  = 0x8000000E

# ── Well-known FIDs ────────────────────────────────────────────────────────
FID_ECHO        = 1000   # 0x3E8  always registered
FID_UNKNOWN     = 0xDEAD # not registered


# ── Helpers ────────────────────────────────────────────────────────────────

async def send_recv(ws, msg: dict, timeout: float = RECV_TIMEOUT) -> dict:
    """Send *msg* as JSON and return the parsed JSON response."""
    await ws.send(json.dumps(msg))
    raw = await asyncio.wait_for(ws.recv(), timeout=timeout)
    return json.loads(raw)


def sta(resp: dict) -> int:
    """Return ARG.STA as an integer from a status response."""
    return int(resp["ARG"]["STA"], 16)


def fid(resp: dict) -> int:
    """Return FID as an integer from a response."""
    return int(resp["FID"], 16)


# ── FLAGS / SID validation ─────────────────────────────────────────────────

@pytest.mark.asyncio
async def test_missing_flags_and_sid_returns_bad_req():
    """Call without FLAGS and SID must be rejected with BAD_REQ (0x80000000)."""
    async with websockets.connect(WS_URL) as ws:
        resp = await send_recv(ws, {"FID": FID_ECHO})
        assert sta(resp) == STA_BAD_REQ


@pytest.mark.asyncio
async def test_missing_flags_returns_bad_req():
    """Call with SID but without FLAGS must be rejected with BAD_REQ."""
    async with websockets.connect(WS_URL) as ws:
        resp = await send_recv(ws, {"FID": FID_ECHO, "SID": 1})
        assert sta(resp) == STA_BAD_REQ


@pytest.mark.asyncio
async def test_missing_sid_returns_bad_req():
    """Call with FLAGS but without SID must be rejected with BAD_REQ."""
    async with websockets.connect(WS_URL) as ws:
        resp = await send_recv(ws, {"FID": FID_ECHO, "FLAGS": 1})
        assert sta(resp) == STA_BAD_REQ


@pytest.mark.asyncio
async def test_bad_req_response_echoes_fid():
    """A BAD_REQ rejection must still carry the original FID in the response."""
    async with websockets.connect(WS_URL) as ws:
        resp = await send_recv(ws, {"FID": FID_ECHO})
        assert fid(resp) == FID_ECHO


# ── FID validation ─────────────────────────────────────────────────────────

@pytest.mark.asyncio
async def test_unknown_fid_returns_no_handler():
    """A valid call (FLAGS+SID present) for an unregistered FID must return NO_HANDLER."""
    async with websockets.connect(WS_URL) as ws:
        resp = await send_recv(ws, {"FID": FID_UNKNOWN, "FLAGS": 1, "SID": 1})
        assert sta(resp) == STA_NO_HANDLER


@pytest.mark.asyncio
async def test_fid_zero_does_not_succeed():
    """FID 0 is reserved; the server closes the connection or sends an error — never a success."""
    async with websockets.connect(WS_URL) as ws:
        await ws.send(json.dumps({"FID": 0, "FLAGS": 1, "SID": 1}))
        try:
            resp = await asyncio.wait_for(ws.recv(), timeout=2.0)
            resp = json.loads(resp)
            assert sta(resp) >= 0x80000000
        except (asyncio.TimeoutError, websockets.exceptions.ConnectionClosedError):
            pass  # Server drops or closes without a frame — both acceptable


# ── Valid calls ────────────────────────────────────────────────────────────

@pytest.mark.asyncio
async def test_valid_call_response_contains_fid():
    """A properly formed call (FLAGS+SID) must produce a response with the matching FID."""
    async with websockets.connect(WS_URL) as ws:
        resp = await send_recv(ws, {"FID": FID_ECHO, "FLAGS": 1, "SID": 42})
        assert fid(resp) == FID_ECHO


@pytest.mark.asyncio
async def test_valid_call_response_has_arg():
    """The response to a valid call must include an ARG object."""
    async with websockets.connect(WS_URL) as ws:
        resp = await send_recv(ws, {"FID": FID_ECHO, "FLAGS": 1, "SID": 42})
        assert "ARG" in resp


@pytest.mark.asyncio
async def test_valid_call_status_is_not_bad_req():
    """A valid call must not be rejected with BAD_REQ."""
    async with websockets.connect(WS_URL) as ws:
        resp = await send_recv(ws, {"FID": FID_ECHO, "FLAGS": 1, "SID": 42})
        assert sta(resp) != STA_BAD_REQ


# ── Robustness ─────────────────────────────────────────────────────────────

@pytest.mark.asyncio
async def test_invalid_json_closes_connection():
    """Server closes the connection when it receives invalid JSON (no clean close frame)."""
    async with websockets.connect(WS_URL) as ws:
        await ws.send("not-json-at-all{{{")
        with pytest.raises((websockets.exceptions.ConnectionClosedError, asyncio.TimeoutError)):
            await asyncio.wait_for(ws.recv(), timeout=2.0)

@pytest.mark.asyncio
async def test_server_accepts_new_connection_after_invalid_json():
    """After a connection was closed by invalid JSON, a fresh connection works normally."""
    # First connection: send invalid JSON, expect it to be closed
    try:
        async with websockets.connect(WS_URL) as ws:
            await ws.send("not-json-at-all{{{")
            await asyncio.wait_for(ws.recv(), timeout=2.0)
    except (websockets.exceptions.ConnectionClosedError, asyncio.TimeoutError):
        pass
    # Second connection: must work fine
    async with websockets.connect(WS_URL) as ws:
        resp = await send_recv(ws, {"FID": FID_ECHO, "FLAGS": 1, "SID": 99})
        assert fid(resp) == FID_ECHO


@pytest.mark.asyncio
async def test_connection_usable_after_bad_req():
    """After a rejected call the same connection must accept a valid one."""
    async with websockets.connect(WS_URL) as ws:
        bad = await send_recv(ws, {"FID": FID_ECHO})
        assert sta(bad) == STA_BAD_REQ

        good = await send_recv(ws, {"FID": FID_ECHO, "FLAGS": 1, "SID": 10})
        assert fid(good) == FID_ECHO


# ── Concurrency ────────────────────────────────────────────────────────────

@pytest.mark.asyncio
async def test_concurrent_clients():
    """N concurrent clients each send a valid call and must each receive a response."""
    N = 5

    async def one_client(sid):
        async with websockets.connect(WS_URL) as ws:
            resp = await send_recv(ws, {"FID": FID_ECHO, "FLAGS": 1, "SID": sid})
            return fid(resp) == FID_ECHO

    results = await asyncio.gather(*[one_client(i) for i in range(1, N + 1)])
    assert all(results)


@pytest.mark.asyncio
async def test_sequential_calls_single_client():
    """One client sends multiple valid calls sequentially; each must receive a response."""
    async with websockets.connect(WS_URL) as ws:
        for sid in range(1, 6):
            resp = await send_recv(ws, {"FID": FID_ECHO, "FLAGS": 1, "SID": sid})
            assert fid(resp) == FID_ECHO


@pytest.mark.asyncio
async def test_mixed_valid_and_invalid_calls():
    """Interleaving invalid and valid calls must produce BAD_REQ then a normal response."""
    async with websockets.connect(WS_URL) as ws:
        for i in range(3):
            bad = await send_recv(ws, {"FID": FID_ECHO, "SID": i})
            assert sta(bad) == STA_BAD_REQ

            good = await send_recv(ws, {"FID": FID_ECHO, "FLAGS": 1, "SID": i})
            assert fid(good) == FID_ECHO
