/*
 * libFuzzer harness: ядро API под потоком событий, а не под одним кадром.
 *
 * ws_server_fuzz.c подаёт один текстовый кадр за вход, поэтому до состояния,
 * которое требует нескольких событий подряд, ему не добраться: снятие вызова,
 * ответ устройства уже ушедшему клиенту, отказ отправки на подтверждении и так
 * далее. Здесь вход — маленькая байт-код программа, каждая операция это одно
 * событие, которое реальная система может выдать:
 *
 *   op              аргументы                событие
 *   --------------  -----------------------  ---------------------------------
 *   OP_TEXT         conn, len, len байт      клиент прислал текстовый кадр
 *   OP_STEP         -                        одна итерация задачи ядра
 *   OP_PERIPH       mode                     устройство отвечает на запрос
 *   OP_TICK         n                        время идёт
 *   OP_SEND_RESULT  conn, err                следующие отправки в conn не проходят
 *   OP_CONNECT      conn                     соединение поднять/закрыть
 *   OP_FRAME        conn, type, len, bytes   управляющий кадр (ping/pong/close)
 *   OP_COMPLETE     conn, err                транспорт завершает отложенную отправку
 *
 * Отправки принимаются транспортом и завершаются только на OP_COMPLETE — это
 * повторяет поведение устройства, где колбэк завершения приходит от httpd.
 * OP_SEND_RESULT ломает саму постановку в очередь (завершения не будет).
 *
 * Недостающие байты читаются как 0, поэтому любой вход — корректная программа.
 *
 * Сервисы те же, что в ws_server_fuzz.c.
 *
 * Сборка: tests/fuzz/build.sh
 * Запуск: tests/fuzz/build/ws_server_seq_fuzzer -max_len=4096 tests/fuzz/seeds_seq/
 */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "web_api.h"
#include "ws_hal.h"
#include "ws_transport.h"
#include "ws_transport_stub.h"
#include "ws_hal_stub.h"

extern void ws_server_init(void);
extern void ws_server_test_worker_step(void);
extern void ws_server_test_reset(void);
extern void *get_ws_worker_queue(void);

#define FUZZ_FID_FULL   "fuzz.full"
#define FUZZ_FID_ALIVE  "fuzz.alive"
#define FUZZ_FID_QUEUE  "fuzz.queue"

#define PERIPH_QUEUE_DEPTH     4
#define CONN_COUNT             2

enum {
    OP_TEXT,
    OP_STEP,
    OP_PERIPH,
    OP_TICK,
    OP_SEND_RESULT,
    OP_CONNECT,
    OP_FRAME,
    OP_COMPLETE,
    OP_COUNT
};

/* Как фальшивое устройство отвечает на запрос, который забрало (аргумент OP_PERIPH). */
enum {
    PERIPH_ECHO,        /* тот же id, полезная нагрузка возвращается как есть */
    PERIPH_EMPTY,       /* тот же id, без данных                             */
    PERIPH_WRONG_ID,    /* id, которого не может быть ни у одного вызова     */
    PERIPH_BROADCAST,   /* id 0 — рассылка всем подписчикам сервиса           */
    PERIPH_MODE_COUNT
};

static ws_transport_stub_conn_t *conns[CONN_COUNT];
static int connected[CONN_COUNT];
static ws_queue_t periph_queue;

/* ── Чтение входа ─────────────────────────────────────────────────────── */

typedef struct {
    const uint8_t *p;
    size_t left;
} reader_t;

static uint8_t rd_u8(reader_t *r) {
    if (!r->left) return 0;
    r->left--;
    return *r->p++;
}

static uint8_t *rd_bytes(reader_t *r, size_t want, size_t *out_len) {
    size_t n = want < r->left ? want : r->left;
    uint8_t *buf = malloc(n ? n : 1);
    if (buf && n) memcpy(buf, r->p, n);
    r->p += n;
    r->left -= n;
    *out_len = n;
    return buf;
}

/* ── Фальшивые концы ──────────────────────────────────────────────────── */

static uint8_t fuzz_done_handler(void *call, void **ctx, uint8_t *data, uint32_t len) {
    (void)ctx; (void)data; (void)len;
    uint32_t id;
    bApiCallGetId(call, &id);
    bApiCallSendStatus(call, API_CALL_STATUS_EXECUTING);
    return 0;
}

static int fuzz_alive_rounds;
static uint8_t fuzz_alive_handler(void *call, void **ctx, uint8_t *data, uint32_t len) {
    (void)ctx; (void)data; (void)len;
    uint32_t id;
    bApiCallGetId(call, &id);
    bApiCallSendStatus(call, API_CALL_STATUS_EXECUTING);
    return (++fuzz_alive_rounds % 8) != 0;
}

static void register_test_endpoints(void) {
    bApiCallRegister(fuzz_done_handler,  FUZZ_FID_FULL,  GW_IFACE_MASK_ANY, NULL);
    ws_server_register_fid_queue(FUZZ_FID_FULL, GW_IFACE_MASK_ANY, periph_queue);
    bApiCallRegister(fuzz_alive_handler, FUZZ_FID_ALIVE, GW_IFACE_MASK_ANY, NULL);
    ws_server_register_fid_queue(FUZZ_FID_QUEUE, GW_IFACE_MASK_ANY, periph_queue);
}

