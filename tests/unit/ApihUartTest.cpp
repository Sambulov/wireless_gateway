extern "C" {
#include "app.h"
#include "web_api.h"
#include "ws_hal.h"
#include "uart.h"
}
#include "ws_hal_stub.h"
#include "gw_uart_stub.h"
#include "CppUTest/TestHarness.h"

#include <stdlib.h>
#include <string.h>

extern "C" {
    void ws_server_init(void);
    void ws_server_test_reset(void);
    void apih_uart_test_init(void);
    void apih_uart_test_reset(void);
    void apih_uart_test_fill_rx(int port, const uint8_t *data, uint32_t len);
    void apih_uart_test_fill_rx_idle(int port, const uint8_t *data, uint32_t len,
                                     uint32_t gap_us, uint8_t idle);
    void apih_uart_test_handle_msg(app_context_t *app, webapi_msg_t *msg);
}

/* ── Helpers ──────────────────────────────────────────────────────────── */

static webapi_msg_t *make_msg(const char *fid, const char *json_data) {
    webapi_msg_t *m = (webapi_msg_t *)malloc(sizeof(webapi_msg_t));
    memset(m, 0, sizeof(webapi_msg_t));
    bApiFidSet(m->fid, fid);
    m->id  = 1;
    if (json_data) {
        m->len  = strlen(json_data);
        m->data = (uint8_t *)malloc(m->len + 1);
        memcpy(m->data, json_data, m->len + 1);
    } else {
        m->data = NULL;
        m->len  = 0;
    }
    return m;
}

static void free_msg(webapi_msg_t *m) {
    free(m->data);
    free(m);
}

/* Очередь задачи в тестах — заголовок её содержимое не освобождает: всё, что
 * сервис положил в неё за тест, надо освободить руками, иначе ASan справедливо
 * ругается на утечку. */
static int count_char(const char *s, char c) {
    int n = 0;
    for (; s && *s; s++) if (*s == c) n++;
    return n;
}

static void drain_ws_queue(void) {
    void *q = get_ws_worker_queue();
    webapi_msg_t out;
    while (ws_hal_queue_receive(q, &out, 0))
        free(out.data);
}

/* ── Test fixture ─────────────────────────────────────────────────────── */

TEST_GROUP(ApihUartCnf) {
    app_context_t app;
    void setup() {
        ws_hal_stub_reset();
        ws_server_init();
        gw_uart_stub_reset();
        apih_uart_test_init();
        memset(&app, 0, sizeof(app));
    }
    void teardown() {
        drain_ws_queue();
        apih_uart_test_reset();
        ws_server_test_reset();
    }
};

TEST_GROUP(ApihUartTx) {
    app_context_t app;
    void setup() {
        ws_hal_stub_reset();
        ws_server_init();
        gw_uart_stub_reset();
        apih_uart_test_init();
        memset(&app, 0, sizeof(app));
    }
    void teardown() {
        drain_ws_queue();
        apih_uart_test_reset();
        ws_server_test_reset();
    }
};

TEST_GROUP(ApihUartRx) {
    app_context_t app;
    void setup() {
        ws_hal_stub_reset();
        ws_server_init();
        gw_uart_stub_reset();
        apih_uart_test_init();
        memset(&app, 0, sizeof(app));
    }
    void teardown() {
        drain_ws_queue();
        apih_uart_test_reset();
        ws_server_test_reset();
    }
};

TEST_GROUP(ApihUartEcho) {
    app_context_t app;
    void setup() {
        ws_hal_stub_reset();
        ws_server_init();
        gw_uart_stub_reset();
        apih_uart_test_init();
        memset(&app, 0, sizeof(app));
    }
    void teardown() {
        drain_ws_queue();
        apih_uart_test_reset();
        ws_server_test_reset();
    }
};

/* ── CNF tests ────────────────────────────────────────────────────────── */

TEST(ApihUartCnf, BaudRateSetOnPort1) {
    webapi_msg_t *m = make_msg(ESP_WS_API_UART1_CNF, "{\"BR\":9600}");
    apih_uart_test_handle_msg(&app, m);
    LONGS_EQUAL(9600, gw_uart_stub_last_config(&app.uart.port[0].desc).boud);
    free_msg(m);
}

