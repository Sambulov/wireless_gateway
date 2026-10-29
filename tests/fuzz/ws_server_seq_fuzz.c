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
 *   OP_STEP         -                        один такт ядра (api_worker)
 *   OP_PERIPH       mode                     устройство отвечает на забранный
 *                                            вызов (api_call_send_arg), отказывает
 *                                            без вызова или рассылает кадр по FID
 *                                            (api_call_group_fid_data)
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
    PERIPH_ECHO,        /* ответ по дескриптору вызова, полезная нагрузка как есть */
    PERIPH_EMPTY,       /* ответ по дескриптору вызова без данных               */
    PERIPH_NO_CALL,     /* отвечать нечему: пустой дескриптор вызова            */
    PERIPH_BROADCAST,   /* рассылка всем подписчикам имени FID                  */
    PERIPH_MODE_COUNT
};

static ws_transport_stub_conn_t *conns[CONN_COUNT];
/* Ядро харнесса: дескриптор создаёт api_init и освобождает api_deinit — так
 * каждый вход получает чистое ядро, а статического состояния у ядра нет. */
static api_t *api;
static int connected[CONN_COUNT];

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
    if (!call) return 0;
    const uint8_t body[] = "{\"OK\":1}";
    api_call_send_arg(call, (const char *)body, sizeof(body) - 1);
    return 0;
}

static int fuzz_alive_rounds;
static uint8_t fuzz_alive_handler(void *call, void **ctx, uint8_t *data, uint32_t len) {
    (void)ctx; (void)data; (void)len;
    if (!call) return 0;
    const uint8_t body[] = "{\"ALIVE\":1}";
    api_call_send_arg(call, (const char *)body, sizeof(body) - 1);
    return (++fuzz_alive_rounds % 8) != 0;
}

/* ── Фальшивое устройство ────────────────────────────────────────────────
 *
 * Очередь запросов — ВНУТРЕННЕЕ дело сервиса: ядро о ней не знает и просто
 * зовёт обработчик. Здесь это массив вызовов, которые обработчик забрал и ещё не
 * завершил; место вызова в массиве лежит в контексте вызова (+1, чтобы 0
 * означало «слота нет»). */
static void *periph_calls[PERIPH_QUEUE_DEPTH];
static uint8_t periph_answered[PERIPH_QUEUE_DEPTH];
static int periph_n;

static void periph_reset(void) {
    memset(periph_calls, 0, sizeof(periph_calls));
    memset(periph_answered, 0, sizeof(periph_answered));
    periph_n = 0;
}

static uint8_t fuzz_periph_handler(void *call, void **ctx, uint8_t *data, uint32_t len) {
    (void)data; (void)len;
    int idx = (int)(intptr_t)*ctx - 1;

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
        const uint8_t busy[] = "{\"STA\":\"0x80000008\"}";
        api_call_send_arg(call, (const char *)busy, sizeof(busy) - 1);
        return 0;
    }
    periph_calls[periph_n++] = call;
    *ctx = (void *)(intptr_t)periph_n;
    return 1;
}

static void register_test_endpoints(void) {
    api_call_register(api, fuzz_done_handler,   FUZZ_FID_FULL,  NULL);
    api_call_register(api, fuzz_alive_handler,  FUZZ_FID_ALIVE, NULL);
    api_call_register(api, fuzz_periph_handler, FUZZ_FID_QUEUE, NULL);
}

/* Устройство отвечает на первый забранный вызов. Ответ на КОНКРЕТНЫЙ вызов идёт
 * через его дескриптор (api_call_send_arg) — так делает любой сервис; оставшиеся
 * ветки — отказ без вызова и рассылка по имени FID (api_call_group_fid_data). */
static void periph_answer(uint8_t mode) {
    void *call = NULL;
    int    idx = -1;
    for (int i = 0; i < periph_n; i++) {
        if (periph_calls[i] && !periph_answered[i]) { call = periph_calls[i]; idx = i; break; }
    }
    const uint8_t body[] = "{\"V\":1}";

    if (!call) {
        /* Забранных вызовов нет — проверяем оба отказа: рассылка без подписчиков
         * и ответ без дескриптора вызова. */
        switch (mode % PERIPH_MODE_COUNT) {
        case PERIPH_BROADCAST: api_call_group_fid_data(api, FUZZ_FID_ALIVE, (const char *)body, sizeof(body) - 1); break;
        default:               api_call_send_arg(NULL, (const char *)body, sizeof(body) - 1); break;
        }
        return;
    }

    switch (mode % PERIPH_MODE_COUNT) {
    case PERIPH_NO_CALL:
        /* Отвечать нечему: дескриптор вызова пуст. */
        api_call_send_arg(NULL, (const char *)body, sizeof(body) - 1);
        return;
    case PERIPH_BROADCAST:
        /* Рассылка по имени FID: адресована подписчикам, а не этому вызову. */
        api_call_group_fid_data(api, FUZZ_FID_ALIVE, (const char *)body, sizeof(body) - 1);
        return;
    case PERIPH_EMPTY:
        api_call_send_arg(call, "", 0);
        break;
    default:
        api_call_send_arg(call, (const char *)body, sizeof(body) - 1);
        break;
    }
    periph_answered[idx] = 1;               /* закроется следующим тактом */
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
    /* Транспорт снимаем раньше ядра: он доводит до конца отложенные отправки, и
     * ядро освобождает свои буферы ответов. Затем ядро снимается целиком
     * (api_deinit отписывает его от транспорта) и заводится заново. */
    ws_transport_stub_reset();
    api_deinit(api);
    periph_reset();
    api = ws_transport_stub_init();
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
            api_worker(api);
            break;
        case OP_PERIPH:
            periph_answer(rd_u8(&r));
            break;
        case OP_TICK:
            system_stub_advance_time((uint32_t)rd_u8(&r) * 16);
            break;
        case OP_SEND_RESULT: {
            int c = rd_u8(&r) % CONN_COUNT;
            ws_transport_stub_set_send_result(conns[c],
                (rd_u8(&r) & 1) ? TRANSPORT_FAULT : TRANSPORT_OK);
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
                (rd_u8(&r) & 1) ? TRANSPORT_FAULT : TRANSPORT_OK);
            break;
        }
        default:
            break;
        }
    }

    rearm();
    return 0;
}
