extern "C" {
#include "app.h"
#include "api.h"
#include "platform.h"
#include "uart.h"
}
#include "system_stub.h"
extern "C" {
#include "Converters/Base64Encoding.h"   /* base64_decode: сверяем содержимое кадра */
}
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

static int count_substr(const char *s, const char *sub) {
    int n = 0;
    size_t len = strlen(sub);
    for (const char *p = s; (p = strstr(p, sub)) != NULL; p += len)
        n++;
    return n;
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
    client_request(api, conn, ESP_WS_API_UART0_CNF, 2, "{\"BR\":9600}");
    LONGS_EQUAL(9600, gw_uart_stub_last_config(&app.uart.port[0].desc).boud);
}

TEST(ApihUartCnf, BaudRateSetOnPort2) {
    client_request(api, conn, ESP_WS_API_UART2_CNF, 2, "{\"BR\":115200}");
    LONGS_EQUAL(115200, gw_uart_stub_last_config(&app.uart.port[2].desc).boud);
}

TEST(ApihUartCnf, ParitySetOnPort1) {
    client_request(api, conn, ESP_WS_API_UART0_CNF, 2, "{\"PAR\":1}");
    LONGS_EQUAL(GW_UART_PARITY_ODD, gw_uart_stub_last_config(&app.uart.port[0].desc).parity);
}

TEST(ApihUartCnf, NullDataDoesNotCallSet) {
    /* Вызов без ARG — чтение: применять нечего, на порту значение не меняется. */
    client_request(api, conn, ESP_WS_API_UART0_CNF, 2);
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
    client_request(api, conn, ESP_WS_API_UART0_CNF, 2, "not json");

    STRCMP_CONTAINS("\"FID\":\"uart0.config\"", ws_transport_stub_last_sent(conn));
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
    client_request(api, conn, ESP_WS_API_UART0_CNF, 2, "{\"BR\":4800}");
    CHECK(ws_transport_stub_send_attempts(conn) >= before + 2);   /* DELIVERED + ответ */
    STRCMP_CONTAINS("\"FID\":\"uart0.config\"", ws_transport_stub_last_sent(conn));
    STRCMP_CONTAINS("\"BR\":4800", ws_transport_stub_last_sent(conn));
}

/* Запись ТЕХ ЖЕ значений настроек ничего не меняет — рассылки нет: клиент
 * получает только ответ на свой вызов (DELIVERED и ответ с его SID). */
TEST(ApihUartCnf, SameValuesDoNotBroadcast) {
    client_request(api, conn, ESP_WS_API_UART0_CNF, 1, "{\"BR\":4800}");
    const int before = ws_transport_stub_send_attempts(conn);
    client_request(api, conn, ESP_WS_API_UART0_CNF, 2, "{\"BR\":4800}");
    LONGS_EQUAL(before + 2, ws_transport_stub_send_attempts(conn));
    STRCMP_CONTAINS("\"SID\":\"0x00000002\"", ws_transport_stub_last_sent(conn));
}

TEST(ApihUartCnf, WordLengthSetOnPort1) {
    client_request(api, conn, ESP_WS_API_UART0_CNF, 2, "{\"WL\":0}");   /* 7 бит */
    LONGS_EQUAL(GW_UART_WORD_7BIT, gw_uart_stub_last_config(&app.uart.port[0].desc).bits);
}

/* Однопроводный режим (MODE 2): приём глушится программно, потому что драйверу
 * тут разворачивать нечего. Значение не влезало в однобитное поле команды. */
TEST(ApihUartCnf, SingleWireModeAccepted) {
    client_request(api, conn, ESP_WS_API_UART2_CNF, 2, "{\"MODE\":2}");
    LONGS_EQUAL(GW_UART_MODE_UART_SINGLE_WIRE,
                gw_uart_stub_last_config(&app.uart.port[2].desc).mode);
}

