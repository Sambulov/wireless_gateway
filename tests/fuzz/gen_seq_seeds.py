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
    # Повтор кадра с тем же SID без ARG — отписка (§1.3, §3.5)
    "cancel_waiting": text('{"FID":"fuzz.full","SID":5,"ARG":{"RDL":0}}') + step()
        + text('{"FID":"fuzz.full","SID":5}') + step(),
    # Отписка после того, как периферия ответила
    "cancel_after_answer": text('{"FID":"fuzz.full","SID":6,"ARG":{"RDL":5000}}') + step()
        + periph() + step() + text('{"FID":"fuzz.full","SID":6}') + step(),
    # Отписка раньше, чем вызов создан: это новый вызов, а не снятие
    "cancel_before_call": text('{"FID":"fuzz.full","SID":7}')
        + text('{"FID":"fuzz.full","SID":7,"ARG":{"RDL":0}}') + step(),
    # Отказ отправки: кадр не ушёл, вызов живёт дальше
    "send_fail_then_ok": text('{"FID":"fuzz.full","SID":1,"ARG":{"a":1}}')
        + send_result(0, True) + step() + send_result(0, False) + step(),
    # Номер живого вызова назван чужим FID — BAD_REQ, вызов не тронут
    "bad_req_foreign_fid": text('{"FID":"fuzz.full","SID":2,"ARG":{}}') + step()
        + text('{"FID":"fuzz.alive","SID":2,"ARG":{}}') + step(),
    # Очередь отправок падает — вызовы этого fd теряют адресата
    "async_send_fail": text('{"FID":"fuzz.full","SID":12,"ARG":{"RDL":0}}') + step()
        + complete(0, True) + periph() + step(),
    # Периферия отвечает на номер, которого ни у одного вызова нет
    "periph_wrong_id": text('{"FID":"fuzz.full","SID":3,"ARG":{"a":1}}') + step()
        + periph(PERIPH_WRONG_ID) + step(),
    # Ответ по FID без обработчика
    "periph_no_handler": text('{"FID":"fuzz.queue","SID":4,"ARG":{"a":1}}') + step()
        + periph() + step(),
    # Публичная рассылка (без SID) двум подписчикам на разных соединениях
    "broadcast_two_conns": text('{"FID":"fuzz.full","SID":8,"ARG":{"RDL":0}}', 0)
        + text('{"FID":"fuzz.full","SID":9,"ARG":{"RDL":0}}', 1) + step()
        + periph(PERIPH_BROADCAST) + step(),
    # Клиент уходит, пока вызов ждёт периферию
    "disconnect_while_waiting": text('{"FID":"fuzz.full","SID":10,"ARG":{"a":1}}') + step()
        + toggle(0) + periph() + step(),
    # Простой, а затем неудачная отправка
    "idle_send_fail": text('{"FID":"fuzz.full","SID":11,"ARG":{"RDL":0}}') + step()
        + send_result(0, True) + tick(255) + step(),
    # Управляющие кадры транспорта
    "frames": frame(0, 2) + frame(0, 3, b"hi") + frame(1, 4) + frame(1, 1, b"\x00"),
    # Очередь периферии переполнена — BUSY
    "periph_busy": b"".join(text('{"FID":"fuzz.full","SID":%d,"ARG":{"a":%d}}' % (20 + i, i)) for i in range(6))
        + step(),
}

out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "seeds_seq")
os.makedirs(out, exist_ok=True)
for name, data in SEEDS.items():
    with open(os.path.join(out, name + ".bin"), "wb") as f:
        f.write(data)
print("wrote %d seeds to %s" % (len(SEEDS), out))
