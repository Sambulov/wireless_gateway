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
#include <stdlib.h>
#include <string.h>

extern "C" {
    void apih_uart_test_init(api_t *api, app_context_t *app);
    void apih_uart_test_reset(void);
    void apih_uart_test_fill_rx(int port, const uint8_t *data, uint32_t len);
    void apih_uart_test_fill_rx_idle(int port, const uint8_t *data, uint32_t len,
                                     uint32_t gap_us, uint8_t idle);
}

/* ── Клиентская сторона ───────────────────────────────────────────────────
 *
 * Сервис UART обслуживается ТАКТОМ ЯДРА: его FID зарегистрированы обработчиками
 * (apih_uart_test_init делает то же, что ws_uart_run в прошивке), а клиент — это
 * кадр из транспорта плюс вызов api_worker. Поэтому тест работает ровно тем
 * путём, что и настоящий клиент: кадр → ядро → обработчик сервиса → ответ в
 * транспорт (ws_transport_stub_last_sent).
 *
 * Очередей ни у ядра, ни у сервиса нет: запрос доставляется вызовом обработчика,
 * ответ уходит по дескриптору вызова (api_reply), а кадр приёма порта —
 * публичной рассылкой по имени FID (api_broadcast). */

/* Кадр клиента: {"FID":…,"FLAGS":…,"SID":…[,"ARG":…]} + один такт ядра. */
static void client_frame(api_t *api, ws_transport_stub_conn_t *conn,
                         const char *fid, int flags, int sid, const char *arg = NULL) {
    char frame[256];
    if (arg)
        snprintf(frame, sizeof(frame),
                 "{\"FID\":\"%s\",\"FLAGS\":%d,\"SID\":%d,\"ARG\":%s}", fid, flags, sid, arg);
    else
        snprintf(frame, sizeof(frame),
                 "{\"FID\":\"%s\",\"FLAGS\":%d,\"SID\":%d}", fid, flags, sid);
    ws_transport_stub_frame(conn, WS_STUB_FRAME_TEXT, frame, strlen(frame), 1);
    api_worker(api);
}

/* Разовый запрос клиента. */
static void client_request(api_t *api, ws_transport_stub_conn_t *conn,
                           const char *fid, int sid, const char *arg = NULL) {
    client_frame(api, conn, fid, 0, sid, arg);
}

/* Подписка (FLAGS:4): данные порта сервис отдаёт рассылкой по FID, а рассылка
 * находит только живые вызовы этого FID. */
static void client_subscription(api_t *api, ws_transport_stub_conn_t *conn,
                                const char *fid, int sid) {
    client_frame(api, conn, fid, 4, sid, NULL);
}

/* Такт ядра без нового кадра: по нему сервис отдаёт накопленное (подписка
 * переспрашивается, когда наступает её срок). */
static void worker_tick(api_t *api, uint32_t advance_ms = 0) {
    if (advance_ms)
        system_stub_advance_time(advance_ms);
    api_worker(api);
}

static int count_char(const char *s, char c) {
    int n = 0;
    for (; s && *s; s++) if (*s == c) n++;
    return n;
}

/* ── Test fixture ─────────────────────────────────────────────────────── */

/* Ядро и одно соединение у каждого теста. Порядок снятия: сначала транспорт (он
 * доводит до конца отложенные отправки, и ядро освобождает свои буферы), потом
 * само ядро — api_deinit отписывается от транспорта. */

#define UART_SETUP()                                  \
    do {                                              \
        memset(&app, 0, sizeof(app));                 \
        system_stub_reset();                          \
        ws_transport_stub_reset();                    \
        gw_uart_stub_reset();                         \
        api = ws_transport_stub_init();               \
        conn = ws_transport_stub_connect(1);          \
        app.api_ap = api;                             \
        app.api_sta = NULL;                           \
        apih_uart_test_init(api, &app);               \
    } while (0)

#define UART_TEARDOWN()                               \
    do {                                              \
        apih_uart_test_reset();                       \
        ws_transport_stub_reset();                    \
        api_deinit(api);                              \
    } while (0)

TEST_GROUP(ApihUartCnf) {
    app_context_t app;
    /* Своё ядро API у каждого теста: сервис берёт его из app.api_ap, как в
     * прошивке (второго ядра — станции — здесь нет: UART проверяется на одном). */
    api_t *api;
    ws_transport_stub_conn_t *conn;
    void setup() { UART_SETUP(); }
    void teardown() { UART_TEARDOWN(); }
};