TEST(ApihUartCnf, BaudRateSetOnPort2) {
    webapi_msg_t *m = make_msg(ESP_WS_API_UART2_CNF, "{\"BR\":115200}");
    apih_uart_test_handle_msg(&app, m);
    LONGS_EQUAL(115200, gw_uart_stub_last_config(&app.uart.port[1].desc).boud);
    free_msg(m);
}

TEST(ApihUartCnf, ParitySetOnPort1) {
    webapi_msg_t *m = make_msg(ESP_WS_API_UART1_CNF, "{\"PAR\":1}");
    apih_uart_test_handle_msg(&app, m);
    LONGS_EQUAL(GW_UART_PARITY_ODD, gw_uart_stub_last_config(&app.uart.port[0].desc).parity);
    free_msg(m);
}

TEST(ApihUartCnf, NullDataDoesNotCallSet) {
    webapi_msg_t *m = make_msg(ESP_WS_API_UART1_CNF, NULL);
    apih_uart_test_handle_msg(&app, m);
    /* gw_uart_set must NOT have been called — config stays at default zero */
    LONGS_EQUAL(0, gw_uart_stub_last_config(&app.uart.port[0].desc).boud);
    free_msg(m);
}

/* Вызов без ARG — это чтение: применять нечего, но ответить обязательно. Раньше
 * такой вызов уходил молча и висел у клиента до таймаута. */
TEST(ApihUartCnf, ReadWithoutArgAnswersCurrentState) {
    void *q = get_ws_worker_queue();

    /* Пусть на порту стоит не ноль: ответ обязан описать состояние порта. */
    webapi_msg_t *set = make_msg(ESP_WS_API_UART2_CNF, "{\"BR\":9600,\"MODE\":0}");
    apih_uart_test_handle_msg(&app, set);
    free_msg(set);

    webapi_msg_t drop;
    while (ws_hal_queue_receive(q, &drop, 0)) free(drop.data);

    webapi_msg_t *m = make_msg(ESP_WS_API_UART2_CNF, NULL);
    apih_uart_test_handle_msg(&app, m);

    webapi_msg_t out;
    memset(&out, 0, sizeof(out));
    CHECK(ws_hal_queue_receive(q, &out, 0) == 1);
    CHECK(out.data != NULL);
    if (out.data) {
        STRCMP_CONTAINS("\"BR\":9600", (const char *)out.data);
        STRCMP_CONTAINS("\"MODE\":0", (const char *)out.data);
        free(out.data);
    }
    free_msg(m);
}

/* Неразобранный ARG применять нельзя, но и молчать нечем: отвечаем состоянием. */
TEST(ApihUartCnf, BrokenArgAnswersCurrentState) {
    void *q = get_ws_worker_queue();
    webapi_msg_t *m = make_msg(ESP_WS_API_UART1_CNF, "not json");
    apih_uart_test_handle_msg(&app, m);

    webapi_msg_t out;
    memset(&out, 0, sizeof(out));
    CHECK(ws_hal_queue_receive(q, &out, 0) == 1);
    CHECK(out.data != NULL);
    if (out.data) {
        STRCMP_CONTAINS("\"BR\":0", (const char *)out.data);
        free(out.data);
    }
    /* И ничего не применили: на порту осталось прежнее (нулевое) значение */
    LONGS_EQUAL(0, gw_uart_stub_last_config(&app.uart.port[0].desc).boud);
    free_msg(m);
}

TEST(ApihUartCnf, ResponseEnqueuedToWorkerQueue) {
    void *q = get_ws_worker_queue();
    webapi_msg_t *m = make_msg(ESP_WS_API_UART1_CNF, "{\"BR\":4800}");
    apih_uart_test_handle_msg(&app, m);
    /* Response message was put on the ws worker queue */
    CHECK(ws_hal_stub_queue_count(q) > 0);
    free_msg(m);
}

TEST(ApihUartCnf, WordLengthSetOnPort1) {
    webapi_msg_t *m = make_msg(ESP_WS_API_UART1_CNF, "{\"WL\":0}"); /* 7-bit */
    apih_uart_test_handle_msg(&app, m);
    LONGS_EQUAL(GW_UART_WORD_7BIT, gw_uart_stub_last_config(&app.uart.port[0].desc).bits);
    free_msg(m);
}

/* Однопроводный режим (MODE 2): приём глушится программно, потому что драйверу
 * тут разворачивать нечего. Значение не влезало в однобитное поле команды. */
