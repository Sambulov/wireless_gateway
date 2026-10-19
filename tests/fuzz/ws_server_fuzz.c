/*
 * libFuzzer harness for the API kernel's untrusted-input entry point.
 *
 * A WebSocket client sends the JSON text {"FID":"..","FLAGS":..,"SID":..,"ARG":..}.
 * These bytes are the first attacker-controlled data that reaches the firmware,
 * so this is the highest-value fuzz target here.
 *
 * Ядро абстрагировано транспортом, поэтому харнесс подключает ЗАГЛУШКУ
 * транспорта (tests/unit/stubs/transport_stub.c) — ту же, которой пользуются
 * хостовые тесты, — и подаёт вход как текстовый кадр WebSocket. Дальше харнесс
 * играет роль периферии: запрос доставляется ядром ВЫЗОВОМ ОБРАБОТЧИКА (очередей
 * у ядра нет), обработчик складывает вызов в свою — внутреннюю — очередь, а
 * харнесс отвечает ему позже через api_reply. Так один вход проходит весь
 * жизненный цикл вызова: разбор → регистрация → вызов обработчика → ответ →
 * отправка клиенту → снятие вызова либо переопрос подписки. Завершение вызова —
 * это возврат обработчика (FALSE), отдельного «завершить» у ядра нет; адресат
 * ответа — дескриптор вызова, а не id в списке ядра.
 *
 * Сервисы:
 *   "fuzz.full"   — обработчик, который отвечает сразу и завершает вызов;
 *   "fuzz.alive"  — обработчик, который живёт тактами (иногда завершается);
 *   "fuzz.queue"  — обработчик, который копит вызовы у себя и отвечает позже
 *                   (так выглядит сервис со своим потоком/очередью).
 *
 * Вход — один сырой текстовый кадр, поэтому корпус остаётся годным.
 *
 * Сборка: tests/fuzz/build.sh
 * Запуск: tests/fuzz/build/ws_server_fuzzer -max_len=4096 tests/fuzz/seeds/
 */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "api.h"
#include "platform.h"
#include "transport.h"
#include "transport_stub.h"
#include "system_stub.h"

/* Такт ядра — публичный api_worker: в прошивке его зовёт задача приложения, а
 * здесь — харнесс (задачи в хосте нет). Ядро у харнесса своё: дескриптор ниже. */

#define FUZZ_FID_FULL   "fuzz.full"
#define FUZZ_FID_ALIVE  "fuzz.alive"
#define FUZZ_FID_QUEUE  "fuzz.queue"

/* Small on purpose so the "peripheral queue full -> BUSY" branch is reachable. */
#define PERIPH_QUEUE_DEPTH     4

/* Worker iterations per input: продвигаем время, чтобы дошли и переопрос
 * длинных вызовов (ARG.RDL), и уборка завершённых. */
#define WORKER_ROUNDS          4
#define TICK_PER_ROUND         600

static ws_transport_stub_conn_t *conn;
/* Ядро харнесса: дескриптор создаёт api_init и освобождает api_deinit — так
 * каждый вход получает чистое ядро, а статического состояния у ядра нет. */
static api_t *api;

/* ── Фальшивое устройство ────────────────────────────────────────────────
 *
 * Очередь запросов — ВНУТРЕННЕЕ дело сервиса (ядро о ней не знает): здесь это
 * массив вызовов, которые обработчик забрал и ещё не завершил. Место вызова в
 * массиве лежит в контексте вызова (+1, чтобы 0 означало «слота нет»). */
static void *periph_calls[PERIPH_QUEUE_DEPTH];
static uint8_t periph_answered[PERIPH_QUEUE_DEPTH];
static int periph_n;

static void periph_reset(void) {
    memset(periph_calls, 0, sizeof(periph_calls));
    memset(periph_answered, 0, sizeof(periph_answered));
    periph_n = 0;
}

/* Обработчик, который отвечает и завершается сразу: покрывает закрытие вызова
 * в том же такте, в котором он пришёл. */
static uint8_t fuzz_done_handler(void *call, void **ctx, uint8_t *data, uint32_t len) {
    (void)ctx; (void)data; (void)len;
    if (!call) return 0;
    const uint8_t body[] = "{\"OK\":1}";
    api_reply(call, body, sizeof(body) - 1);
    return 0;                       /* FALSE — вызов завершён */
}

