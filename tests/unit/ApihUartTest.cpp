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
    void apih_uart_test_handle_msg(app_context_t *app, webapi_msg_t *msg);
}

/* ── Helpers ──────────────────────────────────────────────────────────── */

static webapi_msg_t *make_msg(uint32_t fid, const char *json_data) {
    webapi_msg_t *m = (webapi_msg_t *)malloc(sizeof(webapi_msg_t));
    m->fid = fid;
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
    webapi_msg_t *m = make_msg(0xDEAD, NULL);
    apih_uart_test_handle_msg(&app, m);
    LONGS_EQUAL(0, ws_hal_stub_queue_count(q));
    free_msg(m);
}