TEST(ApihUartCnf, SingleWireModeAccepted) {
    webapi_msg_t *m = make_msg(ESP_WS_API_UART2_CNF, "{\"MODE\":2}");
    apih_uart_test_handle_msg(&app, m);
    LONGS_EQUAL(GW_UART_MODE_UART_SINGLE_WIRE,
                gw_uart_stub_last_config(&app.uart.port[1].desc).mode);
    free_msg(m);
}

TEST(ApihUartCnf, UnknownModeNotApplied) {
    webapi_msg_t *set = make_msg(ESP_WS_API_UART2_CNF, "{\"MODE\":2}");
    apih_uart_test_handle_msg(&app, set);
    free_msg(set);

    webapi_msg_t *bad = make_msg(ESP_WS_API_UART2_CNF, "{\"MODE\":3}");
    apih_uart_test_handle_msg(&app, bad);
    /* Режим 3 не существует: на порту остаётся прежний, а не «ноль». */
    LONGS_EQUAL(GW_UART_MODE_UART_SINGLE_WIRE,
                gw_uart_stub_last_config(&app.uart.port[1].desc).mode);
    free_msg(bad);
}

/* ── TX tests ─────────────────────────────────────────────────────────── */

TEST(ApihUartTx, ValidBase64WritesToPort1) {
    /* base64("Hello") = "SGVsbG8=" */
    webapi_msg_t *m = make_msg(ESP_WS_API_UART1_RAW_TX, "\"SGVsbG8=\"");
    apih_uart_test_handle_msg(&app, m);
    LONGS_EQUAL(1, gw_uart_stub_write_calls(&app.uart.port[0].desc));
    LONGS_EQUAL(5, gw_uart_stub_last_write_len(&app.uart.port[0].desc));
    MEMCMP_EQUAL("Hello", gw_uart_stub_last_write_buf(&app.uart.port[0].desc), 5);
    free_msg(m);
}

TEST(ApihUartTx, ValidBase64WritesToPort2) {
    webapi_msg_t *m = make_msg(ESP_WS_API_UART2_RAW_TX, "\"SGVsbG8=\"");
    apih_uart_test_handle_msg(&app, m);
    LONGS_EQUAL(1, gw_uart_stub_write_calls(&app.uart.port[1].desc));
    free_msg(m);
}

TEST(ApihUartTx, NullDataDoesNotWrite) {
    webapi_msg_t *m = make_msg(ESP_WS_API_UART1_RAW_TX, NULL);
    apih_uart_test_handle_msg(&app, m);
    LONGS_EQUAL(0, gw_uart_stub_write_calls(&app.uart.port[0].desc));
    free_msg(m);
}

TEST(ApihUartTx, ResponseEnqueuedAfterWrite) {
    void *q = get_ws_worker_queue();
    webapi_msg_t *m = make_msg(ESP_WS_API_UART1_RAW_TX, "\"SGVsbG8=\"");
    apih_uart_test_handle_msg(&app, m);
    CHECK(ws_hal_stub_queue_count(q) > 0);
    free_msg(m);
}

/* Порог простоя линии: по нему драйвер отмечает конец кадра, а страница делит
 * поток данных порта на кадры (протоколы на стороне веб). */
TEST(ApihUartCnf, IdleCharsSetAndReported) {
    void *q = get_ws_worker_queue();
    webapi_msg_t *m = make_msg(ESP_WS_API_UART2_CNF, "{\"IDLE_CH\":8}");
    apih_uart_test_handle_msg(&app, m);
    LONGS_EQUAL(8, gw_uart_stub_last_config(&app.uart.port[1].desc).idle_ch);

    webapi_msg_t out;
    memset(&out, 0, sizeof(out));
    CHECK(ws_hal_queue_receive(q, &out, 0) == 1);
    CHECK(out.data != NULL);
    if (out.data) {
        STRCMP_CONTAINS("\"IDLE_CH\":8", (const char *)out.data);
        free(out.data);
    }
    free_msg(m);
}

TEST(ApihUartCnf, IdleCharsOutOfRangeNotApplied) {
    webapi_msg_t *m = make_msg(ESP_WS_API_UART2_CNF, "{\"IDLE_CH\":0}");
    apih_uart_test_handle_msg(&app, m);
    /* Ноль знаков — не порог: на порту остаётся прежнее (в стабе — ноль) */
    LONGS_EQUAL(0, gw_uart_stub_last_config(&app.uart.port[1].desc).idle_ch);
    free_msg(m);
}