TEST(ApihUartCnf, UnknownModeNotApplied) {
    client_request(api, conn, ESP_WS_API_UART2_CNF, 1, "{\"MODE\":2}");
    client_request(api, conn, ESP_WS_API_UART2_CNF, 2, "{\"MODE\":3}");
    /* Режим 3 не существует: на порту остаётся текущий, а не «ноль». */
    LONGS_EQUAL(GW_UART_MODE_UART_SINGLE_WIRE,
                gw_uart_stub_last_config(&app.uart.port[2].desc).mode);
}

/* Порог простоя линии: по нему драйвер отмечает конец кадра, а страница делит
 * поток данных порта на кадры (протоколы на стороне веб). */
TEST(ApihUartCnf, IdleCharsSetAndReported) {
    client_request(api, conn, ESP_WS_API_UART2_CNF, 2, "{\"IDLE_CH\":8}");
    LONGS_EQUAL(8, gw_uart_stub_last_config(&app.uart.port[2].desc).idle_ch);
    STRCMP_CONTAINS("\"IDLE_CH\":8", ws_transport_stub_last_sent(conn));
}

TEST(ApihUartCnf, IdleCharsOutOfRangeNotApplied) {
    client_request(api, conn, ESP_WS_API_UART2_CNF, 2, "{\"IDLE_CH\":0}");
    /* Ноль знаков — не порог: на порту остаётся текущее (в стабе — ноль) */
    LONGS_EQUAL(0, gw_uart_stub_last_config(&app.uart.port[2].desc).idle_ch);
}

/* ── TX tests ─────────────────────────────────────────────────────────── */

TEST(ApihUartTx, ValidBase64WritesToPort1) {
    /* base64("Hello") = "SGVsbG8=" */
    client_request(api, conn, ESP_WS_API_UART0_RAW_TX, 2, "\"SGVsbG8=\"");
    LONGS_EQUAL(1, gw_uart_stub_write_calls(&app.uart.port[0].desc));
    LONGS_EQUAL(5, gw_uart_stub_last_write_len(&app.uart.port[0].desc));
    MEMCMP_EQUAL("Hello", gw_uart_stub_last_write_buf(&app.uart.port[0].desc), 5);
}

TEST(ApihUartTx, ValidBase64WritesToPort2) {
    client_request(api, conn, ESP_WS_API_UART2_RAW_TX, 2, "\"SGVsbG8=\"");
    LONGS_EQUAL(1, gw_uart_stub_write_calls(&app.uart.port[2].desc));
}

TEST(ApihUartTx, NullDataDoesNotWrite) {
    /* Без ARG передавать нечего: обработчик отвечает FALSE сразу и молча — в
     * порт ничего не уходит и вызов не остаётся висеть. */
    client_request(api, conn, ESP_WS_API_UART0_RAW_TX, 2);
    LONGS_EQUAL(0, gw_uart_stub_write_calls(&app.uart.port[0].desc));
    worker_tick(api, 20);
    LONGS_EQUAL(0, gw_uart_stub_write_calls(&app.uart.port[0].desc));
}

TEST(ApihUartTx, ResponseSentAfterWrite) {
    client_request(api, conn, ESP_WS_API_UART0_RAW_TX, 2, "\"SGVsbG8=\"");
    LONGS_EQUAL(1, gw_uart_stub_write_calls(&app.uart.port[0].desc));
    STRCMP_CONTAINS("\"written\":5", ws_transport_stub_last_sent(conn));
}

/* ── RX tests ─────────────────────────────────────────────────────────── */

/* Пустой опрос: данных нет — кадра нет. Подписка при этом жива: следующий такт
 * снова спрашивает накопитель. */
TEST(ApihUartRx, EmptyRingSendsNoFrame) {
    client_subscription(api, conn, ESP_WS_API_UART0_RAW_RX, 1);
    const int before = ws_transport_stub_send_attempts(conn);

    worker_tick(api, 20);
    LONGS_EQUAL(before, ws_transport_stub_send_attempts(conn));
}

