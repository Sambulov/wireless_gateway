/*
 * libFuzzer harness for ws_server.c's untrusted-input entry point.
 *
 * ws_server_on_frame()/ws_server_on_text() parse the raw JSON text a
 * WebSocket client sends over the wire ({"FID":.., "SID":.., "ARG":..}).
 * This is the first place attacker-controlled bytes hit the firmware, so
 * it's the highest-value fuzz target in this component.
 *
 * After the frame is parsed, the harness drives the worker task
 * synchronously via ws_server_test_worker_step() (there is no FreeRTOS task
 * on the host) and plays the role of a peripheral that echoes every request
 * back into the worker queue. That way one input exercises the whole call
 * lifecycle: parse -> enqueue -> forward to peripheral -> DELIVERED ->
 * response -> handler -> JSON reply -> GC / LONG_TERM re-poll -> ping.
 *
 * FIDs are small so the mutator can reach them with minimal byte flips:
 *   FID 1 — handler + peripheral queue (full path, handler gets invoked)
 *   FID 2 — handler only, no queue      (worker replies INVALID)
 *   FID 3 — peripheral queue only       (no handler)
 *
 * The input format is unchanged (a single raw text frame), so the existing
 * corpus stays valid.
 *
 * Reuses the same host stubs (ws_hal_stub, ws_conn_stub) already proven to
 * link ws_server.c on the host by tests/unit/WsServerTest.cpp.
 *
 * Build: tests/fuzz/build.sh
 * Run:   tests/fuzz/build/ws_server_fuzzer -max_len=4096 corpus/
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
extern void ws_server_on_frame(ws_conn_t *conn, ws_frame_type_t type,
                                uint8_t *data, size_t len);
extern void ws_server_test_worker_step(void);
extern void ws_server_test_reset(void);
extern void *get_ws_worker_queue(void);

#define FUZZ_FID_FULL          1
#define FUZZ_FID_HANDLER_ONLY  2
#define FUZZ_FID_QUEUE_ONLY    3

/* Small on purpose so the "periph queue full -> BUSY" branch is reachable. */
#define PERIPH_QUEUE_DEPTH     4

/* Worker iterations per input. Each round advances the tick by
 * TICK_PER_ROUND, so by the last rounds the session is idle for longer
 * than CONFIG_WEB_SOCKET_PING_DELAY (1000) and the ping path runs. */
#define WORKER_ROUNDS          4
#define TICK_PER_ROUND         600

static ws_conn_t *conn;
static ws_queue_t periph_queue;

static uint8_t fuzz_test_handler(void *call, void **ctx, uint32_t pending,
                                  uint8_t *data, uint32_t len) {
    (void)ctx; (void)pending; (void)data; (void)len;
    /* Call back into the public API from inside the worker, like real
     * handlers do (with the worker already holding the recursive mutex). */
    uint32_t id;
    bApiCallGetId(call, &id);
    bApiCallSendStatus(call, API_CALL_STATUS_EXECUTING);
    return 1; /* complete */
}

/* ws_server_test_reset() also unregisters every handler and FID queue (it
 * frees those lists too), so registration must be redone after each reset —
 * otherwise it would only ever be live for the very first input. */
static void register_test_endpoints(void) {
    bApiCallRegister(fuzz_test_handler, FUZZ_FID_FULL, NULL);
    ws_server_register_fid_queue(FUZZ_FID_FULL, periph_queue);
    bApiCallRegister(fuzz_test_handler, FUZZ_FID_HANDLER_ONLY, NULL);
    ws_server_register_fid_queue(FUZZ_FID_QUEUE_ONLY, periph_queue);
}

/* Fake peripheral: consume every forwarded request and echo its payload
 * back to the worker as a response to the same call id. The peripheral
 * queue carries webapi_msg_t* (ownership of msg and msg->data moves to the
 * peripheral); the worker queue carries webapi_msg_t by value and the
 * worker frees .data. */
static void periph_service(void) {
    webapi_msg_t *req;
    while (ws_hal_queue_receive(periph_queue, &req, WS_HAL_WAIT_NONE)) {
        webapi_msg_t resp = { .fid = req->fid, .id = req->id };
        if (req->data && req->len) {
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
}

/* Drop anything still in flight so runs stay independent and LSan doesn't
 * blame the next input for this one's leftovers. */
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

int LLVMFuzzerInitialize(int *argc, char ***argv) {
    (void)argc;
    (void)argv;
    ws_server_init();
    periph_queue = ws_hal_queue_create(PERIPH_QUEUE_DEPTH, sizeof(webapi_msg_t *));
    conn = ws_conn_stub_make(1);
    ws_server_on_connect(conn);
    register_test_endpoints();
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t len) {
    if (len == 0)
        return 0;

    /* ws_server_on_text() takes ownership of the buffer it's handed
     * (it frees it internally), so libFuzzer's own input buffer must
     * never be passed in directly. */
    uint8_t *copy = malloc(len);
    if (!copy)
        return 0;
    memcpy(copy, data, len);

    ws_server_on_frame(conn, WS_FRAME_TEXT, copy, len);

    for (int i = 0; i < WORKER_ROUNDS; i++) {
        ws_server_test_worker_step();
        periph_service();
        ws_hal_stub_advance_tick(TICK_PER_ROUND);
    }

    drain_queues();
    ws_server_test_reset();
    register_test_endpoints();
    return 0;
}