/* ── RX tests ─────────────────────────────────────────────────────────── */

TEST(ApihUartRx, EmptyBufferSendsResponse) {
    void *q = get_ws_worker_queue();
    webapi_msg_t *m = make_msg(ESP_WS_API_UART1_RAW_RX, NULL);
    apih_uart_test_handle_msg(&app, m);
    /* Even with no data, a null response is enqueued */
    CHECK(ws_hal_stub_queue_count(q) > 0);
    free_msg(m);
}

TEST(ApihUartRx, FilledBufferSendsBase64Response) {
    void *q = get_ws_worker_queue();
    const uint8_t payload[] = "Hello";
    apih_uart_test_fill_rx(0, payload, 5);
    webapi_msg_t *m = make_msg(ESP_WS_API_UART1_RAW_RX, NULL);
    apih_uart_test_handle_msg(&app, m);
    CHECK(ws_hal_stub_queue_count(q) > 0);
    free_msg(m);
}

/* Кусок приёма, закончившийся на линии, отдаётся с разделителем '#': несколько
 * передач, попавших в один кадр отдачи, клиент по нему и разделяет. Кусок без
 * разделителя — продолжение предыдущего. */
TEST(ApihUartRx, CompletedPiecesAreDelimitedByHash) {
    void *q = get_ws_worker_queue();
    apih_uart_test_fill_rx_idle(1, (const uint8_t *)"AB", 2, 0, 1);
    apih_uart_test_fill_rx_idle(1, (const uint8_t *)"AB", 2, 0, 1);

    webapi_msg_t *m = make_msg(ESP_WS_API_UART2_RAW_RX, NULL);
    apih_uart_test_handle_msg(&app, m);

    webapi_msg_t out;
    memset(&out, 0, sizeof(out));
    CHECK(ws_hal_queue_receive(q, &out, 0) == 1);
    CHECK(out.data != NULL);
    if (out.data) {
        /* base64("AB") = "QUI=", а '#' — конец передачи на линии. */
        STRCMP_CONTAINS("\"RX\":\"QUI=#QUI=#\"", (const char *)out.data);
        free(out.data);
    }
    free_msg(m);
}

/* Незакрытый кусок уходит только тогда, когда перевалил за порог частичной
 * выгрузки, и уходит БЕЗ разделителя — значит кадр продолжается. Вслед за ним
 * приходит остаток, уже закрытый разделителем. */
TEST(ApihUartRx, ContinuationGoesWithoutDelimiter) {
    void *q = get_ws_worker_queue();
    static uint8_t long_data[1200];
    memset(long_data, 'x', sizeof(long_data));

    apih_uart_test_fill_rx_idle(1, long_data, sizeof(long_data), 0, 0);
    apih_uart_test_fill_rx_idle(1, (const uint8_t *)"EF", 2, 0, 1);

    webapi_msg_t *m = make_msg(ESP_WS_API_UART2_RAW_RX, NULL);
    apih_uart_test_handle_msg(&app, m);

    webapi_msg_t out;
    memset(&out, 0, sizeof(out));
    CHECK(ws_hal_queue_receive(q, &out, 0) == 1);
    CHECK(out.data != NULL);
    if (out.data) {
        /* В сообщении два куска: первый — ровно порог, без разделителя (кадр
         * продолжается); второй — остаток, закрытый разделителем. Значит
         * разделитель в сообщении РОВНО один. */
        LONGS_EQUAL(1, count_char((const char *)out.data, '#'));
        STRCMP_CONTAINS("#\"}", (const char *)out.data);
        CHECK(strlen((const char *)out.data) > 1000);
        free(out.data);
    }
    free_msg(m);
}

TEST(ApihUartRx, BufferClearedAfterRead) {
    const uint8_t payload[] = "Hi";
    apih_uart_test_fill_rx(1, payload, 2);
    webapi_msg_t *m = make_msg(ESP_WS_API_UART2_RAW_RX, NULL);
    apih_uart_test_handle_msg(&app, m);
    /* Second call on the same port returns empty (amount reset to 0) */
    void *q = get_ws_worker_queue();
    uint32_t before = ws_hal_stub_queue_count(q);
    webapi_msg_t *m2 = make_msg(ESP_WS_API_UART2_RAW_RX, NULL);
    apih_uart_test_handle_msg(&app, m2);
    /* Both calls produced a response — second one is the empty-buffer path */
    CHECK(ws_hal_stub_queue_count(q) > before);
    free_msg(m);
    free_msg(m2);
}

