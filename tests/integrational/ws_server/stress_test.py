"""
Stress tests for ws_server.c WebSocket API.

Exercises the server under high load to catch races, queue exhaustion,
memory leaks, and connection-limit edge cases.

Device limits (from Kconfig.projbuild):
  WEB_SERVER_MAX_CLIENTS      = 10
  WEB_SOCKET_MAX_PENDING_API  = 20

Run:
    pip install websockets pytest pytest-asyncio
    pytest stress_test.py -v -s
    WS_URL=ws://192.168.4.1/ws pytest stress_test.py -v -s
"""

import asyncio
import json
import os
import time
import statistics
from dataclasses import dataclass, field
from typing import List

import pytest
import websockets

WS_URL = os.environ.get("WS_URL", "ws://192.168.4.1/ws")

MAX_CLIENTS         = 10   # WEB_SERVER_MAX_CLIENTS
MAX_PENDING_API     = 20   # WEB_SOCKET_MAX_PENDING_API
RECV_TIMEOUT        = 10.0

FID_ECHO            = 1000
STA_BAD_REQ         = 0x80000000
STA_BUSY            = 0x00000003


# ── Helpers ────────────────────────────────────────────────────────────────

def make_call(sid: int, flags: int = 1) -> str:
    return json.dumps({"FID": FID_ECHO, "FLAGS": flags, "SID": sid & 0xFFFF})


def parse(raw: str) -> dict:
    return json.loads(raw)


def sta(resp: dict) -> int:
    return int(resp["ARG"]["STA"], 16)


def fid(resp: dict) -> int:
    return int(resp["FID"], 16)


@dataclass
class ClientStats:
    sent:       int = 0
    received:   int = 0
    errors:     int = 0
    latencies:  List[float] = field(default_factory=list)

    def ok_rate(self) -> float:
        return self.received / self.sent if self.sent else 0.0

    def avg_latency_ms(self) -> float:
        return statistics.mean(self.latencies) * 1000 if self.latencies else 0.0

    def p99_latency_ms(self) -> float:
        if not self.latencies:
            return 0.0
        s = sorted(self.latencies)
        idx = max(0, int(len(s) * 0.99) - 1)
        return s[idx] * 1000


def _print_summary(label: str, all_stats: List[ClientStats]) -> None:
    total_sent     = sum(s.sent     for s in all_stats)
    total_received = sum(s.received for s in all_stats)
    total_errors   = sum(s.errors   for s in all_stats)
    all_lat        = [l for s in all_stats for l in s.latencies]
    avg = statistics.mean(all_lat) * 1000   if all_lat else 0
    p99 = sorted(all_lat)[max(0, int(len(all_lat) * 0.99) - 1)] * 1000 if all_lat else 0
    print(
        f"\n[{label}] sent={total_sent} recv={total_received} "
        f"err={total_errors} ok={total_received/total_sent*100:.1f}% "
        f"avg={avg:.1f}ms p99={p99:.1f}ms"
    )


# ── Single-client burst ────────────────────────────────────────────────────

async def _burst_client(n_msgs: int, start_sid: int = 0) -> ClientStats:
    """Send *n_msgs* calls as fast as possible and collect stats."""
    s = ClientStats()
    try:
        async with websockets.connect(WS_URL) as ws:
            for i in range(n_msgs):
                sid = (start_sid + i) & 0xFFFF
                t0 = time.perf_counter()
                await ws.send(make_call(sid))
                s.sent += 1
                try:
                    raw = await asyncio.wait_for(ws.recv(), timeout=RECV_TIMEOUT)
                    s.latencies.append(time.perf_counter() - t0)
                    resp = parse(raw)
                    if fid(resp) == FID_ECHO:
                        s.received += 1
                    else:
                        s.errors += 1
                except asyncio.TimeoutError:
                    s.errors += 1
    except Exception:
        s.errors += 1
    return s


# ── Tests ──────────────────────────────────────────────────────────────────

@pytest.mark.asyncio
async def test_burst_single_client_100():
    """One client sends 100 calls back-to-back; success rate must be 100%."""
    s = await _burst_client(100)
    _print_summary("burst_single_100", [s])
    assert s.sent == 100
    assert s.ok_rate() == 1.0, f"ok_rate={s.ok_rate():.2f}"