TEST_GROUP(ApihUartTx) {
    app_context_t app;
    api_t *api;
    ws_transport_stub_conn_t *conn;
    void setup() { UART_SETUP(); }
    void teardown() { UART_TEARDOWN(); }
};

TEST_GROUP(ApihUartRx) {
    app_context_t app;
    api_t *api;
    ws_transport_stub_conn_t *conn;
    void setup() { UART_SETUP(); }
    void teardown() { UART_TEARDOWN(); }
};

TEST_GROUP(ApihUartEcho) {
    app_context_t app;
    api_t *api;
    ws_transport_stub_conn_t *conn;
    void setup() { UART_SETUP(); }
    void teardown() { UART_TEARDOWN(); }
};

/* ── CNF tests ────────────────────────────────────────────────────────── */

TEST(ApihUartCnf, BaudRateSetOnPort1) {
    client_request(api, conn, ESP_WS_API_UART1_CNF, 2, "{\"BR\":9600}");
    LONGS_EQUAL(9600, gw_uart_stub_last_config(&app.uart.port[0].desc).boud);
}

TEST(ApihUartCnf, BaudRateSetOnPort2) {
    client_request(api, conn, ESP_WS_API_UART2_CNF, 2, "{\"BR\":115200}");
    LONGS_EQUAL(115200, gw_uart_stub_last_config(&app.uart.port[1].desc).boud);
}

TEST(ApihUartCnf, ParitySetOnPort1) {
    client_request(api, conn, ESP_WS_API_UART1_CNF, 2, "{\"PAR\":1}");
    LONGS_EQUAL(GW_UART_PARITY_ODD, gw_uart_stub_last_config(&app.uart.port[0].desc).parity);
}

TEST(ApihUartCnf, NullDataDoesNotCallSet) {
    /* Вызов без ARG — чтение: применять нечего, на порту значение не меняется. */
    client_request(api, conn, ESP_WS_API_UART1_CNF, 2);
    LONGS_EQUAL(0, gw_uart_stub_last_config(&app.uart.port[0].desc).boud);
}

/* Вызов без ARG — это чтение: применять нечего, но ответить обязательно, иначе
 * клиент ждёт до своего таймаута. */
TEST(ApihUartCnf, ReadWithoutArgAnswersCurrentState) {
    client_request(api, conn, ESP_WS_API_UART2_CNF, 1, "{\"BR\":9600,\"MODE\":0}");

    client_request(api, conn, ESP_WS_API_UART2_CNF, 2);
    STRCMP_CONTAINS("\"FID\":\"uart2.config\"", ws_transport_stub_last_sent(conn));
    STRCMP_CONTAINS("\"BR\":9600", ws_transport_stub_last_sent(conn));
    STRCMP_CONTAINS("\"MODE\":0", ws_transport_stub_last_sent(conn));
}

/* Неразобранный ARG применять нельзя, но и молчать нечем: отвечаем состоянием. */
TEST(ApihUartCnf, BrokenArgAnswersCurrentState) {
    client_request(api, conn, ESP_WS_API_UART1_CNF, 2, "not json");

    STRCMP_CONTAINS("\"FID\":\"uart1.config\"", ws_transport_stub_last_sent(conn));
    STRCMP_CONTAINS("\"BR\":0", ws_transport_stub_last_sent(conn));
    /* И ничего не применили: на порту остаётся нулевое значение */
    LONGS_EQUAL(0, gw_uart_stub_last_config(&app.uart.port[0].desc).boud);
}

TEST(ApihUartCnf, ResponseGoesToClient) {
    /* Ответ сервиса доходит до клиента сразу: обработчик отвечает через
     * дескриптор вызова, ядро оборачивает его в конверт и отправляет. Смена
     * настроек — ещё и рассылка подписчикам (без SID: кадр адресован не вызову,
     * а имени FID), поэтому отправок за такт больше одной. */
    const int before = ws_transport_stub_send_attempts(conn);
    client_request(api, conn, ESP_WS_API_UART1_CNF, 2, "{\"BR\":4800}");
    CHECK(ws_transport_stub_send_attempts(conn) >= before + 2);   /* DELIVERED + ответ */
    STRCMP_CONTAINS("\"FID\":\"uart1.config\"", ws_transport_stub_last_sent(conn));
    STRCMP_CONTAINS("\"BR\":4800", ws_transport_stub_last_sent(conn));
}

