"""
Innovert latency test.

Measures round-trip latency of Modbus FC3 requests routed through the
wireless gateway WebSocket API to the virtual Innovert device on UART2.

Prerequisites:
  - Gateway running at GW_URL (default: http://localhost:8080)
  - Virtual Innovert device connected to UART2
  - pip install pytest pytest-asyncio websockets requests

Run:
  pytest test_latency.py -v -s
"""

import asyncio
import json
import time
import statistics

import pytest
import requests
import websockets

# ── Configuration ─────────────────────────────────────────────
GW_HTTP = "http://localhost:8080"
GW_WS   = "ws://localhost:8080/ws"

MB_ADDR     = 1
FID_UART2   = 0x1123   # UART2 Modbus FID
READ_REG    = 0        # first status register block
READ_COUNT  = 6

N_SAMPLES       = 50   # requests per test run
WARMUP_SAMPLES  = 5    # discarded from statistics
PAGE_TIMEOUT_S  = 30   # max seconds to wait for page to become reachable
PAGE_POLL_S     = 0.5

# Thresholds are intentionally loose — QEMU adds ~1-2 s of emulation overhead.
# On real hardware expect mean ~50 ms, p95 ~150 ms.
LATENCY_MEAN_MS = 5000
LATENCY_P95_MS  = 8000
LATENCY_CONCURRENT_MEAN_MS = 5000

STRESS_CLIENTS  = 5     # concurrent WebSocket clients
STRESS_DURATION = 30    # seconds
STRESS_MAX_ERR_RATE  = 0.05   # max 5% errors
STRESS_MIN_THROUGHPUT = 0.5   # req/s total across all clients (QEMU is slow)

# ── Helpers ───────────────────────────────────────────────────

def wait_for_page(url: str, timeout: float = PAGE_TIMEOUT_S) -> float:
    """Block until GET url returns 200 or timeout expires. Returns elapsed seconds."""
    t0 = time.monotonic()
    deadline = t0 + timeout
    last_exc = None
    while time.monotonic() < deadline:
        try:
            r = requests.get(url, timeout=2)
            if r.status_code == 200:
                return time.monotonic() - t0
            last_exc = f"HTTP {r.status_code}"
        except requests.exceptions.RequestException as e:
            last_exc = str(e)
        time.sleep(PAGE_POLL_S)
    raise TimeoutError(
        f"Page {url} not reachable after {timeout}s — last error: {last_exc}"
    )


def make_request(sid: int) -> str:
    return json.dumps({
        "FID":   FID_UART2,
        "FLAGS": 0,
        "SID":   sid,
        "ARG":   {"ADR": MB_ADDR, "FN": 3, "RA": READ_REG, "RC": READ_COUNT},
    })


async def recv_skipping_pings(ws) -> str:
    """Receive next non-ping data frame, sending pong for any ping received."""
    while True:
        msg = await ws.recv()
        if isinstance(msg, bytes):
            continue
        return msg


async def send_and_measure(ws, sid: int) -> float:
    """
    Send one Modbus FC3 request and return the latency in seconds.

    The gateway sends two messages per request:
      1. ACK  — contains STA field only
      2. Data — contains the actual register values
    We time from send to the data response.
    """
    req = make_request(sid)
    t0 = time.perf_counter()
    await ws.send(req)

    # first message: ACK
    await recv_skipping_pings(ws)
    # second message: data
    data_raw = await recv_skipping_pings(ws)
    t1 = time.perf_counter()

    data = json.loads(data_raw)
    assert "RD" in data["ARG"], f"unexpected response: {data_raw}"
    return t1 - t0


def print_stats(latencies: list[float]) -> None:
    ms = [v * 1000 for v in latencies]
    print(f"\n  samples : {len(ms)}")
    print(f"  min     : {min(ms):.1f} ms")
    print(f"  max     : {max(ms):.1f} ms")
    print(f"  mean    : {statistics.mean(ms):.1f} ms")
    print(f"  median  : {statistics.median(ms):.1f} ms")
    print(f"  p95     : {sorted(ms)[int(len(ms) * 0.95)]:.1f} ms")
    print(f"  stdev   : {statistics.stdev(ms):.1f} ms")


# ── Tests ─────────────────────────────────────────────────────