@pytest.mark.asyncio
async def test_burst_single_client_500():
    """One client sends 500 calls; success rate must be >= 99%."""
    s = await _burst_client(500)
    _print_summary("burst_single_500", [s])
    assert s.ok_rate() >= 0.99, f"ok_rate={s.ok_rate():.2f}"


@pytest.mark.asyncio
async def test_concurrent_clients_at_max():
    """MAX_CLIENTS clients each send 20 calls simultaneously."""
    n_msgs = 20
    tasks = [_burst_client(n_msgs, start_sid=c * n_msgs) for c in range(MAX_CLIENTS)]
    all_stats = await asyncio.gather(*tasks)
    _print_summary(f"concurrent_{MAX_CLIENTS}x{n_msgs}", list(all_stats))
    for i, s in enumerate(all_stats):
        assert s.ok_rate() >= 0.95, f"client {i} ok_rate={s.ok_rate():.2f}"


@pytest.mark.asyncio
async def test_connection_storm():
    """Open MAX_CLIENTS connections simultaneously, send one call each, close all — 5 rounds.

    The server has a finite socket pool; not every connection is guaranteed to
    succeed when opened in a burst.  The critical assertion is that the server
    remains responsive after each round.
    """
    rounds = 5
    total_attempted = 0
    total_ok = 0

    async def one_conn(sid: int) -> bool:
        try:
            async with websockets.connect(WS_URL) as ws:
                await ws.send(make_call(sid))
                raw = await asyncio.wait_for(ws.recv(), timeout=RECV_TIMEOUT)
                return fid(parse(raw)) == FID_ECHO
        except Exception:
            return False

    for r in range(rounds):
        results = await asyncio.gather(
            *[one_conn(r * MAX_CLIENTS + i) for i in range(MAX_CLIENTS)],
            return_exceptions=True,
        )
        ok = sum(1 for res in results if res is True)
        total_attempted += MAX_CLIENTS
        total_ok += ok
        await asyncio.sleep(0.5)   # let the server reclaim sockets between rounds

    ok_rate = total_ok / total_attempted if total_attempted else 0
    print(f"\n[connection_storm] rounds={rounds} attempted={total_attempted} ok={total_ok} rate={ok_rate:.1%}")

    # Server must still respond after the storm
    async with websockets.connect(WS_URL) as ws:
        await ws.send(make_call(0xAA))
        raw = await asyncio.wait_for(ws.recv(), timeout=RECV_TIMEOUT)
        assert fid(parse(raw)) == FID_ECHO, "server not responsive after connection storm"


@pytest.mark.asyncio
async def test_over_max_clients_rejected_gracefully():
    """Opening MAX_CLIENTS+3 connections must not crash the server.

    The extra connections may be refused by the server; that is acceptable.
    What matters is that after they close, a normal client still works.
    """
    extras = MAX_CLIENTS + 3
    conns = await asyncio.gather(
        *[websockets.connect(WS_URL) for _ in range(extras)],
        return_exceptions=True,
    )
    open_conns = [c for c in conns if not isinstance(c, Exception)]
    print(f"\n[over_max] attempted={extras} opened={len(open_conns)}")
    for c in open_conns:
        await c.close()

    await asyncio.sleep(0.2)

    # Server must still be responsive
    async with websockets.connect(WS_URL) as ws:
        await ws.send(make_call(1))
        raw = await asyncio.wait_for(ws.recv(), timeout=RECV_TIMEOUT)
        assert fid(parse(raw)) == FID_ECHO


@pytest.mark.asyncio
async def test_queue_saturation():
    """Flood the server with MAX_PENDING_API+10 rapid calls from one client.

    Some may get STA_BUSY; the server must not crash and subsequent calls
    must eventually succeed.
    """
    n = MAX_PENDING_API + 10
    busy_count = 0
    ok_count = 0

    async with websockets.connect(WS_URL) as ws:
        # Fire all at once without waiting for responses
        for i in range(n):
            await ws.send(make_call(i))

        # Drain responses
        for _ in range(n):
            try:
                raw = await asyncio.wait_for(ws.recv(), timeout=RECV_TIMEOUT)
                resp = parse(raw)
                if sta(resp) == STA_BUSY:
                    busy_count += 1
                else:
                    ok_count += 1
            except asyncio.TimeoutError:
                break

        print(f"\n[queue_sat] sent={n} ok={ok_count} busy={busy_count}")
        # At least some calls must succeed
        assert ok_count > 0

        # Server must still respond after the flood
        await ws.send(make_call(0xAB))
        raw = await asyncio.wait_for(ws.recv(), timeout=RECV_TIMEOUT)
        assert fid(parse(raw)) == FID_ECHO


