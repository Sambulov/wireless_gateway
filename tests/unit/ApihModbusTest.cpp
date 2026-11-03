/* Служба Modbus (main/apih_modbus.c) через настоящее ядро API.
 *
 * Клиент — кадр в стаб-транспорт плюс такт ядра, линия — заглушка драйвера
 * UART: что служба отправила в линию, видно по gw_uart_stub_last_write_buf, а
 * ответ прибора тест подаёт байтами тем же путём, что событие приёма UART
 * (apih_modbus_test_rx). Время — system_stub_advance_time. */
extern "C" {
#include "app.h"
#include "api.h"
#include "platform.h"
#include "uart.h"
}
#include "system_stub.h"
#include "transport_stub.h"
#include "gw_uart_stub.h"
#include "CppUTest/TestHarness.h"

#include <stdio.h>
#include <string.h>

extern "C" {
    void apih_modbus_test_reset(void);
    void apih_modbus_test_rx(int worker, const uint8_t *data, uint32_t len, uint8_t end);
}

/* Порт воркера 0 — "uart1.modbus"; его линия — дескриптор app.uart.port[0]
 * (так связывает ws_modbus_run). */
#define MB_FID   ESP_WS_API_UART1_MODBUS_REQ

static void client_request(api_t *api, ws_transport_stub_conn_t *conn,
                           int sid, const char *arg) {
    char frame[256];
    snprintf(frame, sizeof(frame), "{\"FID\":\"%s\",\"SID\":%d,\"ARG\":%s}", MB_FID, sid, arg);
    ws_transport_stub_frame(conn, WS_STUB_FRAME_TEXT, frame, strlen(frame), 1);
    api_worker(api);
}

TEST_GROUP(ApihModbus) {
    app_context_t app;
    api_t *api;
    ws_transport_stub_conn_t *conn;
    void *line;

    void setup() {
        memset(&app, 0, sizeof(app));
        system_stub_reset();
        ws_transport_stub_reset();
        gw_uart_stub_reset();
        apih_modbus_test_reset();
        api  = ws_transport_stub_init();
        conn = ws_transport_stub_connect(1);
        app.api_ap  = api;
        app.api_sta = NULL;
        LONGS_EQUAL(ESP_OK, ws_modbus_run(&app));
        line = &app.uart.port[0].desc;
    }
    void teardown() {
        ws_transport_stub_reset();
        api_deinit(api);
        apih_modbus_test_reset();
    }
};

/* Чтение одного регистра: кадр RTU уходит в линию сразу, ответ прибора
 * возвращается клиенту значениями регистров. */
TEST(ApihModbus, ReadRequestGoesToTheLineAndReplyComesBack) {
    client_request(api, conn, 1, "{\"ADR\":1,\"FN\":3,\"RA\":0,\"RVC\":1}");

    static const uint8_t req[] = { 0x01, 0x03, 0x00, 0x00, 0x00, 0x01, 0x84, 0x0A };
    LONGS_EQUAL(1, gw_uart_stub_write_calls(line));
    LONGS_EQUAL(sizeof(req), gw_uart_stub_last_write_len(line));
    MEMCMP_EQUAL(req, gw_uart_stub_last_write_buf(line), sizeof(req));

    static const uint8_t resp[] = { 0x01, 0x03, 0x02, 0x00, 0x2A, 0x39, 0x9B };
    apih_modbus_test_rx(0, resp, sizeof(resp), 1);
    system_stub_advance_time(10);
    api_worker(api);

    STRCMP_CONTAINS("\"ERR\":0", ws_transport_stub_last_sent(conn));
    STRCMP_CONTAINS("\"RD\":[42]", ws_transport_stub_last_sent(conn));
}
