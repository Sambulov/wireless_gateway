/* Служба Modbus (main/apih_modbus.c) через настоящее ядро API.
 *
 * Клиент — кадр в стаб-транспорт плюс такт ядра, линия — заглушка драйвера
 * UART: что служба отправила в линию, видно по gw_uart_stub_last_write_buf, а
 * ответ прибора тест подаёт в линию порта (gw_uart_stub_receive) — его
 * получает тот, кто подписан на приём этого порта. Время —
 * system_stub_advance_time. */
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
}

/* "uartN.modbus" — линия аппаратного порта N, как и у "uartN.tx/rx/config":
 * дескриптор app.uart.port[N]. Воркер 0 — "uart1.modbus". */
#define MB_FID   ESP_WS_API_UART1_MODBUS_REQ

static void client_request_fid(api_t *api, ws_transport_stub_conn_t *conn,
                               const char *fid, int sid, const char *arg) {
    char frame[256];
    snprintf(frame, sizeof(frame), "{\"FID\":\"%s\",\"SID\":%d,\"ARG\":%s}", fid, sid, arg);
    ws_transport_stub_frame(conn, WS_STUB_FRAME_TEXT, frame, strlen(frame), 1);
    api_worker(api);
}

static void client_request(api_t *api, ws_transport_stub_conn_t *conn,
                           int sid, const char *arg) {
    client_request_fid(api, conn, MB_FID, sid, arg);
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
        line = &app.uart.port[1].desc;
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
    gw_uart_stub_receive(line, resp, sizeof(resp), 1);
    system_stub_advance_time(10);
    api_worker(api);

    STRCMP_CONTAINS("\"ERR\":0", ws_transport_stub_last_sent(conn));
    STRCMP_CONTAINS("\"RD\":[42]", ws_transport_stub_last_sent(conn));
}

/* Запрос, кадр которого драйвер собрать не может (код функции вне таблицы
 * библиотеки): клиент получает отказ «не стартовал», а очередь порта не
 * стоит — следующий запрос уходит в линию. Раньше такой запрос навсегда
 * оставался во главе очереди, и порт был мёртв до отключения клиента. */
TEST(ApihModbus, UnbuildableRequestIsRefusedAndDoesNotBlockThePort) {
    client_request(api, conn, 1, "{\"ADR\":1,\"FN\":100,\"RA\":0,\"RVC\":1}");
    api_worker(api);
    STRCMP_CONTAINS("\"SID\":\"0x00000001\"", ws_transport_stub_last_sent(conn));
    STRCMP_CONTAINS("\"ERR\":3", ws_transport_stub_last_sent(conn));
    LONGS_EQUAL(0, gw_uart_stub_write_calls(line));

    client_request(api, conn, 2, "{\"ADR\":1,\"FN\":3,\"RA\":0,\"RVC\":1}");
    LONGS_EQUAL(1, gw_uart_stub_write_calls(line));      /* линия свободна */
}

/* Тот же запрос, но в очереди за идущей транзакцией: когда линия
 * освободится, он получает отказ, а следующий за ним уходит в линию. */
TEST(ApihModbus, UnbuildableRequestInTheQueueIsSkipped) {
    ws_transport_stub_conn_t *conn2 = ws_transport_stub_connect(2);
    ws_transport_stub_conn_t *conn3 = ws_transport_stub_connect(3);
    client_request(api, conn, 1, "{\"ADR\":1,\"FN\":3,\"RA\":0,\"RVC\":1}");
    LONGS_EQUAL(1, gw_uart_stub_write_calls(line));     /* первая — на линии */

    client_request(api, conn2, 2, "{\"ADR\":1,\"FN\":100,\"RA\":0,\"RVC\":1}");
    client_request(api, conn3, 3, "{\"ADR\":2,\"FN\":3,\"RA\":0,\"RVC\":1}");
    LONGS_EQUAL(1, gw_uart_stub_write_calls(line));     /* обе ждут линию */

    static const uint8_t resp[] = { 0x01, 0x03, 0x02, 0x00, 0x2A, 0x39, 0x9B };
    gw_uart_stub_receive(line, resp, sizeof(resp), 1);
    for (int i = 0; i < 4; i++) {
        system_stub_advance_time(10);
        api_worker(api);
    }

    STRCMP_CONTAINS("\"RD\":[42]", ws_transport_stub_last_sent(conn));
    STRCMP_CONTAINS("\"ERR\":3", ws_transport_stub_last_sent(conn2));
    LONGS_EQUAL(2, gw_uart_stub_write_calls(line));     /* третья — на линии */
    LONGS_EQUAL(0x02, gw_uart_stub_last_write_buf(line)[0]);
}

/* Номер порта в имени FID — аппаратный, как у остальных служб UART: кадр
 * "uartN.modbus" уходит в UART N, а консоль (UART0) Modbus не трогает. */
TEST(ApihModbus, FidPortNumberIsTheHardwarePort) {
    static const uint8_t req[] = { 0x01, 0x03, 0x00, 0x00, 0x00, 0x01, 0x84, 0x0A };

    client_request_fid(api, conn, ESP_WS_API_UART1_MODBUS_REQ, 1,
                       "{\"ADR\":1,\"FN\":3,\"RA\":0,\"RVC\":1}");
    client_request_fid(api, conn, ESP_WS_API_UART2_MODBUS_REQ, 2,
                       "{\"ADR\":1,\"FN\":3,\"RA\":0,\"RVC\":1}");

    LONGS_EQUAL(0, gw_uart_stub_write_calls(&app.uart.port[0].desc));   /* консоль */
    LONGS_EQUAL(1, gw_uart_stub_write_calls(&app.uart.port[1].desc));
    LONGS_EQUAL(1, gw_uart_stub_write_calls(&app.uart.port[2].desc));
    MEMCMP_EQUAL(req, gw_uart_stub_last_write_buf(&app.uart.port[2].desc), sizeof(req));
}