@pytest.mark.asyncio
async def test_sustained_load_10s():
    """Run MAX_CLIENTS clients continuously for 10 seconds and report throughput."""
    duration = 10.0
    all_stats: List[ClientStats] = []

    async def sustained_client(client_id: int) -> ClientStats:
        s = ClientStats()
        sid = 0
        deadline = time.perf_counter() + duration
        try:
            async with websockets.connect(WS_URL) as ws:
                while time.perf_counter() < deadline:
                    t0 = time.perf_counter()
                    await ws.send(make_call(sid, flags=1))
                    s.sent += 1
                    sid = (sid + 1) & 0xFFFF
                    try:
                        raw = await asyncio.wait_for(ws.recv(), timeout=RECV_TIMEOUT)
                        s.latencies.append(time.perf_counter() - t0)
                        if fid(parse(raw)) == FID_ECHO:
                            s.received += 1
                        else:
                            s.errors += 1
                    except asyncio.TimeoutError:
                        s.errors += 1
        except Exception:
            s.errors += 1
        return s

    tasks = [sustained_client(i) for i in range(MAX_CLIENTS)]
    all_stats = list(await asyncio.gather(*tasks))
    _print_summary(f"sustained_{duration:.0f}s_{MAX_CLIENTS}clients", all_stats)

    total_sent = sum(s.sent for s in all_stats)
    total_ok   = sum(s.received for s in all_stats)
    throughput = total_ok / duration
    print(f"  throughput = {throughput:.1f} msg/s")

    global_ok_rate = total_ok / total_sent if total_sent else 0
    assert global_ok_rate >= 0.95, f"global ok_rate={global_ok_rate:.2f}"


@pytest.mark.asyncio
async def test_rapid_reconnect():
    """One client connects, sends one call, disconnects — repeated 50 times.

    Verifies that the server reclaims session memory correctly.
    """
    n = 50
    failures = 0
    for i in range(n):
        try:
            async with websockets.connect(WS_URL) as ws:
                await ws.send(make_call(i & 0xFFFF))
                raw = await asyncio.wait_for(ws.recv(), timeout=RECV_TIMEOUT)
                if fid(parse(raw)) != FID_ECHO:
                    failures += 1
        except Exception:
            failures += 1
        # No sleep — stress the session allocation path

    print(f"\n[rapid_reconnect] n={n} failures={failures}")
    assert failures == 0, f"{failures}/{n} reconnects failed"


@pytest.mark.asyncio
async def test_mixed_valid_invalid_under_load():
    """Multiple clients interleave valid and invalid calls under concurrent load.

    Invalid calls must return BAD_REQ; valid calls must succeed.
    """
    n_clients = 5
    n_rounds  = 20

    async def mixed_client(client_id: int) -> ClientStats:
        s = ClientStats()
        try:
            async with websockets.connect(WS_URL) as ws:
                for i in range(n_rounds):
                    if i % 3 == 0:
                        # Invalid: missing FLAGS
                        await ws.send(json.dumps({"FID": FID_ECHO, "SID": i}))
                        s.sent += 1
                        try:
                            raw = await asyncio.wait_for(ws.recv(), timeout=RECV_TIMEOUT)
                            if sta(parse(raw)) == STA_BAD_REQ:
                                s.received += 1
                            else:
                                s.errors += 1
                        except asyncio.TimeoutError:
                            s.errors += 1
                    else:
                        # Valid
                        t0 = time.perf_counter()
                        await ws.send(make_call(i))
                        s.sent += 1
                        try:
                            raw = await asyncio.wait_for(ws.recv(), timeout=RECV_TIMEOUT)
                            s.latencies.append(time.perf_counter() - t0)
                            if fid(parse(raw)) == FID_ECHO:
                                s.received += 1
                            else:
                                s.errors += 1
                        except asyncio.TimeoutError:
                            s.errors += 1
        except Exception:
            s.errors += 1
        return s

    all_stats = list(await asyncio.gather(*[mixed_client(i) for i in range(n_clients)]))
    _print_summary(f"mixed_{n_clients}clients_{n_rounds}rounds", all_stats)
    for i, s in enumerate(all_stats):
        assert s.ok_rate() >= 0.95, f"client {i} ok_rate={s.ok_rate():.2f}"