/* Запись ТЕХ ЖЕ значений настроек ничего не меняет — рассылки нет: клиент
 * получает только ответ на свой вызов (DELIVERED и ответ с его SID). */
TEST(ApihUartCnf, SameValuesDoNotBroadcast) {
    client_request(api, conn, ESP_WS_API_UART1_CNF, 1, "{\"BR\":4800}");
    const int before = ws_transport_stub_send_attempts(conn);
    client_request(api, conn, ESP_WS_API_UART1_CNF, 2, "{\"BR\":4800}");
    LONGS_EQUAL(before + 2, ws_transport_stub_send_attempts(conn));
    STRCMP_CONTAINS("\"SID\":\"0x00000002\"", ws_transport_stub_last_sent(conn));
}

TEST(ApihUartCnf, WordLengthSetOnPort1) {
    client_request(api, conn, ESP_WS_API_UART1_CNF, 2, "{\"WL\":0}");   /* 7 бит */
    LONGS_EQUAL(GW_UART_WORD_7BIT, gw_uart_stub_last_config(&app.uart.port[0].desc).bits);
}

/* Однопроводный режим (MODE 2): приём глушится программно, потому что драйверу
 * тут разворачивать нечего. Значение не влезало в однобитное поле команды. */
TEST(ApihUartCnf, SingleWireModeAccepted) {
    client_request(api, conn, ESP_WS_API_UART2_CNF, 2, "{\"MODE\":2}");
    LONGS_EQUAL(GW_UART_MODE_UART_SINGLE_WIRE,
                gw_uart_stub_last_config(&app.uart.port[1].desc).mode);
}

TEST(ApihUartCnf, UnknownModeNotApplied) {
    client_request(api, conn, ESP_WS_API_UART2_CNF, 1, "{\"MODE\":2}");
    client_request(api, conn, ESP_WS_API_UART2_CNF, 2, "{\"MODE\":3}");
    /* Режим 3 не существует: на порту остаётся текущий, а не «ноль». */
    LONGS_EQUAL(GW_UART_MODE_UART_SINGLE_WIRE,
                gw_uart_stub_last_config(&app.uart.port[1].desc).mode);
}

/* Порог простоя линии: по нему драйвер отмечает конец кадра, а страница делит
 * поток данных порта на кадры (протоколы на стороне веб). */
TEST(ApihUartCnf, IdleCharsSetAndReported) {
    client_request(api, conn, ESP_WS_API_UART2_CNF, 2, "{\"IDLE_CH\":8}");
    LONGS_EQUAL(8, gw_uart_stub_last_config(&app.uart.port[1].desc).idle_ch);
    STRCMP_CONTAINS("\"IDLE_CH\":8", ws_transport_stub_last_sent(conn));
}

TEST(ApihUartCnf, IdleCharsOutOfRangeNotApplied) {
    client_request(api, conn, ESP_WS_API_UART2_CNF, 2, "{\"IDLE_CH\":0}");
    /* Ноль знаков — не порог: на порту остаётся текущее (в стабе — ноль) */
    LONGS_EQUAL(0, gw_uart_stub_last_config(&app.uart.port[1].desc).idle_ch);
}

/* ── TX tests ─────────────────────────────────────────────────────────── */

TEST(ApihUartTx, ValidBase64WritesToPort1) {
    /* base64("Hello") = "SGVsbG8=" */
    client_request(api, conn, ESP_WS_API_UART1_RAW_TX, 2, "\"SGVsbG8=\"");
    LONGS_EQUAL(1, gw_uart_stub_write_calls(&app.uart.port[0].desc));
    LONGS_EQUAL(5, gw_uart_stub_last_write_len(&app.uart.port[0].desc));
    MEMCMP_EQUAL("Hello", gw_uart_stub_last_write_buf(&app.uart.port[0].desc), 5);
}

TEST(ApihUartTx, ValidBase64WritesToPort2) {
    client_request(api, conn, ESP_WS_API_UART2_RAW_TX, 2, "\"SGVsbG8=\"");
    LONGS_EQUAL(1, gw_uart_stub_write_calls(&app.uart.port[1].desc));
}