TEST(ApihUartRx, FilledBufferSendsBase64Response) {
    client_subscription(api, conn, ESP_WS_API_UART0_RAW_RX, 1);

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

    apih_uart_test_fill_rx_idle(2, (const uint8_t *)"AB", 2, 0, 1);
    apih_uart_test_fill_rx_idle(2, (const uint8_t *)"AB", 2, 0, 1);
    worker_tick(api, 100);

    /* base64("AB") = "QUI=", а '#' — конец передачи на линии. */
    STRCMP_CONTAINS("\"RX\":\"QUI=#QUI=#\"", ws_transport_stub_last_sent(conn));
}

/* Незакрытый сегмент отдаётся и без разделителя — значит кадр продолжается;
 * закрывающий '#' приходит с последними байтами. */
TEST(ApihUartRx, ContinuationGoesWithoutDelimiter) {
    client_subscription(api, conn, ESP_WS_API_UART2_RAW_RX, 1);
    static uint8_t long_data[1200];
    memset(long_data, 'x', sizeof(long_data));

    apih_uart_test_fill_rx_idle(2, long_data, sizeof(long_data), 0, 0);
    apih_uart_test_fill_rx_idle(2, (const uint8_t *)"EF", 2, 0, 1);
    worker_tick(api, 100);

    const char *sent = ws_transport_stub_last_sent(conn);
    /* Сегмент один и он закрылся на последних байтах: разделитель в сообщении
     * РОВНО один, в самом конце. */
    LONGS_EQUAL(1, count_char(sent, '#'));
    STRCMP_CONTAINS("#\"}", sent);
    CHECK(strlen(sent) > 1000);
}

/* Кадр забивается данными ДО ПРЕДЕЛА: сервис вычитывает кольцо столько раз,
 * сколько влезает в одно сообщение, и только потом отправляет. Пятьдесят
 * маленьких передач подряд обязаны уйти ОДНИМ кадром (иначе на каждой паузе в
 * линии получался бы отдельный кадр) — и с пятьюдесятью разделителями '#' в нём.
 * '#' — конец фрейма, '~' — его разрыв. */
TEST(ApihUartRx, FrameIsFilledUntilTheRingIsEmpty) {
    client_subscription(api, conn, ESP_WS_API_UART2_RAW_RX, 1);
    const int before = ws_transport_stub_send_attempts(conn);

    for (int i = 0; i < 50; i++)
        apih_uart_test_fill_rx_idle(2, (const uint8_t *)"AB", 2, 0, 1);
    worker_tick(api, 100);

    /* Одно сообщение на все пятьдесят передач. */
    LONGS_EQUAL(before + 1, ws_transport_stub_send_attempts(conn));

    const char *sent = ws_transport_stub_last_sent(conn);
    LONGS_EQUAL(50, count_char(sent, '#'));
    LONGS_EQUAL(50, count_substr(sent, "QUI=#"));       /* base64("AB") = "QUI=" */
}

/* Транспорт занят (его рабочая очередь полна) — кадр собран, но не отправлен.
 * Данные из кольца уже забраны, поэтому кадр обязан остаться и уйти следующим
 * тактом, а не пропасть: иначе «данные дошли до прибора» и «данные дошли до
 * клиента» расходятся молча (на приборе так терялось ~2 % потока). */
TEST(ApihUartRx, BusyTransportKeepsTheFrameForTheNextTick) {
    client_subscription(api, conn, ESP_WS_API_UART2_RAW_RX, 1);

    apih_uart_test_fill_rx(2, (const uint8_t *)"AB", 2);
    ws_transport_stub_set_send_result(conn, TRANSPORT_BUSY);

    /* Первый такт: кадр не принят транспортом — данных в отправленном нет. */
    worker_tick(api, 100);
    CHECK(strstr(ws_transport_stub_last_sent(conn), "\"RX\"") == NULL);

    /* Транспорт освободился — тот же кадр уходит, данные целы. */
    ws_transport_stub_set_send_result(conn, TRANSPORT_OK);
    worker_tick(api, 100);
    STRCMP_CONTAINS("\"RX\":\"QUI=#\"", ws_transport_stub_last_sent(conn));
}

