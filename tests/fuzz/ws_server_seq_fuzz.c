/*
 * libFuzzer harness: ws_server driven by a *sequence* of events.
 *
 * ws_server_fuzz.c feeds one text frame per input, which can't reach state
 * that needs several events in a row (a TO_DELETE for a call that is still
 * pending, a response arriving after the client went away, a send failure
 * on the DELIVERED status, ...). Here the input is a little bytecode
 * program; each op is one event the real system can produce:
 *
 *   op % OP_COUNT   args                     event
 *   ------------    ----------------------   -----------------------------------
 *   OP_TEXT         conn, len, len bytes     client sends a text frame
 *   OP_STEP         -                        one worker iteration
 *   OP_PERIPH       mode                     peripheral answers one request
 *   OP_TICK         n                        time passes (n * 16 ticks)
 *   OP_SEND_RESULT  conn, err                next sends on conn fail / succeed
 *   OP_CONNECT      conn                     toggle connect / disconnect
 *   OP_FRAME        conn, type, len, bytes   non-text frame (ping/pong/close/..)
 *   OP_COMPLETE     conn, err                httpd finishes queued sends on conn
 *
 * Sends are accepted into the (stubbed) httpd queue and their completion
 * callbacks only run on OP_COMPLETE — never in the middle of a worker step,
 * which matches the target where the callback runs on the httpd task.
 * OP_SEND_RESULT makes the *queuing* itself fail (callback never runs).
 *
 * Missing trailing bytes read as 0, so every input is a valid program.
 *
 * FIDs are the same as in ws_server_fuzz.c:
 *   FID 1 — handler + peripheral queue
 *   FID 2 — handler only, no queue
 *   FID 3 — peripheral queue only
 *
 * Build: tests/fuzz/build.sh
 * Run:   tests/fuzz/build/ws_server_seq_fuzzer -max_len=4096 corpus_seq/ seeds_seq/
 */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "web_api.h"
#include "ws_hal.h"
#include "ws_transport.h"
#include "ws_conn_stub.h"
#include "ws_hal_stub.h"

extern void ws_server_init(void);
extern void ws_server_on_connect(ws_conn_t *conn);
extern void ws_server_on_disconnect(ws_conn_t *conn);
extern void ws_server_on_frame(ws_conn_t *conn, ws_frame_type_t type,
                                uint8_t *data, size_t len);
extern void ws_server_test_worker_step(void);
extern void ws_server_test_reset(void);
extern void *get_ws_worker_queue(void);

#define FUZZ_FID_FULL          1
#define FUZZ_FID_HANDLER_ONLY  2
#define FUZZ_FID_QUEUE_ONLY    3

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

/* How the fake peripheral answers the request it pops (OP_PERIPH arg). */
enum {
    PERIPH_ECHO,        /* same id, request payload echoed back          */
    PERIPH_EMPTY,       /* same id, no payload                           */
    PERIPH_WRONG_ID,    /* id no call can have (ids are masked to 16 bit) */
    PERIPH_BROADCAST,   /* id 0 — delivered to every subscriber of FID    */
    PERIPH_MODE_COUNT
};

static ws_conn_t *conns[CONN_COUNT];
static int connected[CONN_COUNT];
static ws_queue_t periph_queue;

/* ── Input reader ─────────────────────────────────────────────────────── */

typedef struct {
    const uint8_t *p;
    size_t left;
} reader_t;

static uint8_t rd_u8(reader_t *r) {
    if (!r->left) return 0;
    r->left--;
    return *r->p++;
}

/* Returns a malloc'd copy of up to `want` bytes (the frame handlers take
 * ownership of the buffer). *out_len gets the real length. */
static uint8_t *rd_bytes(reader_t *r, size_t want, size_t *out_len) {
    size_t n = want < r->left ? want : r->left;
    uint8_t *buf = malloc(n ? n : 1);
    if (buf && n) memcpy(buf, r->p, n);
    r->p += n;
    r->left -= n;
    *out_len = n;
    return buf;
}