TEST(ApihUartTx, NullDataDoesNotWrite) {
    /* Без ARG передавать нечего: обработчик отвечает FALSE сразу и молча — в
     * порт ничего не уходит и вызов не остаётся висеть. */
    client_request(api, conn, ESP_WS_API_UART1_RAW_TX, 2);
    LONGS_EQUAL(0, gw_uart_stub_write_calls(&app.uart.port[0].desc));
    worker_tick(api, 20);
    LONGS_EQUAL(0, gw_uart_stub_write_calls(&app.uart.port[0].desc));
}

TEST(ApihUartTx, ResponseSentAfterWrite) {
    client_request(api, conn, ESP_WS_API_UART1_RAW_TX, 2, "\"SGVsbG8=\"");
    LONGS_EQUAL(1, gw_uart_stub_write_calls(&app.uart.port[0].desc));
    STRCMP_CONTAINS("\"written\":5", ws_transport_stub_last_sent(conn));
}

/* ── RX tests ─────────────────────────────────────────────────────────── */

/* Пустой опрос: данных нет — кадра нет. Подписка при этом жива: следующий такт
 * снова спрашивает накопитель. */
TEST(ApihUartRx, EmptyRingSendsNoFrame) {
    client_subscription(api, conn, ESP_WS_API_UART1_RAW_RX, 1);
    const int before = ws_transport_stub_send_attempts(conn);

    worker_tick(api, 20);
    LONGS_EQUAL(before, ws_transport_stub_send_attempts(conn));
}

TEST(ApihUartRx, FilledBufferSendsBase64Response) {
    client_subscription(api, conn, ESP_WS_API_UART1_RAW_RX, 1);

    const uint8_t payload[] = "Hello";
    apih_uart_test_fill_rx(0, payload, 5);
    worker_tick(api, 100);

    /* Данные порта уходят подписчикам рассылкой по FID — кадр в транспорте.
     * '#' на конце — признак «передача на линии закончилась» (fill_rx так и
     * кладёт данные: кусок с концом передачи). */
    STRCMP_CONTAINS("\"RX\":\"SGVsbG8=#\"", ws_transport_stub_last_sent(conn));
}

/* Кусок приёма, закончившийся на линии, отдаётся с разделителем '#': несколько
 * передач, попавших в один кадр отдачи, клиент по нему и разделяет. Кусок без
 * разделителя — продолжение предыдущего. */
TEST(ApihUartRx, CompletedPiecesAreDelimitedByHash) {
    client_subscription(api, conn, ESP_WS_API_UART2_RAW_RX, 1);

    apih_uart_test_fill_rx_idle(1, (const uint8_t *)"AB", 2, 0, 1);
    apih_uart_test_fill_rx_idle(1, (const uint8_t *)"AB", 2, 0, 1);
    worker_tick(api, 100);

    /* base64("AB") = "QUI=", а '#' — конец передачи на линии. */
    STRCMP_CONTAINS("\"RX\":\"QUI=#QUI=#\"", ws_transport_stub_last_sent(conn));
}

/* Незакрытый кусок уходит только тогда, когда перевалил за порог частичной
 * выгрузки, и уходит БЕЗ разделителя — значит кадр продолжается. Вслед за ним
 * приходит остаток, уже закрытый разделителем. */
TEST(ApihUartRx, ContinuationGoesWithoutDelimiter) {
    client_subscription(api, conn, ESP_WS_API_UART2_RAW_RX, 1);
    static uint8_t long_data[1200];
    memset(long_data, 'x', sizeof(long_data));

    apih_uart_test_fill_rx_idle(1, long_data, sizeof(long_data), 0, 0);
    apih_uart_test_fill_rx_idle(1, (const uint8_t *)"EF", 2, 0, 1);
    worker_tick(api, 100);

    const char *sent = ws_transport_stub_last_sent(conn);
    /* В сообщении два куска: первый — ровно порог, без разделителя (кадр
     * продолжается); второй — остаток, закрытый разделителем. Значит
     * разделитель в сообщении РОВНО один. */
    LONGS_EQUAL(1, count_char(sent, '#'));
    STRCMP_CONTAINS("#\"}", sent);
    CHECK(strlen(sent) > 1000);
}

TEST(ApihUartRx, BufferClearedAfterRead) {
    client_subscription(api, conn, ESP_WS_API_UART2_RAW_RX, 1);

    const uint8_t payload[] = "Hi";
    apih_uart_test_fill_rx(1, payload, 2);
    worker_tick(api, 100);
    const int after_first = ws_transport_stub_send_attempts(conn);

    /* Второй опрос того же порта: кольцо пусто — данных нет, кадра нет. */
    worker_tick(api, 100);
    LONGS_EQUAL(after_first, ws_transport_stub_send_attempts(conn));
}