def test_page_reachable():
    """Page must load before any WS tests run."""
    elapsed = wait_for_page(f"{GW_HTTP}/innovert.html")
    print(f"\n  page reachable in {elapsed * 1000:.0f} ms")


@pytest.mark.asyncio
async def test_modbus_latency():
    """Measure end-to-end latency for N Modbus FC3 reads via WebSocket."""
    wait_for_page(f"{GW_HTTP}/innovert.html")

    latencies: list[float] = []
    async with websockets.connect(GW_WS) as ws:
        for i in range(N_SAMPLES + WARMUP_SAMPLES):
            lat = await send_and_measure(ws, sid=i + 1)
            if i >= WARMUP_SAMPLES:
                latencies.append(lat)

    print_stats(latencies)

    mean_ms = statistics.mean(latencies) * 1000
    p95_ms  = sorted(latencies)[int(len(latencies) * 0.95)] * 1000
    assert mean_ms < LATENCY_MEAN_MS, f"mean latency too high: {mean_ms:.1f} ms"
    assert p95_ms  < LATENCY_P95_MS,  f"p95 latency too high: {p95_ms:.1f} ms"


@pytest.mark.asyncio
async def test_modbus_latency_concurrent():
    """Two concurrent WS clients both reading Innovert — checks for interference."""
    wait_for_page(f"{GW_HTTP}/innovert.html")

    async def client(client_id: int) -> list[float]:
        lats = []
        async with websockets.connect(GW_WS) as ws:
            for i in range(20):
                lat = await send_and_measure(ws, sid=client_id * 1000 + i)
                lats.append(lat)
        return lats

    results = await asyncio.gather(client(1), client(2))
    for lats in results:
        print_stats(lats)
        mean_ms = statistics.mean(lats) * 1000
        assert mean_ms < LATENCY_CONCURRENT_MEAN_MS, f"mean latency under load too high: {mean_ms:.1f} ms"


@pytest.mark.asyncio
async def test_modbus_stress():
    """
    Stress test: STRESS_CLIENTS concurrent WebSocket clients each sending
    Modbus FC3 requests as fast as possible for STRESS_DURATION seconds.
    Reports throughput, error rate, and latency percentiles.
    """
    wait_for_page(f"{GW_HTTP}/innovert.html")

    async def stress_client(client_id: int, stop_event: asyncio.Event):
        latencies = []
        errors = 0
        sid_base = client_id * 100000
        req_num = 0
        try:
            async with websockets.connect(GW_WS) as ws:
                while not stop_event.is_set():
                    try:
                        lat = await send_and_measure(ws, sid=sid_base + req_num)
                        latencies.append(lat)
                    except Exception as e:
                        errors += 1
                        print(f"\n  [client {client_id}] req error: {type(e).__name__}: {e}")
                    req_num += 1
        except Exception as e:
            errors += 1
            print(f"\n  [client {client_id}] connect error: {type(e).__name__}: {e}")
        return latencies, errors

    stop = asyncio.Event()
    loop = asyncio.get_event_loop()
    loop.call_later(STRESS_DURATION, stop.set)

    tasks = [stress_client(i, stop) for i in range(STRESS_CLIENTS)]
    results = await asyncio.gather(*tasks)

    all_latencies = []
    total_errors = 0
    for lats, errs in results:
        all_latencies.extend(lats)
        total_errors += errs

    total_requests = len(all_latencies) + total_errors
    error_rate = total_errors / total_requests if total_requests > 0 else 1.0
    throughput = len(all_latencies) / STRESS_DURATION

    print(f"\n  duration    : {STRESS_DURATION} s")
    print(f"  clients     : {STRESS_CLIENTS}")
    print(f"  total req   : {total_requests}")
    print(f"  errors      : {total_errors}  ({error_rate*100:.1f}%)")
    print(f"  throughput  : {throughput:.2f} req/s")
    if all_latencies:
        print_stats(all_latencies)

    assert error_rate <= STRESS_MAX_ERR_RATE, \
        f"error rate too high: {error_rate*100:.1f}% ({total_errors}/{total_requests})"
    assert throughput >= STRESS_MIN_THROUGHPUT, \
        f"throughput too low: {throughput:.2f} req/s"