/* ── Fake endpoints ───────────────────────────────────────────────────── */

static uint8_t fuzz_test_handler(void *call, void **ctx, uint32_t pending,
                                  uint8_t *data, uint32_t len) {
    (void)ctx; (void)pending; (void)data; (void)len;
    uint32_t id;
    bApiCallGetId(call, &id);
    bApiCallSendStatus(call, API_CALL_STATUS_EXECUTING);
    return 1;
}

static void register_test_endpoints(void) {
    bApiCallRegister(fuzz_test_handler, FUZZ_FID_FULL, NULL);
    ws_server_register_fid_queue(FUZZ_FID_FULL, periph_queue);
    bApiCallRegister(fuzz_test_handler, FUZZ_FID_HANDLER_ONLY, NULL);
    ws_server_register_fid_queue(FUZZ_FID_QUEUE_ONLY, periph_queue);
}

static void periph_answer(uint8_t mode) {
    webapi_msg_t *req;
    if (!ws_hal_queue_receive(periph_queue, &req, WS_HAL_WAIT_NONE))
        return;

    webapi_msg_t resp = { .fid = req->fid, .id = req->id };
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

static const ws_frame_type_t frame_types[] = {
    WS_FRAME_CONT, WS_FRAME_BINARY, WS_FRAME_CLOSE, WS_FRAME_PING, WS_FRAME_PONG,
};

/* ── libFuzzer entry points ───────────────────────────────────────────── */

int LLVMFuzzerInitialize(int *argc, char ***argv) {
    (void)argc;
    (void)argv;
    ws_server_init();
    periph_queue = ws_hal_queue_create(PERIPH_QUEUE_DEPTH, sizeof(webapi_msg_t *));
    for (int i = 0; i < CONN_COUNT; i++) {
        conns[i] = ws_conn_stub_make(i + 1);
        ws_conn_stub_set_deferred(conns[i], 1);
    }
    register_test_endpoints();
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t len) {
    reader_t r = { data, len };

    for (int i = 0; i < CONN_COUNT; i++) {
        ws_conn_stub_set_send_result(conns[i], 0);
        ws_server_on_connect(conns[i]);
        connected[i] = 1;
    }

    while (r.left) {
        uint8_t op = rd_u8(&r) % OP_COUNT;
        switch (op) {
        case OP_TEXT: {
            int c = rd_u8(&r) % CONN_COUNT;
            size_t n;
            uint8_t *buf = rd_bytes(&r, rd_u8(&r), &n);
            if (buf) ws_server_on_frame(conns[c], WS_FRAME_TEXT, buf, n);
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
            ws_conn_stub_set_send_result(conns[c], (rd_u8(&r) & 1) ? -1 : 0);
            break;
        }
        case OP_CONNECT: {
            int c = rd_u8(&r) % CONN_COUNT;
            if (connected[c]) ws_server_on_disconnect(conns[c]);
            else              ws_server_on_connect(conns[c]);
            connected[c] = !connected[c];
            break;
        }
        case OP_FRAME: {
            int c = rd_u8(&r) % CONN_COUNT;
            ws_frame_type_t t = frame_types[rd_u8(&r) % (sizeof(frame_types) / sizeof(frame_types[0]))];
            size_t n;
            uint8_t *buf = rd_bytes(&r, rd_u8(&r), &n);
            if (buf) ws_server_on_frame(conns[c], t, buf, n);
            break;
        }
        case OP_COMPLETE: {
            int c = rd_u8(&r) % CONN_COUNT;
            ws_conn_stub_complete_pending(conns[c], (rd_u8(&r) & 1) ? -1 : 0);
            break;
        }
        }
    }

    for (int i = 0; i < CONN_COUNT; i++)
        ws_conn_stub_complete_pending(conns[i], 0);
    for (int i = 0; i < CONN_COUNT; i++) {
        if (connected[i]) ws_server_on_disconnect(conns[i]);
        connected[i] = 0;
    }
    drain_queues();
    ws_server_test_reset();
    register_test_endpoints();
    return 0;
}