/* ── Темп кадров приёма ───────────────────────────────────────────────────
 *
 * Данные уходят кадром не чаще, чем раз в UART_RX_FRAME_MS (75 мс): при
 * предельной скорости порта (500000 бод = 50000 Б/с) за этот срок накапливается
 * 3750 байт — меньше накопителя (4096), то есть копить можно без потерь, а
 * кадров в разы меньше (иначе эхо набора в терминале давало кадр на символ).
 * Задержку в тестах стаб не отбывает, а запоминает. */

TEST(ApihUartRx, SecondFrameWaitsForTheRateGate) {
    /* Первый кадр уходит сразу — ждать нечего */
    const uint8_t first[] = "one";
    apih_uart_test_fill_rx(1, first, 3);
    webapi_msg_t *m1 = make_msg(ESP_WS_API_UART2_RAW_RX, NULL);
    apih_uart_test_handle_msg(&app, m1);
    LONGS_EQUAL(0, ws_hal_stub_last_delay());

    /* Кадр сразу за ним — только по истечении темпа, не раньше 75 мс */
    const uint8_t second[] = "two";
    apih_uart_test_fill_rx(1, second, 3);
    webapi_msg_t *m2 = make_msg(ESP_WS_API_UART2_RAW_RX, NULL);
    apih_uart_test_handle_msg(&app, m2);
    CHECK(ws_hal_stub_last_delay() > 0);
    CHECK(ws_hal_stub_last_delay() <= 75);

    /* Пустой опрос темпа не ждёт: данных нет — отвечать нечем и незачем ждать */
    uint32_t waited = ws_hal_stub_last_delay();
    webapi_msg_t *m3 = make_msg(ESP_WS_API_UART2_RAW_RX, NULL);
    apih_uart_test_handle_msg(&app, m3);
    LONGS_EQUAL(waited, ws_hal_stub_last_delay());

    free_msg(m1);
    free_msg(m2);
    free_msg(m3);
}

TEST(ApihUartRx, RateGateIsPerPort) {
    /* Темп считается на КАЖДЫЙ порт отдельно: кадр второго порта не заставляет
     * ждать первый. */
    const uint8_t data[] = "x";
    apih_uart_test_fill_rx(0, data, 1);
    webapi_msg_t *m1 = make_msg(ESP_WS_API_UART1_RAW_RX, NULL);
    apih_uart_test_handle_msg(&app, m1);

    apih_uart_test_fill_rx(1, data, 1);
    webapi_msg_t *m2 = make_msg(ESP_WS_API_UART2_RAW_RX, NULL);
    apih_uart_test_handle_msg(&app, m2);

    LONGS_EQUAL(0, ws_hal_stub_last_delay());
    free_msg(m1);
    free_msg(m2);
}

/* ── Echo tests ───────────────────────────────────────────────────────── */

TEST(ApihUartEcho, SetEchoOnPort1) {
    webapi_msg_t *m = make_msg(ESP_WS_API_UART1_ECHO, "{\"E\":1}");
    apih_uart_test_handle_msg(&app, m);
    LONGS_EQUAL(1, gw_uart_get_echo(&app.uart.port[0].desc));
    free_msg(m);
}

TEST(ApihUartEcho, ClearEchoOnPort2) {
    gw_uart_set_echo(&app.uart.port[1].desc, 1);
    webapi_msg_t *m = make_msg(ESP_WS_API_UART2_ECHO, "{\"E\":0}");
    apih_uart_test_handle_msg(&app, m);
    LONGS_EQUAL(0, gw_uart_get_echo(&app.uart.port[1].desc));
    free_msg(m);
}

TEST(ApihUartEcho, QuerySendsResponse) {
    void *q = get_ws_worker_queue();
    webapi_msg_t *m = make_msg(ESP_WS_API_UART1_ECHO, NULL);
    apih_uart_test_handle_msg(&app, m);
    CHECK(ws_hal_stub_queue_count(q) > 0);
    free_msg(m);
}

TEST(ApihUartEcho, UnknownFidNoResponse) {
    void *q = get_ws_worker_queue();
    webapi_msg_t *m = make_msg("uart3.rx", NULL);
    apih_uart_test_handle_msg(&app, m);
    LONGS_EQUAL(0, ws_hal_stub_queue_count(q));
    free_msg(m);
}