/* Обработчик, который живёт: покрывает такт работы и переопрос подписки. */
static int fuzz_alive_rounds;
static uint8_t fuzz_alive_handler(void *call, void **ctx, uint8_t *data, uint32_t len) {
    (void)ctx; (void)data; (void)len;
    if (!call) return 0;
    const uint8_t body[] = "{\"ALIVE\":1}";
    api_reply(call, body, sizeof(body) - 1);
    return (++fuzz_alive_rounds % 8) != 0;      /* иногда всё же завершается */
}

/* Обработчик со своей очередью: берёт вызов и отвечает на следующем такте. */
static uint8_t fuzz_periph_handler(void *call, void **ctx, uint8_t *data, uint32_t len) {
    (void)data; (void)len;
    int idx = (int)(intptr_t)*ctx - 1;          /* 0 → слота ещё нет */

    if (!call) {                                /* снятие: освобождаем слот */
        if (idx >= 0 && idx < PERIPH_QUEUE_DEPTH) {
            periph_calls[idx]    = NULL;
            periph_answered[idx] = 0;
        }
        *ctx = NULL;
        return 0;
    }

    if (idx >= 0 && idx < PERIPH_QUEUE_DEPTH) {
        if (!periph_answered[idx])
            return 1;                           /* устройство ещё думает */
        periph_calls[idx]    = NULL;
        periph_answered[idx] = 0;
        *ctx = NULL;
        return 0;                               /* ответ ушёл — вызов закрыт */
    }

    if (periph_n >= PERIPH_QUEUE_DEPTH) {
        /* Очередь полна — честный отказ, как у сервиса с ограниченной очередью:
         * ответ уходит сразу, и вызов завершается. */
        const uint8_t busy[] = "{\"STA\":\"0x80000008\"}";
        api_reply(call, busy, sizeof(busy) - 1);
        return 0;
    }
    periph_calls[periph_n++] = call;
    *ctx = (void *)(intptr_t)periph_n;          /* индекс слота + 1 */
    return 1;                                   /* «ответ будет позже» */
}

/* api_deinit() снимает и регистрации (освобождает их списки), поэтому после
 * каждого входа их надо оформить заново — уже на новом ядре. */
static void register_test_endpoints(void) {
    api_register_handler(api, fuzz_done_handler,   FUZZ_FID_FULL,  NULL);
    api_register_handler(api, fuzz_alive_handler,  FUZZ_FID_ALIVE, NULL);
    api_register_handler(api, fuzz_periph_handler, FUZZ_FID_QUEUE, NULL);
}

/* Фальшивая периферия: отвечает на всё, что её обработчик забрал. Ответ идёт
 * через дескриптор вызова (api_reply) — ядро вызов помнит сам, а очередей ему
 * для этого не нужно. */
static void periph_service(void) {
    const uint8_t body[] = "{\"OK\":1}";
    for (int i = 0; i < periph_n; i++) {
        if (!periph_calls[i] || periph_answered[i])
            continue;
        api_reply(periph_calls[i], body, sizeof(body) - 1);
        periph_answered[i] = 1;                 /* закроется следующим тактом */
    }
}

static void rearm(void) {
    /* Транспорт снимаем раньше ядра: он доводит до конца отложенные отправки, и
     * ядро освобождает свои буферы ответов. Затем ядро снимается целиком
     * (api_deinit отписывает его от транспорта) и заводится заново. */
    ws_transport_stub_reset();
    api_deinit(api);
    periph_reset();
    api = ws_transport_stub_init();
    conn = ws_transport_stub_connect(1);
    register_test_endpoints();
}

int LLVMFuzzerInitialize(int *argc, char ***argv) {
    (void)argc;
    (void)argv;
    ws_transport_stub_reset();
    api = ws_transport_stub_init();
    conn = ws_transport_stub_connect(1);
    periph_reset();
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
        api_worker(api);
        periph_service();
        system_stub_advance_time(TICK_PER_ROUND);
    }

    rearm();
    return 0;
}
