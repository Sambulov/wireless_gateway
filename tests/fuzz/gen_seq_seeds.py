#!/usr/bin/env python3
"""Generates seed programs for ws_server_seq_fuzz.c into seeds_seq/.

Each seed is a short scenario aimed at a branch of the worker that a single
text frame can't reach. Opcodes must match the enum in ws_server_seq_fuzz.c.
"""
import os

OP_TEXT, OP_STEP, OP_PERIPH, OP_TICK, OP_SEND_RESULT, OP_CONNECT, OP_FRAME, OP_COMPLETE = range(8)
PERIPH_ECHO, PERIPH_EMPTY, PERIPH_WRONG_ID, PERIPH_BROADCAST = range(4)


def text(json, conn=0):
    b = json.encode()
    assert len(b) < 256
    return bytes([OP_TEXT, conn, len(b)]) + b


def step():
    return bytes([OP_STEP])


def periph(mode=PERIPH_ECHO):
    return bytes([OP_PERIPH, mode])


def tick(n):
    return bytes([OP_TICK, n])


def send_result(conn, fail):
    return bytes([OP_SEND_RESULT, conn, 1 if fail else 0])


def toggle(conn):
    return bytes([OP_CONNECT, conn])


def complete(conn, fail):
    return bytes([OP_COMPLETE, conn, 1 if fail else 0])


def frame(conn, type_idx, payload=b""):
    return bytes([OP_FRAME, conn, type_idx, len(payload)]) + payload


SEEDS = {
    # TO_DELETE removes a matching call already waiting for the peripheral
    "delete_waiting": text('{"FID":1,"SID":5,"FLAGS":4}') + step()
        + text('{"FID":1,"SID":5,"FLAGS":8}') + step(),
    # TO_DELETE removes a matching LONG_TERM call parked in pxWsApiCall (RDL)
    "delete_parked": text('{"FID":1,"SID":6,"FLAGS":4,"ARG":{"RDL":5000}}') + step()
        + periph() + step() + text('{"FID":1,"SID":6,"FLAGS":8}') + step(),
    # TO_DELETE queued *before* the call it deletes (same step)
    "delete_before_target": text('{"FID":1,"SID":7,"FLAGS":8}')
        + text('{"FID":1,"SID":7,"FLAGS":4}') + step(),
    # DELIVERED status can't be queued -> CALL_FLAG_NEW stays set
    "delivered_send_fail": text('{"FID":1,"SID":1,"FLAGS":2}')
        + send_result(0, True) + step() + send_result(0, False) + step(),
    # ping fails on an idle session -> TO_DELETE call left without session
    "delete_no_session": text('{"FID":1,"SID":2,"FLAGS":8}')
        + send_result(0, True) + tick(255) + step(),
    # queued send later fails in httpd -> all calls on that fd are broken
    "async_send_fail": text('{"FID":1,"SID":12,"FLAGS":4}') + step()
        + complete(0, True) + periph() + step(),
    # peripheral answers with an id no call has
    "periph_wrong_id": text('{"FID":1,"SID":3,"FLAGS":2,"ARG":{"a":1}}') + step()
        + periph(PERIPH_WRONG_ID) + step(),
    # response for a queue-only FID (call without handler)
    "periph_no_handler": text('{"FID":3,"SID":4,"FLAGS":2,"ARG":{"a":1}}') + step()
        + periph() + step(),
    # broadcast (id 0) to LONG_TERM subscribers on both connections
    "broadcast_two_conns": text('{"FID":1,"SID":8,"FLAGS":4}', 0)
        + text('{"FID":1,"SID":9,"FLAGS":4}', 1) + step()
        + periph(PERIPH_BROADCAST) + step(),
    # client disconnects while its call waits for the peripheral
    "disconnect_while_waiting": text('{"FID":1,"SID":10,"FLAGS":2}') + step()
        + toggle(0) + periph() + step(),
    # idle long enough to ping, ping send fails
    "ping_fail": text('{"FID":1,"SID":11,"FLAGS":4}') + step()
        + send_result(0, True) + tick(255) + step(),
    # control frames
    "frames": frame(0, 2) + frame(0, 3, b"hi") + frame(1, 4) + frame(1, 1, b"\x00"),
    # peripheral queue overflow -> BUSY
    "periph_busy": b"".join(text('{"FID":1,"SID":%d,"FLAGS":%d}' % (20 + i, 2 + 2 * (i & 1))) for i in range(6))
        + step(),
}

out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "seeds_seq")
os.makedirs(out, exist_ok=True)
for name, data in SEEDS.items():
    with open(os.path.join(out, name + ".bin"), "wb") as f:
        f.write(data)
print("wrote %d seeds to %s" % (len(SEEDS), out))