/* Побайтовая сверка кадра: сервис обязан уложить в base64 РОВНО то, что отдало
 * кольцо, без дыр и дублей. На приборе расхождение сборки выглядело как «клиент
 * получил меньше байт, чем прибор принял», причём оба клиента — одинаково. */
/* Разбирает строку данных ОДНОГО кадра: куски base64 и разделители. Части
 * самостоятельны, поэтому склеиваются без оговорок; '#'/'~' — не данные.
 * Буфер ядра приходит с длиной и без '\0', поэтому сначала копия в строку. */
static size_t take_parts(const char *sent, uint8_t *out, size_t cap);

static size_t take_parts_len(const void *data, uint32_t len, uint8_t *out, size_t cap) {
    static char line[8192];
    if (len >= sizeof(line)) len = (uint32_t)sizeof(line) - 1;
    memcpy(line, data, len);
    line[len] = '\0';
    return take_parts(line, out, cap);
}

static size_t take_parts(const char *sent, uint8_t *out, size_t cap) {
    const char *rx = strstr(sent, "\"RX\":\"");
    if (rx == NULL) return 0;
    rx += 6;
    size_t got = 0;
    const char *q = rx;
    while (*q && *q != '"') {
        if (*q == '#' || *q == '~') { q++; continue; }
        const char *e = q;
        while (*e && *e != '"' && *e != '#' && *e != '~') e++;
        if (got < cap)
            got += (size_t)base64_decode(&out[got], (uint32_t)(cap - got),
                                         (const uint8_t *)q, (uint32_t)(e - q));
        q = e;
    }
    return got;
}

TEST(ApihUartRx, FrameContentMatchesTheRing) {
    /* Отложенные завершения: за один такт сервис отдаёт столько кадров, сколько
     * нужно (кадр ограничен накопителем), и в стабе они копятся — так видно ВСЁ,
     * что ушло клиенту, а не только последний кадр. */
    ws_transport_stub_set_deferred(conn, 1);
    client_subscription(api, conn, ESP_WS_API_UART2_RAW_RX, 1);

    /* Влезает в кольцо порта (2048): ровно как непрерывный поток линии, только
     * короче кольца — иначе часть байт кольцо не удержит и получит метку потери
     * (это отдельное свойство кольца, см. SegRingTest). */
    static uint8_t input[1500];
    for (size_t i = 0; i < sizeof(input); i++)
        input[i] = (uint8_t)((i * 7 + 3) & 0xFF);

    /* Один незакрытый сегмент — непрерывный поток линии. */
    apih_uart_test_fill_rx_idle(2, input, sizeof(input), 0, 0);
    worker_tick(api, 100);
    /* Ещё такт: кадр ограничен накопителем, остаток уходит следующим. */
    worker_tick(api, 100);
    worker_tick(api, 100);

    static uint8_t decoded[8192];
    size_t got = 0;
    const char *sent = ws_transport_stub_last_sent(conn);
    (void)sent;
    int frames = ws_transport_stub_pending_count(conn);
    CHECK(frames >= 1);
    for (int i = 0; i < frames; i++) {
        const void *data = ws_transport_stub_pending_data(conn, i);
        CHECK(data != NULL);
        /* Данные кадра — тот же конверт, что уходит клиенту, но без '\0'. */
        got += take_parts_len(data, ws_transport_stub_pending_len(conn, i),
                              &decoded[got],
                              (got < sizeof(decoded)) ? sizeof(decoded) - got : 0);
    }
    ws_transport_stub_set_deferred(conn, 0);

    LONGS_EQUAL(sizeof(input), got);
    MEMCMP_EQUAL(input, decoded, sizeof(input));
}