/* ── Темп кадров приёма ───────────────────────────────────────────────────
 *
 * Данные уходят кадром не чаще, чем раз в UART_RX_FRAME_MS (75 мс): при
 * предельной скорости порта (500000 бод = 50000 Б/с) за этот срок накапливается
 * 3750 байт — меньше накопителя (4096), то есть копить можно без потерь, а
 * кадров в разы меньше (иначе эхо набора в терминале давало кадр на символ).
 *
 * Темп — не ожидание: когда срок не вышел, обработчик возвращает TRUE, и ядро
 * позовёт его снова на следующем такте. Ни одной задержки в системе при этом не
 * запрашивается (system_stub_last_delay это и проверяет). */

TEST(ApihUartRx, SecondFrameWaitsForTheRateGate) {
    client_subscription(api, conn, ESP_WS_API_UART2_RAW_RX, 1);
    const int before = ws_transport_stub_send_attempts(conn);

    /* Первый кадр уходит сразу — ждать нечего */
    const uint8_t first[] = "one";
    apih_uart_test_fill_rx(1, first, 3);
    worker_tick(api, 100);
    LONGS_EQUAL(before + 1, ws_transport_stub_send_attempts(conn));

    /* Кадр сразу за ним не уходит: темп не вышел. Обработчик вернул TRUE —
     * вызов жив и ждёт следующего такта, а не спит. */
    const uint8_t second[] = "two";
    apih_uart_test_fill_rx(1, second, 3);
    worker_tick(api, 10);
    LONGS_EQUAL(before + 1, ws_transport_stub_send_attempts(conn));
    LONGS_EQUAL(0, system_stub_last_delay());

    /* Срок вышел — данные уходят следующим тактом. */
    worker_tick(api, 80);
    LONGS_EQUAL(before + 2, ws_transport_stub_send_attempts(conn));
}

TEST(ApihUartRx, RateGateIsPerPort) {
    /* Темп считается на КАЖДЫЙ порт отдельно: кадр второго порта не задерживает
     * первый. */
    client_subscription(api, conn, ESP_WS_API_UART1_RAW_RX, 1);
    client_subscription(api, conn, ESP_WS_API_UART2_RAW_RX, 2);
    const int before = ws_transport_stub_send_attempts(conn);

    const uint8_t data[] = "x";
    apih_uart_test_fill_rx(0, data, 1);
    worker_tick(api, 100);
    LONGS_EQUAL(before + 1, ws_transport_stub_send_attempts(conn));

    apih_uart_test_fill_rx(1, data, 1);
    worker_tick(api, 10);
    LONGS_EQUAL(before + 2, ws_transport_stub_send_attempts(conn));
    LONGS_EQUAL(0, system_stub_last_delay());
}

/* ── Echo tests ───────────────────────────────────────────────────────── */

TEST(ApihUartEcho, SetEchoOnPort1) {
    client_request(api, conn, ESP_WS_API_UART1_ECHO, 2, "{\"E\":1}");
    LONGS_EQUAL(1, gw_uart_get_echo(&app.uart.port[0].desc));
}

TEST(ApihUartEcho, ClearEchoOnPort2) {
    gw_uart_set_echo(&app.uart.port[1].desc, 1);
    client_request(api, conn, ESP_WS_API_UART2_ECHO, 2, "{\"E\":0}");
    LONGS_EQUAL(0, gw_uart_get_echo(&app.uart.port[1].desc));
}

TEST(ApihUartEcho, QuerySendsResponse) {
    client_request(api, conn, ESP_WS_API_UART1_ECHO, 2);
    STRCMP_CONTAINS("\"E\":0", ws_transport_stub_last_sent(conn));
}

TEST(ApihUartEcho, UnknownFidNoResponse) {
    /* Порта "uart3" нет: такого FID на ядре не зарегистрировано — клиент получает
     * NO_HANDLER, а не тишину. */
    const int before = ws_transport_stub_send_attempts(conn);
    client_frame(api, conn, "uart3.rx", 0, 1);
    LONGS_EQUAL(before + 1, ws_transport_stub_send_attempts(conn));
}