static void periph_answer(uint8_t mode) {
    webapi_msg_t *req;
    if (!ws_hal_queue_receive(periph_queue, &req, WS_HAL_WAIT_NONE))
        return;

    webapi_msg_t resp = {0};
    memcpy(resp.fid, req->fid, sizeof(resp.fid));
    resp.id = req->id;
    switch (mode % PERIPH_MODE_COUNT) {
    case PERIPH_WRONG_ID:  resp.id = 0x10000 | req->id; break;
    case PERIPH_BROADCAST: resp.id = 0;                 break;
    default:                                            break;
    }
    if ((mode % PERIPH_MODE_COUNT) != PERIPH_EMPTY && req->data && req->len) {
        resp.data = malloc(req->len);
        if (resp.data) {
            memcpy(resp.data, req->data, req->len);
            resp.len = req->len;
        }
    }
    if (!ws_hal_queue_send(get_ws_worker_queue(), &resp, WS_HAL_WAIT_NONE))
        free(resp.data);
    free(req->data);
    free(req);
}

static void drain_queues(void) {
    webapi_msg_t *req;
    while (ws_hal_queue_receive(periph_queue, &req, WS_HAL_WAIT_NONE)) {
        free(req->data);
        free(req);
    }
    webapi_msg_t resp;
    while (ws_hal_queue_receive(get_ws_worker_queue(), &resp, WS_HAL_WAIT_NONE))
        free(resp.data);
}

/* Типы кадров уровня WebSocket, которые может подать вход. */
static const ws_transport_stub_frame_t frame_types[] = {
    WS_STUB_FRAME_CONT, WS_STUB_FRAME_TEXT, WS_STUB_FRAME_CLOSE,
    WS_STUB_FRAME_PING, WS_STUB_FRAME_PONG,
};

static void connect_conn(int c, int up) {
    if (up) {
        ws_transport_stub_set_deferred(conns[c], 1);
        if (conns[c] == NULL || connected[c]) return;
        conns[c] = ws_transport_stub_connect(c + 1);
        connected[c] = 1;
    } else {
        ws_transport_stub_disconnect(conns[c]);
        connected[c] = 0;
    }
}

static void rearm(void) {
    ws_server_test_reset();
    ws_transport_stub_reset();
    ws_transport_stub_attach();
    for (int i = 0; i < CONN_COUNT; i++) {
        conns[i] = ws_transport_stub_connect(i + 1);
        ws_transport_stub_set_deferred(conns[i], 1);
        connected[i] = 1;
    }
    register_test_endpoints();
}

/* ── Точки входа libFuzzer ────────────────────────────────────────────── */

int LLVMFuzzerInitialize(int *argc, char ***argv) {
    (void)argc;
    (void)argv;
    ws_server_init();
    periph_queue = ws_hal_queue_create(PERIPH_QUEUE_DEPTH, sizeof(webapi_msg_t *));
    rearm();
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t len) {
    reader_t r = { data, len };

    fuzz_alive_rounds = 0;

    while (r.left) {
        uint8_t op = rd_u8(&r) % OP_COUNT;
        switch (op) {
        case OP_TEXT: {
            int c = rd_u8(&r) % CONN_COUNT;
            size_t n;
            uint8_t *buf = rd_bytes(&r, rd_u8(&r), &n);
            if (buf && connected[c])
                ws_transport_stub_frame(conns[c], WS_STUB_FRAME_TEXT, buf, n, 1);
            free(buf);
            break;
        }
        case OP_STEP:
            ws_server_test_worker_step();
            break;
        case OP_PERIPH:
            periph_answer(rd_u8(&r));
            break;
        case OP_TICK:
            ws_hal_stub_advance_tick((uint32_t)rd_u8(&r) * 16);
            break;
        case OP_SEND_RESULT: {
            int c = rd_u8(&r) % CONN_COUNT;
            ws_transport_stub_set_send_result(conns[c],
                (rd_u8(&r) & 1) ? WS_TRANSPORT_FAULT : WS_TRANSPORT_OK);
            break;
        }
        case OP_CONNECT: {
            int c = rd_u8(&r) % CONN_COUNT;
            connect_conn(c, !connected[c]);
            break;
        }
        case OP_FRAME: {
            int c = rd_u8(&r) % CONN_COUNT;
            ws_transport_stub_frame_t t = frame_types[rd_u8(&r) % 5];
            size_t n;
            uint8_t *buf = rd_bytes(&r, rd_u8(&r), &n);
            if (buf && connected[c]) ws_transport_stub_frame(conns[c], t, buf, n, 1);
            free(buf);
            break;
        }
        case OP_COMPLETE: {
            int c = rd_u8(&r) % CONN_COUNT;
            ws_transport_stub_complete_pending(conns[c],
                (rd_u8(&r) & 1) ? WS_TRANSPORT_FAULT : WS_TRANSPORT_OK);
            break;
        }
        default:
            break;
        }
    }

    drain_queues();
    rearm();
    return 0;
}