/* Каждая часть в строке данных (то, что между '#' и '~') обязана быть
 * САМОСТОЯТЕЛЬНОЙ base64-строкой: клиент декодирует части по отдельности, и '='
 * в середине части делает её неразбираемой. Ровно на этом прибор терял ~5 %
 * потока: данные кодировались кусками по 120 байт, у куска не кратной трём длины
 * появлялось '=', и всё после него пропадало. */
TEST(ApihUartRx, EveryPartIsSelfContainedBase64) {
    client_subscription(api, conn, ESP_WS_API_UART2_RAW_RX, 1);

    /* Длины нарочно не кратны трём — именно они дают выравнивание. */
    apih_uart_test_fill_rx_idle(2, (const uint8_t *)"abcde", 5, 0, 1);
    apih_uart_test_fill_rx_idle(2, (const uint8_t *)"1234567", 7, 0, 1);
    worker_tick(api, 100);

    const char *sent = ws_transport_stub_last_sent(conn);
    const char *rx = strstr(sent, "\"RX\":\"");
    CHECK(rx != NULL);
    rx += 6;

    /* Части: "abcde#" и "1234567#". */
    static uint8_t decoded[64];
    const char *parts[4];
    size_t part_len[4];
    int n_parts = 0;
    const char *q = rx;
    while (*q && *q != '"') {
        const char *e = q;
        while (*e && *e != '"' && *e != '#' && *e != '~') e++;
        if (e > q) {
            parts[n_parts] = q;
            part_len[n_parts] = (size_t)(e - q);
            n_parts++;
        }
        if (*e == '#' || *e == '~') e++;
        q = e;
    }
    LONGS_EQUAL(2, n_parts);

    for (int i = 0; i < n_parts; i++) {
        LONGS_EQUAL(0, part_len[i] % 4);          /* без '=' внутри части */
        int32_t got = base64_decode(decoded, sizeof(decoded),
                                    (const uint8_t *)parts[i], (uint32_t)part_len[i]);
        LONGS_EQUAL(i == 0 ? 5 : 7, got);
        MEMCMP_EQUAL(i == 0 ? "abcde" : "1234567", decoded, (size_t)got);
    }
}

/* Транспорт не принимает кадр долго (клиент ушёл, связь встала). Держать его
 * вечно нельзя: пока кадр в буфере, порт не отдаёт ничего — терминал «просто
 * перестаёт работать» до перезагрузки. Через UART_RX_PENDING_HOLD_MS кадр
 * отпускается, и порт снова живой. */
TEST(ApihUartRx, StuckPendingFrameDoesNotBlockThePort) {
    client_subscription(api, conn, ESP_WS_API_UART2_RAW_RX, 1);
    ws_transport_stub_set_send_result(conn, TRANSPORT_BUSY);

    apih_uart_test_fill_rx(2, (const uint8_t *)"AB", 2);
    worker_tick(api, 100);                     /* кадр отложен */

    /* Транспорт «молчит» дольше предела — кадр отбрасывается. */
    for (int i = 0; i < 40; i++)
        worker_tick(api, 100);

    /* Транспорт ожил: новые данные обязаны уйти, порт не заблокирован. */
    ws_transport_stub_set_send_result(conn, TRANSPORT_OK);
    apih_uart_test_fill_rx(2, (const uint8_t *)"CD", 2);
    worker_tick(api, 100);
    STRCMP_CONTAINS("\"RX\":\"Q0Q=#\"", ws_transport_stub_last_sent(conn));
}

