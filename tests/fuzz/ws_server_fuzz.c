/*
 * libFuzzer harness for the API kernel's untrusted-input entry point.
 *
 * A WebSocket client sends the JSON text {"FID":"..","FLAGS":..,"SID":..,"ARG":..}.
 * These bytes are the first attacker-controlled data that reaches the firmware,
 * so this is the highest-value fuzz target here.
 *
 * Ядро абстрагировано транспортом, поэтому харнесс подключает ЗАГЛУШКУ
 * транспорта (tests/unit/stubs/ws_transport_stub.c) — ту же, которой
 * пользуются хостовые тесты, — и подаёт вход как текстовый кадр WebSocket.
 * Дальше харнесс играет роль периферии: отвечает на всё, что ядро переслало в
 * очередь устройства, чтобы один вход прошёл весь жизненный цикл вызова:
 * разбор → регистрация → пересылка → ответ → обработчик → ответ клиенту →
 * снятие вызова либо переопрос подписки.
 *
 * Сервисы:
 *   "fuzz.full"   — обработчик И очередь устройства (полный путь)
 *   "fuzz.alive"  — обработчик, который не завершается сразу (живёт тактами)
 *   "fuzz.queue"  — только очередь, обработчика нет
 *
 * Вход — один сырой текстовый кадр, поэтому прежний корпус остаётся годным.
 *
 * Сборка: tests/fuzz/build.sh
 * Запуск: tests/fuzz/build/ws_server_fuzzer -max_len=4096 tests/fuzz/seeds/
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

/* Small on purpose so the "periph queue full -> BUSY" branch is reachable. */
#define PERIPH_QUEUE_DEPTH     4

/* Worker iterations per input: продвигаем тик, чтобы дошли и переопрос
 * длинных вызовов (ARG.RDL), и уборка завершённых. */
#define WORKER_ROUNDS          4
#define TICK_PER_ROUND         600

static ws_transport_stub_conn_t *conn;
static ws_queue_t periph_queue;

/* Обработчик, который завершается сразу: покрывает закрытие вызова. */
static uint8_t fuzz_done_handler(void *call, void **ctx, uint8_t *data, uint32_t len) {
    (void)ctx; (void)data; (void)len;
    uint32_t id;
    bApiCallGetId(call, &id);
    bApiCallSendStatus(call, API_CALL_STATUS_EXECUTING);
    return 0;                       /* вызов обработан */
}

/* Обработчик, который живёт: покрывает такт работы и переопрос подписки. */
static int fuzz_alive_rounds;
static uint8_t fuzz_alive_handler(void *call, void **ctx, uint8_t *data, uint32_t len) {
    (void)ctx; (void)data; (void)len;
    uint32_t id;
    bApiCallGetId(call, &id);
    bApiCallSendStatus(call, API_CALL_STATUS_EXECUTING);
    return (++fuzz_alive_rounds % 8) != 0;      /* иногда всё же завершается */
}

/* ws_server_test_reset() снимает и регистрации (освобождает их списки),
 * поэтому после каждого входа их надо оформить заново. */
static void register_test_endpoints(void) {
    bApiCallRegister(fuzz_done_handler,  FUZZ_FID_FULL,  GW_IFACE_MASK_ANY, NULL);
    ws_server_register_fid_queue(FUZZ_FID_FULL, GW_IFACE_MASK_ANY, periph_queue);
    bApiCallRegister(fuzz_alive_handler, FUZZ_FID_ALIVE, GW_IFACE_MASK_ANY, NULL);
    ws_server_register_fid_queue(FUZZ_FID_QUEUE, GW_IFACE_MASK_ANY, periph_queue);
}

/* Фальшивая периферия: забирает пересланный запрос и возвращает его же как
 * ответ тому же вызову (очередь устройства владеет сообщением и данными). */
static void periph_service(void) {
    webapi_msg_t *req;
    while (ws_hal_queue_receive(periph_queue, &req, WS_HAL_WAIT_NONE)) {
        webapi_msg_t resp = {0};
        memcpy(resp.fid, req->fid, sizeof(resp.fid));
        resp.id = req->id;
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

/* Не оставляем ничего в полёте: следующий вход не должен расплачиваться за
 * этот (иначе LeakSanitizer свалит чужие остатки на новый вход). */
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

static void rearm(void) {
    ws_server_test_reset();
    ws_transport_stub_reset();
    ws_transport_stub_attach();
    conn = ws_transport_stub_connect(1);
    register_test_endpoints();
}

int LLVMFuzzerInitialize(int *argc, char ***argv) {
    (void)argc;
    (void)argv;
    ws_server_init();
    periph_queue = ws_hal_queue_create(PERIPH_QUEUE_DEPTH, sizeof(webapi_msg_t *));
    ws_transport_stub_reset();
    ws_transport_stub_attach();
    conn = ws_transport_stub_connect(1);
    register_test_endpoints();
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t len) {
    if (len == 0 || conn == NULL)
        return 0;

    fuzz_alive_rounds = 0;
    /* Заглушка копирует данные к себе, поэтому буфер фаззера отдаём как есть. */
    ws_transport_stub_frame(conn, WS_STUB_FRAME_TEXT, data, len, 1);

    for (int i = 0; i < WORKER_ROUNDS; i++) {
        ws_server_test_worker_step();
        periph_service();
        ws_hal_stub_advance_tick(TICK_PER_ROUND);
    }

    drain_queues();
    rearm();
    return 0;
}
