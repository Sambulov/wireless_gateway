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
    char frame[2048];
    int n = snprintf(frame, sizeof(frame), "{\"FID\":\"%s\",\"SID\":%d,\"ARG\":%s}", fid, sid, arg);
    CHECK(n > 0 && n < (int)sizeof(frame));          /* кадр не обрезан */
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

/* ── запись регистров ────────────────────────────────────────────────────── */

TEST(ApihModbus, WriteRegistersGoToTheLineAsGiven) {
    client_request(api, conn, 1, "{\"ADR\":1,\"FN\":16,\"RA\":0,\"RVC\":2,\"RD\":[1,2]}");
    static const uint8_t req[] = { 0x01, 0x10, 0x00, 0x00, 0x00, 0x02, 0x04,
                                   0x00, 0x01, 0x00, 0x02, 0x23, 0xAE };
    LONGS_EQUAL(sizeof(req), gw_uart_stub_last_write_len(line));
    MEMCMP_EQUAL(req, gw_uart_stub_last_write_buf(line), sizeof(req));
}

/* Регрессия: данные записи разбирались в массив на стеке разбора, а запрос
 * хранил указатель на него. Запись, ждущая линию в очереди, уходила с тем,
 * что к тому времени лежало на стеке. */
static void clobber_stack(void) {
    volatile uint8_t junk[1024];
    for (unsigned i = 0; i < sizeof(junk); i++) junk[i] = 0xCC;
    (void)junk[0];
}

TEST(ApihModbus, QueuedWriteKeepsItsData) {
    ws_transport_stub_conn_t *conn2 = ws_transport_stub_connect(2);
    client_request(api, conn, 1, "{\"ADR\":1,\"FN\":3,\"RA\":0,\"RVC\":1}");
    client_request(api, conn2, 2, "{\"ADR\":1,\"FN\":16,\"RA\":0,\"RVC\":2,\"RD\":[1,2]}");
    clobber_stack();

    static const uint8_t resp[] = { 0x01, 0x03, 0x02, 0x00, 0x2A, 0x39, 0x9B };
    gw_uart_stub_receive(line, resp, sizeof(resp), 1);
    for (int i = 0; i < 4; i++) {
        system_stub_advance_time(10);
        api_worker(api);
    }

    static const uint8_t req[] = { 0x01, 0x10, 0x00, 0x00, 0x00, 0x02, 0x04,
                                   0x00, 0x01, 0x00, 0x02, 0x23, 0xAE };
    LONGS_EQUAL(2, gw_uart_stub_write_calls(line));
    MEMCMP_EQUAL(req, gw_uart_stub_last_write_buf(line), sizeof(req));
}

/* ── разбор ARG ──────────────────────────────────────────────────────────── */

/* Отказ разбора: клиенту ERR 2, в линию ничего не уходит. */
static void expect_arg_refused(api_t *api, ws_transport_stub_conn_t *conn, void *line,
                               const char *arg) {
    client_request(api, conn, 7, arg);
    STRCMP_CONTAINS("\"ERR\":2", ws_transport_stub_last_sent(conn));
    LONGS_EQUAL(0, gw_uart_stub_write_calls(line));
}

/* Регрессия: нечисловой элемент RD молча становился нулём и уходил в
 * регистр прибора. */
TEST(ApihModbus, NonIntegerRegisterValueIsRefused) {
    expect_arg_refused(api, conn, line, "{\"ADR\":1,\"FN\":16,\"RA\":0,\"RVC\":3,\"RD\":[100,\"x\",3]}");
}

TEST(ApihModbus, RegisterValueOutOfRangeIsRefused) {
    expect_arg_refused(api, conn, line, "{\"ADR\":1,\"FN\":16,\"RA\":0,\"RVC\":1,\"RD\":[70000]}");
}