TEST(ApihUartRx, BufferClearedAfterRead) {
    client_subscription(api, conn, ESP_WS_API_UART2_RAW_RX, 1);

    const uint8_t payload[] = "Hi";
    apih_uart_test_fill_rx(2, payload, 2);
    worker_tick(api, 100);
    const int after_first = ws_transport_stub_send_attempts(conn);

    /* Второй опрос того же порта: кольцо пусто — данных нет, кадра нет. */
    worker_tick(api, 100);
    LONGS_EQUAL(after_first, ws_transport_stub_send_attempts(conn));
}

/* ── Темп кадров приёма ───────────────────────────────────────────────────
 *
 * Данные уходят кадром не чаще, чем раз в UART_RX_FRAME_MS (75 мс): при
 * скорости порта 115200 бод (11520 Б/с) за этот срок накапливается 864 байта —
 * меньше накопителя (2048), то есть копить можно без потерь, а кадров в разы
 * меньше (иначе эхо набора в терминале давало кадр на символ).
 *
 * Темп — не ожидание: когда срок не вышел, обработчик возвращает TRUE, и ядро
 * позовёт его снова на следующем такте. Ни одной задержки в системе при этом не
 * запрашивается (system_stub_last_delay это и проверяет). */

TEST(ApihUartRx, SecondFrameWaitsForTheRateGate) {
    client_subscription(api, conn, ESP_WS_API_UART2_RAW_RX, 1);
    const int before = ws_transport_stub_send_attempts(conn);

    /* Первый кадр уходит сразу — ждать нечего */
    const uint8_t first[] = "one";
    apih_uart_test_fill_rx(2, first, 3);
    worker_tick(api, 100);
    LONGS_EQUAL(before + 1, ws_transport_stub_send_attempts(conn));

    /* Кадр сразу за ним не уходит: темп не вышел. Обработчик вернул TRUE —
     * вызов жив и ждёт следующего такта, а не спит. */
    const uint8_t second[] = "two";
    apih_uart_test_fill_rx(2, second, 3);
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
    client_subscription(api, conn, ESP_WS_API_UART0_RAW_RX, 1);
    client_subscription(api, conn, ESP_WS_API_UART2_RAW_RX, 2);
    const int before = ws_transport_stub_send_attempts(conn);

    const uint8_t data[] = "x";
    apih_uart_test_fill_rx(0, data, 1);
    worker_tick(api, 100);
    LONGS_EQUAL(before + 1, ws_transport_stub_send_attempts(conn));

    apih_uart_test_fill_rx(2, data, 1);
    worker_tick(api, 10);
    LONGS_EQUAL(before + 2, ws_transport_stub_send_attempts(conn));
    LONGS_EQUAL(0, system_stub_last_delay());
}

/* ── Echo tests ───────────────────────────────────────────────────────── */

TEST(ApihUartEcho, SetEchoOnPort1) {
    client_request(api, conn, ESP_WS_API_UART0_ECHO, 2, "{\"E\":1}");
    LONGS_EQUAL(1, gw_uart_get_echo(&app.uart.port[0].desc));
}

TEST(ApihUartEcho, ClearEchoOnPort2) {
    gw_uart_set_echo(&app.uart.port[2].desc, 1);
    client_request(api, conn, ESP_WS_API_UART2_ECHO, 2, "{\"E\":0}");
    LONGS_EQUAL(0, gw_uart_get_echo(&app.uart.port[2].desc));
}

TEST(ApihUartEcho, QuerySendsResponse) {
    client_request(api, conn, ESP_WS_API_UART0_ECHO, 2);
    STRCMP_CONTAINS("\"E\":0", ws_transport_stub_last_sent(conn));
}

TEST(ApihUartEcho, UnknownFidNoResponse) {
    /* Порта "uart3" нет: такого FID на ядре не зарегистрировано — клиент получает
     * NO_HANDLER, а не тишину. */
    const int before = ws_transport_stub_send_attempts(conn);
    client_frame(api, conn, "uart3.rx", 0, 1);
    LONGS_EQUAL(before + 1, ws_transport_stub_send_attempts(conn));
}