TEST(ApihModbus, NegativeRegisterValueIsRefused) {
    expect_arg_refused(api, conn, line, "{\"ADR\":1,\"FN\":16,\"RA\":0,\"RVC\":1,\"RD\":[-1]}");
}

TEST(ApihModbus, RegisterAddressOutOfRangeIsRefused) {
    expect_arg_refused(api, conn, line, "{\"ADR\":1,\"FN\":3,\"RA\":70000,\"RVC\":1}");
}

TEST(ApihModbus, RegisterCountOutOfRangeIsRefused) {
    expect_arg_refused(api, conn, line, "{\"ADR\":1,\"FN\":3,\"RA\":0,\"RVC\":70000}");
}

/* AWT усекался до uint16: 65536 давал таймаут 0, 70000 — 4,4 с. */
TEST(ApihModbus, AnswerWaitTimeOutOfRangeIsRefused) {
    expect_arg_refused(api, conn, line, "{\"ADR\":1,\"FN\":3,\"RA\":0,\"RVC\":1,\"AWT\":65536}");
}

TEST(ApihModbus, WriteRegistersCountMustMatchTheValues) {
    expect_arg_refused(api, conn, line, "{\"ADR\":1,\"FN\":16,\"RA\":0,\"RVC\":3,\"RD\":[1,2]}");
}

/* Запись 100 регистров: раньше разбор ARG упирался в 48 токенов. */
TEST(ApihModbus, WriteOfHundredRegistersGoesToTheLine) {
    char arg[1024];
    int off = snprintf(arg, sizeof(arg), "{\"ADR\":1,\"FN\":16,\"RA\":0,\"RVC\":100,\"RD\":[");
    for (int i = 0; i < 100; i++)
        off += snprintf(arg + off, sizeof(arg) - off, "%s%d", i ? "," : "", i);
    snprintf(arg + off, sizeof(arg) - off, "]}");
    client_request(api, conn, 1, arg);
    LONGS_EQUAL(1, gw_uart_stub_write_calls(line));
    LONGS_EQUAL(7 + 200 + 2, gw_uart_stub_last_write_len(line));
}

/* ── границы диапазонов: крайние допустимые значения проходят ───────────── */

TEST(ApihModbus, RegisterValueAtTheTopOfTheRangeIsAccepted) {
    client_request(api, conn, 1, "{\"ADR\":1,\"FN\":16,\"RA\":65535,\"RVC\":1,\"RD\":[65535]}");
    LONGS_EQUAL(1, gw_uart_stub_write_calls(line));
    const uint8_t *b = gw_uart_stub_last_write_buf(line);
    LONGS_EQUAL(0xFF, b[2]); LONGS_EQUAL(0xFF, b[3]);          /* RA */
    LONGS_EQUAL(0xFF, b[7]); LONGS_EQUAL(0xFF, b[8]);          /* значение */
}

TEST(ApihModbus, AnswerWaitTimeAtTheLimitIsAccepted) {
    client_request(api, conn, 1, "{\"ADR\":1,\"FN\":3,\"RA\":0,\"RVC\":1,\"AWT\":60000}");
    LONGS_EQUAL(1, gw_uart_stub_write_calls(line));
}

TEST(ApihModbus, SingleRegisterValueAtTheTopIsAccepted) {
    client_request(api, conn, 1, "{\"ADR\":1,\"FN\":6,\"RA\":1,\"RVC\":65535}");
    LONGS_EQUAL(1, gw_uart_stub_write_calls(line));
    const uint8_t *b = gw_uart_stub_last_write_buf(line);
    LONGS_EQUAL(0xFF, b[4]); LONGS_EQUAL(0xFF, b[5]);
}

/* Катушки (FN 15): значения RD — упакованные байты, 0..255. */
TEST(ApihModbus, CoilByteRangeIsChecked) {
    client_request(api, conn, 1, "{\"ADR\":1,\"FN\":15,\"RA\":0,\"RVC\":8,\"RD\":[255]}");
    LONGS_EQUAL(1, gw_uart_stub_write_calls(line));
}

TEST(ApihModbus, CoilByteOverTheRangeIsRefused) {
    expect_arg_refused(api, conn, line, "{\"ADR\":1,\"FN\":15,\"RA\":0,\"RVC\":8,\"RD\":[256]}");
}

/* Запись регистров без RVC: число в кадре — число значений, а не 0. */
TEST(ApihModbus, WriteRegistersWithoutCountTakesItFromTheValues) {
    client_request(api, conn, 1, "{\"ADR\":1,\"FN\":16,\"RA\":0,\"RD\":[1,2]}");
    static const uint8_t req[] = { 0x01, 0x10, 0x00, 0x00, 0x00, 0x02, 0x04,
                                   0x00, 0x01, 0x00, 0x02, 0x23, 0xAE };
    LONGS_EQUAL(sizeof(req), gw_uart_stub_last_write_len(line));
    MEMCMP_EQUAL(req, gw_uart_stub_last_write_buf(line), sizeof(req));
}

/* Предел записи — по кадру RTU (255 байт: 6 + 2n + 3 для FN 16), то есть 123
 * регистра, как и в спецификации Modbus. Больше — ошибка аргументов (ERR 2)
 * сразу, а не «не стартовал» (ERR 3) после очереди. */
static void write_regs(api_t *api, ws_transport_stub_conn_t *conn, int n) {
    char arg[1200];
    int off = snprintf(arg, sizeof(arg), "{\"ADR\":1,\"FN\":16,\"RA\":0,\"RD\":[");
    for (int i = 0; i < n; i++)
        off += snprintf(arg + off, sizeof(arg) - off, "%s%d", i ? "," : "", i);
    snprintf(arg + off, sizeof(arg) - off, "]}");
    client_request(api, conn, 1, arg);
}

TEST(ApihModbus, WriteOf123RegistersIsTheLimit) {
    write_regs(api, conn, 123);
    LONGS_EQUAL(1, gw_uart_stub_write_calls(line));
    LONGS_EQUAL(6 + 1 + 246 + 2, gw_uart_stub_last_write_len(line));
}

TEST(ApihModbus, WriteOf124RegistersIsAnArgumentError) {
    write_regs(api, conn, 124);
    STRCMP_CONTAINS("\"ERR\":2", ws_transport_stub_last_sent(conn));
    LONGS_EQUAL(0, gw_uart_stub_write_calls(line));
}

/* В ответе RTU на чтение адреса регистра нет — RA ответа клиенту обязан
 * повторять запрос. Раньше уходило RA из кадра ответа, то есть 0: страница,
 * раскладывающая значения по RA (innovert), писала каждый диапазон в регистры
 * с нуля — значения «прыгали», а настоящие регистры не обновлялись. */
TEST(ApihModbus, ReplyCarriesTheRequestedRegisterAddress) {
    client_request(api, conn, 5, "{\"ADR\":1,\"FN\":3,\"RA\":100,\"RVC\":2}");
    static const uint8_t req[] = { 0x01, 0x03, 0x00, 0x64, 0x00, 0x02, 0x85, 0xD4 };
    MEMCMP_EQUAL(req, gw_uart_stub_last_write_buf(line), sizeof(req));

    static const uint8_t resp[] = { 0x01, 0x03, 0x04, 0x00, 0x07, 0x00, 0x08, 0x4A, 0x34 };
    gw_uart_stub_receive(line, resp, sizeof(resp), 1);
    system_stub_advance_time(10);
    api_worker(api);

    STRCMP_CONTAINS("\"RA\":100", ws_transport_stub_last_sent(conn));
    STRCMP_CONTAINS("\"RC\":2", ws_transport_stub_last_sent(conn));
    STRCMP_CONTAINS("\"RD\":[7,8]", ws_transport_stub_last_sent(conn));
}
