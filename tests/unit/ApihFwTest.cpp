extern "C" {
#include "app.h"
#include "api.h"
#include "platform.h"
#include "apih_sys_internal.h"
#include "esp_image_format.h"
#include "esp_ota_ops.h"
#include "esp_rom_md5.h"
}
#include "system_stub.h"
#include "transport_stub.h"
#include "net_stub.h"
#include "esp_ota_stub.h"
#include "CppUTest/TestHarness.h"

#include <stdio.h>
#include <string.h>

extern "C" {
    /* Состояние службы прошивки — статическое (на приборе оно одно на всё время
     * жизни), поэтому тест сбрасывает его между случаями: apih_fw.c под
     * APIH_FW_TEST отдаёт эту точку входа. */
    void apih_fw_test_reset(void);
}

/* ── Клиентская сторона ───────────────────────────────────────────────────
 *
 * Служба прошивки обслуживается ТАКТОМ ЯДРА: её FID зарегистрированы
 * обработчиками (apih_fw_run — то же, что зовёт api_sys_run в прошивке), а
 * клиент — это кадр из транспорта плюс вызов api_worker. Поэтому тест идёт ровно
 * тем путём, что и настоящий клиент: кадр → ядро → обработчик службы → ответ в
 * транспорт (ws_transport_stub_last_sent). Вместо flash и OTA-раздела стоит
 * esp_ota_stub: он запоминает образ в памяти и умеет отказывать по требованию.
 *
 * Образца «как это выглядит на клиенте» здесь два: сначала sys.fw.begin с
 * размером, потом порции sys.fw.write (SEQ строго по порядку, DATA в base64),
 * и в конце sys.fw.end. Проверяется разбор ARG и порядок порций — то, что на
 * приборе видно только по журналу и по перезагрузке. */

/* Кадр клиента: {"FID":…,"FLAGS":…,"SID":…[,"ARG":…]} + один такт ядра. */
static void client_frame(api_t *api, ws_transport_stub_conn_t *conn,
                         const char *fid, int flags, int sid, const char *arg = NULL) {
    char frame[256];
    if (arg)
        snprintf(frame, sizeof(frame),
                 "{\"FID\":\"%s\",\"FLAGS\":%d,\"SID\":%d,\"ARG\":%s}", fid, flags, sid, arg);
    else
        snprintf(frame, sizeof(frame), "{\"FID\":\"%s\",\"FLAGS\":%d,\"SID\":%d}", fid, flags, sid);
    ws_transport_stub_frame(conn, WS_STUB_FRAME_TEXT, frame, strlen(frame), 1);
    api_worker(api);
}

static void client_request(api_t *api, ws_transport_stub_conn_t *conn,
                           const char *fid, int sid, const char *arg = NULL) {
    client_frame(api, conn, fid, 0, sid, arg);
}

/* Такт ядра без нового кадра. Вызов sys.fw.begin/write остаётся открытым
 * (обработчик вернул TRUE) и получает такой такт с data == NULL — так служба и
 * замечает, что куска нет слишком долго. */
static void worker_tick(api_t *api, uint32_t advance_ms = 0) {
    if (advance_ms)
        system_stub_advance_time(advance_ms);
    api_worker(api);
}

/* Ответ на последний запрос: служба отвечает прямо в вызов, ядро оборачивает
 * ответ в конверт. */
static const char *reply(ws_transport_stub_conn_t *conn) {
    return ws_transport_stub_last_sent(conn);
}

/* Образ из 24 байт (минимум для esp_image_header_t, он же минимум SIZE):
 * "abc" и нули. Порция в base64 и её MD5 — постоянные величины, чтобы тест
 * сверки не считал ожидание той же функцией, что и служба. */
#define FW_IMAGE_B64 "YWJjAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
#define FW_IMAGE_MD5 "bd337e48eebd58b922d172295310a2fc"
#define FW_IMAGE_SIZE 24

/* Открыть заливку образа из FW_IMAGE_SIZE байт. */
static void begin_image(api_t *api, ws_transport_stub_conn_t *conn, int sid, int size = FW_IMAGE_SIZE) {
    char arg[64];
    snprintf(arg, sizeof(arg), "{\"SIZE\":%d}", size);
    client_request(api, conn, ESP_WS_API_SYS_FW_BEGIN, sid, arg);
}

/* Отправить порцию образа-образца (SEQ — номер порции, первая — 1). */
static void write_image(api_t *api, ws_transport_stub_conn_t *conn, int sid, int seq = 1) {
    char arg[128];
    snprintf(arg, sizeof(arg), "{\"SEQ\":%d,\"DATA\":\"" FW_IMAGE_B64 "\"}", seq);
    client_request(api, conn, ESP_WS_API_SYS_FW_WRITE, sid, arg);
}

/* ── Test fixture ─────────────────────────────────────────────────────── */

#define FW_SETUP()                                     \
    do {                                               \
        memset(&app, 0, sizeof(app));                  \
        system_stub_reset();                           \
        ws_transport_stub_reset();                     \
        esp_ota_stub_reset();                          \
        net_stub_reset();                              \
        api = ws_transport_stub_init();                \
        conn = ws_transport_stub_connect(1);           \
        app.api_ap = api;                              \
        app.api_sta = NULL;                            \
        apih_sys_common_init(&app);                    \
        apih_fw_test_reset();                          \
        apih_fw_run(&app);                             \
    } while (0)

#define FW_TEARDOWN()                                  \
    do {                                               \
        apih_fw_test_reset();                          \
        ws_transport_stub_reset();                     \
        api_deinit(api);                               \
    } while (0)

TEST_GROUP(ApihFwBegin) {
    app_context_t app;
    /* Своё ядро API у каждого теста: служба берёт его из app.api_ap, как в
     * прошивке. Второго ядра (станции) у службы прошивки нет намеренно —
     * прошивка принимается только с точки доступа. */
    api_t *api;
    ws_transport_stub_conn_t *conn;
    void setup() { FW_SETUP(); }
    void teardown() { FW_TEARDOWN(); }
};

TEST_GROUP(ApihFwWrite) {
    app_context_t app;
    api_t *api;
    ws_transport_stub_conn_t *conn;
    void setup() { FW_SETUP(); }
    void teardown() { FW_TEARDOWN(); }
};

TEST_GROUP(ApihFwEnd) {
    app_context_t app;
    api_t *api;
    ws_transport_stub_conn_t *conn;
    void setup() { FW_SETUP(); }
    void teardown() { FW_TEARDOWN(); }
};

TEST_GROUP(ApihFwAbort) {
    app_context_t app;
    api_t *api;
    ws_transport_stub_conn_t *conn;
    void setup() { FW_SETUP(); }
    void teardown() { FW_TEARDOWN(); }
};

TEST_GROUP(ApihFwGuard) {
    app_context_t app;
    api_t *api;
    ws_transport_stub_conn_t *conn;
    void setup() { FW_SETUP(); }
    void teardown() { FW_TEARDOWN(); }
};

/* Два ядра, как в прошивке: точка доступа и станция. Служба прошивки
 * регистрируется только на первом — это и есть «только AP», без масок путей. */
TEST_GROUP(ApihFwApOnly) {
    app_context_t app;
    api_t *api_ap;
    api_t *api_sta;
    ws_transport_stub_conn_t *conn_ap;
    ws_transport_stub_conn_t *conn_sta;
    void setup() {
        memset(&app, 0, sizeof(app));
        system_stub_reset();
        ws_transport_stub_reset();
        esp_ota_stub_reset();
        net_stub_reset();
        api_ap = ws_transport_stub_init();                    /* ядро точки доступа */
        api_sta = ws_transport_stub_init_only(GW_IFACE_STA);   /* ядро станции */
        conn_ap = ws_transport_stub_connect(1);                /* интерфейс по умолчанию — AP */
        ws_transport_stub_set_iface(GW_IFACE_STA);
        conn_sta = ws_transport_stub_connect(2);
        app.api_ap = api_ap;
        app.api_sta = api_sta;
        apih_sys_common_init(&app);
        apih_fw_test_reset();
        apih_fw_run(&app);
    }
    void teardown() {
        apih_fw_test_reset();
        ws_transport_stub_reset();
        api_deinit(api_ap);
        api_deinit(api_sta);
    }
};

/* ── Начало заливки ───────────────────────────────────────────────────── */

TEST(ApihFwBegin, OpensSessionInTheNextOtaPartition) {
    begin_image(api, conn, 1);

    LONGS_EQUAL(1, esp_ota_stub_begin_calls());
    LONGS_EQUAL(FW_IMAGE_SIZE, (int)esp_ota_stub_begin_size());
    LONGS_EQUAL(1, esp_ota_stub_active());
    STRCMP_CONTAINS("\"FID\":\"sys.fw.begin\"", reply(conn));
    STRCMP_CONTAINS("\"SESSION\":1", reply(conn));
    STRCMP_CONTAINS("\"STA\":\"0x00000000\"", reply(conn));
}

TEST(ApihFwBegin, SizeIsRequired) {
    client_request(api, conn, ESP_WS_API_SYS_FW_BEGIN, 1, "{\"WRONG\":1}");

    LONGS_EQUAL(0, esp_ota_stub_begin_calls());
    STRCMP_CONTAINS("\"ERR\":\"FW_SIZE\"", reply(conn));
    STRCMP_CONTAINS("\"STA\":\"0x80000003\"", reply(conn));
}

TEST(ApihFwBegin, BrokenArgIsRefused) {
    client_request(api, conn, ESP_WS_API_SYS_FW_BEGIN, 1, "not json");

    LONGS_EQUAL(0, esp_ota_stub_begin_calls());
    STRCMP_CONTAINS("\"ERR\":\"ARG\"", reply(conn));
}

TEST(ApihFwBegin, ImageSmallerThanTheHeaderIsRefused) {
    begin_image(api, conn, 1, (int)sizeof(esp_image_header_t) - 1);

    LONGS_EQUAL(0, esp_ota_stub_begin_calls());
    STRCMP_CONTAINS("\"ERR\":\"FW_SIZE\"", reply(conn));
    /* Отказ объясняет предел: странице есть что показать человеку. */
    STRCMP_CONTAINS("Firmware must be between", reply(conn));
}

TEST(ApihFwBegin, ImageLargerThanThePartitionIsRefused) {
    begin_image(api, conn, 1, (int)ESP_OTA_STUB_PART_SIZE + 1);

    LONGS_EQUAL(0, esp_ota_stub_begin_calls());
    STRCMP_CONTAINS("\"ERR\":\"FW_SIZE\"", reply(conn));
}

TEST(ApihFwBegin, SecondBeginIsRefusedWhileTheFirstIsOpen) {
    begin_image(api, conn, 1);
    begin_image(api, conn, 2);

    /* Второй begin не стирает раздел и не рвёт уже открытую заливку: клиент
     * получает BUSY и счётчик принятого, чтобы закончить начатое. */
    LONGS_EQUAL(1, esp_ota_stub_begin_calls());
    STRCMP_CONTAINS("\"ERR\":\"SESSION\"", reply(conn));
    STRCMP_CONTAINS("\"STA\":\"0x80000008\"", reply(conn));
}

TEST(ApihFwBegin, RefusedWhileTwoClientsAreOnTheAp) {
    net_stub_set_clients(2);
    begin_image(api, conn, 1);

    LONGS_EQUAL(0, esp_ota_stub_begin_calls());
    STRCMP_CONTAINS("\"ERR\":\"AP_CLIENTS\"", reply(conn));
    STRCMP_CONTAINS("\"STA\":\"0x80000007\"", reply(conn));
    STRCMP_CONTAINS("\"CLIENTS\":2", reply(conn));
}

TEST(ApihFwBegin, RefusedWhenThereIsNoOtaPartition) {
    esp_ota_stub_set_partition_size(0);
    begin_image(api, conn, 1);

    LONGS_EQUAL(0, esp_ota_stub_begin_calls());
    STRCMP_CONTAINS("\"ERR\":\"PART\"", reply(conn));
}

TEST(ApihFwBegin, OtaFailureIsReportedAsInternal) {
    esp_ota_stub_set_begin_error(ESP_ERR_OTA_SELECT_INFO_INVALID);
    begin_image(api, conn, 1);

    LONGS_EQUAL(1, esp_ota_stub_begin_calls());
    STRCMP_CONTAINS("\"ERR\":\"OTA\"", reply(conn));
    STRCMP_CONTAINS("\"STA\":\"0x8000000f\"", reply(conn));
}

/* ── Порции ───────────────────────────────────────────────────────────── */

TEST(ApihFwWrite, ChunkWithoutBeginIsRefused) {
    write_image(api, conn, 1);

    LONGS_EQUAL(0, esp_ota_stub_write_calls());
    STRCMP_CONTAINS("\"ERR\":\"SESSION\"", reply(conn));
    STRCMP_CONTAINS("\"STA\":\"0x80000003\"", reply(conn));
}

TEST(ApihFwWrite, ChunkGoesIntoTheImageAndIsCounted) {
    begin_image(api, conn, 1);
    write_image(api, conn, 2);

    LONGS_EQUAL(1, esp_ota_stub_write_calls());
    LONGS_EQUAL(FW_IMAGE_SIZE, (int)esp_ota_stub_bytes());
    MEMCMP_EQUAL("abc", esp_ota_stub_data(), 3);
    STRCMP_CONTAINS("\"FID\":\"sys.fw.write\"", reply(conn));
    STRCMP_CONTAINS("\"WRITTEN\":24", reply(conn));
}

TEST(ApihFwWrite, OutOfOrderChunkIsRefusedAndNothingIsWritten) {
    begin_image(api, conn, 1);
    client_request(api, conn, ESP_WS_API_SYS_FW_WRITE, 2,
                   "{\"SEQ\":2,\"DATA\":\"" FW_IMAGE_B64 "\"}");

    /* Порция не та, которую ждём: записать её значило бы сдвинуть образ. */
    LONGS_EQUAL(0, esp_ota_stub_write_calls());
    STRCMP_CONTAINS("\"ERR\":\"SEQ\"", reply(conn));
    STRCMP_CONTAINS("\"EXPECT\":1", reply(conn));
}

TEST(ApihFwWrite, BrokenBase64IsRefused) {
    begin_image(api, conn, 1);
    client_request(api, conn, ESP_WS_API_SYS_FW_WRITE, 2, "{\"SEQ\":1,\"DATA\":\"SGVsbG8\"}");

    LONGS_EQUAL(0, esp_ota_stub_write_calls());
    STRCMP_CONTAINS("\"ERR\":\"DATA\"", reply(conn));
    LONGS_EQUAL(1, esp_ota_stub_active());          /* заливка жива: порцию можно повторить */
}

TEST(ApihFwWrite, MoreDataThanAnnouncedAbortsTheUpload) {
    begin_image(api, conn, 1, 8);
    /* 8 — меньше заголовка образа, поэтому begin откажет; берём допустимый
     * размер и шлём больше, чем объявили. */
    begin_image(api, conn, 2);
    write_image(api, conn, 3);
    client_request(api, conn, ESP_WS_API_SYS_FW_WRITE, 4,
                   "{\"SEQ\":2,\"DATA\":\"" FW_IMAGE_B64 "\"}");

    STRCMP_CONTAINS("\"ERR\":\"FW_SIZE\"", reply(conn));
    LONGS_EQUAL(1, esp_ota_stub_abort_calls());
    LONGS_EQUAL(0, esp_ota_stub_active());
}

TEST(ApihFwWrite, WrongImageAbortsTheUpload) {
    esp_ota_stub_set_write_error(ESP_ERR_OTA_VALIDATE_FAILED);
    begin_image(api, conn, 1);
    write_image(api, conn, 2);

    /* Ошибка клиента (не образ) — отказ 0x80000003 и разговор окончен: сессия
     * OTA освобождена, раздел не остался занятым. */
    STRCMP_CONTAINS("\"ERR\":\"IMAGE\"", reply(conn));
    STRCMP_CONTAINS("\"STA\":\"0x80000003\"", reply(conn));
    LONGS_EQUAL(1, esp_ota_stub_abort_calls());
    LONGS_EQUAL(0, esp_ota_stub_active());
}

TEST(ApihFwWrite, SecondChunkContinuesTheCount) {
    /* Образ из двух порций по 12 байт: begin 24, две порции по base64 от 12
     * байт ("abc" + 9 нулей и ещё 12 нулей). */
    begin_image(api, conn, 1);
    client_request(api, conn, ESP_WS_API_SYS_FW_WRITE, 2,
                   "{\"SEQ\":1,\"DATA\":\"YWJjAAAAAAAAAAAA\"}");
    client_request(api, conn, ESP_WS_API_SYS_FW_WRITE, 3,
                   "{\"SEQ\":2,\"DATA\":\"AAAAAAAAAAAAAAAA\"}");

    LONGS_EQUAL(2, esp_ota_stub_write_calls());
    LONGS_EQUAL(24, (int)esp_ota_stub_bytes());
    STRCMP_CONTAINS("\"WRITTEN\":24", reply(conn));
}

TEST(ApihFwWrite, EmptyChunkKeepsTheOrder) {
    begin_image(api, conn, 1);
    /* Пустая порция — тоже порция: номер двигается, принятых байт нет. */
    client_request(api, conn, ESP_WS_API_SYS_FW_WRITE, 2, "{\"SEQ\":1,\"DATA\":\"\"}");
    STRCMP_CONTAINS("\"WRITTEN\":0", reply(conn));
    write_image(api, conn, 3, 2);
    STRCMP_CONTAINS("\"WRITTEN\":24", reply(conn));
}

/* ── Завершение ───────────────────────────────────────────────────────── */

TEST(ApihFwEnd, EndWithoutBeginIsRefused) {
    client_request(api, conn, ESP_WS_API_SYS_FW_END, 1, "{}");

    LONGS_EQUAL(0, esp_ota_stub_end_calls());
    STRCMP_CONTAINS("\"ERR\":\"SESSION\"", reply(conn));
}

TEST(ApihFwEnd, MatchingMd5SelectsTheBootPartition) {
    begin_image(api, conn, 1);
    write_image(api, conn, 2);
    client_request(api, conn, ESP_WS_API_SYS_FW_END, 3,
                   "{\"MD5\":\"" FW_IMAGE_MD5 "\"}");

    LONGS_EQUAL(1, esp_ota_stub_end_calls());
    LONGS_EQUAL(1, esp_ota_stub_set_boot_calls());
    STRCMP_CONTAINS("\"OK\":1", reply(conn));
    STRCMP_CONTAINS("\"REBOOT\":1", reply(conn));
    /* Перезагрузку задача откладывает: ответ должен уйти раньше, чем
     * соединение оборвётся. */
    LONGS_EQUAL(1, system_stub_task_created_count());
}

TEST(ApihFwEnd, WithoutMd5TheImageIsOnlyCheckedByTheOtaLayer) {
    begin_image(api, conn, 1);
    write_image(api, conn, 2);
    client_request(api, conn, ESP_WS_API_SYS_FW_END, 3, "{}");

    LONGS_EQUAL(1, esp_ota_stub_end_calls());
    LONGS_EQUAL(1, esp_ota_stub_set_boot_calls());
    STRCMP_CONTAINS("\"REBOOT\":1", reply(conn));
}

TEST(ApihFwEnd, WrongMd5AbortsAndKeepsTheOldFirmware) {
    begin_image(api, conn, 1);
    write_image(api, conn, 2);
    client_request(api, conn, ESP_WS_API_SYS_FW_END, 3,
                   "{\"MD5\":\"00000000000000000000000000000000\"}");

    LONGS_EQUAL(0, esp_ota_stub_end_calls());
    LONGS_EQUAL(0, esp_ota_stub_set_boot_calls());
    LONGS_EQUAL(1, esp_ota_stub_abort_calls());
    STRCMP_CONTAINS("\"ERR\":\"MD5\"", reply(conn));
}

TEST(ApihFwEnd, Md5MustBeHexadecimal) {
    begin_image(api, conn, 1);
    write_image(api, conn, 2);
    client_request(api, conn, ESP_WS_API_SYS_FW_END, 3, "{\"MD5\":\"not-a-digest\"}");

    LONGS_EQUAL(0, esp_ota_stub_end_calls());
    STRCMP_CONTAINS("\"ERR\":\"MD5\"", reply(conn));
}

TEST(ApihFwEnd, ShortImageIsRefused) {
    begin_image(api, conn, 1, FW_IMAGE_SIZE + 12);
    write_image(api, conn, 2);
    client_request(api, conn, ESP_WS_API_SYS_FW_END, 3, "{}");

    /* Недостача порций видна только здесь: esp_ota_end смотрит на образ, а не
     * на то, сколько байт обещал клиент. */
    LONGS_EQUAL(0, esp_ota_stub_end_calls());
    LONGS_EQUAL(1, esp_ota_stub_abort_calls());
    STRCMP_CONTAINS("\"ERR\":\"FW_SIZE\"", reply(conn));
    STRCMP_CONTAINS("Received 24 of 36 bytes", reply(conn));
}

TEST(ApihFwEnd, NonImageIsReportedAsAClientMistake) {
    esp_ota_stub_set_end_error(ESP_ERR_OTA_VALIDATE_FAILED);
    begin_image(api, conn, 1);
    write_image(api, conn, 2);
    client_request(api, conn, ESP_WS_API_SYS_FW_END, 3, "{}");

    /* esp_ota_end при отказе освобождает сессию сам: abort после него нельзя, и
     * служба его не зовёт. */
    LONGS_EQUAL(0, esp_ota_stub_abort_calls());
    LONGS_EQUAL(0, esp_ota_stub_set_boot_calls());
    STRCMP_CONTAINS("\"ERR\":\"IMAGE\"", reply(conn));
}

TEST(ApihFwEnd, BootSelectionFailureIsReported) {
    esp_ota_stub_set_boot_error(ESP_ERR_INVALID_STATE);
    begin_image(api, conn, 1);
    write_image(api, conn, 2);
    client_request(api, conn, ESP_WS_API_SYS_FW_END, 3, "{}");

    LONGS_EQUAL(1, esp_ota_stub_end_calls());
    LONGS_EQUAL(1, esp_ota_stub_set_boot_calls());
    LONGS_EQUAL(0, system_stub_task_created_count());   /* перезагружаться некуда */
    STRCMP_CONTAINS("\"ERR\":\"OTA\"", reply(conn));
}

/* ── Отказ ────────────────────────────────────────────────────────────── */

TEST(ApihFwAbort, AbortDropsTheUpload) {
    begin_image(api, conn, 1);
    client_request(api, conn, ESP_WS_API_SYS_FW_ABORT, 2, "{}");

    LONGS_EQUAL(1, esp_ota_stub_abort_calls());
    LONGS_EQUAL(0, esp_ota_stub_active());
    STRCMP_CONTAINS("\"OK\":1", reply(conn));

    /* Сессии больше нет: следующая порция — отказ, а не запись в чужой раздел. */
    write_image(api, conn, 3);
    STRCMP_CONTAINS("\"ERR\":\"SESSION\"", reply(conn));
    LONGS_EQUAL(0, esp_ota_stub_write_calls());
}

TEST(ApihFwAbort, AbortWithoutSessionIsStillOk) {
    /* «Заливки нет» — тоже успех: страница не должна различать два этих случая,
     * когда отменяет передачу. */
    client_request(api, conn, ESP_WS_API_SYS_FW_ABORT, 1, "{}");

    LONGS_EQUAL(0, esp_ota_stub_abort_calls());
    STRCMP_CONTAINS("\"OK\":1", reply(conn));
}

/* ── Стража заливки ───────────────────────────────────────────────────── */

TEST(ApihFwGuard, MissingChunkAbortsTheUpload) {
    begin_image(api, conn, 1);
    LONGS_EQUAL(1, esp_ota_stub_active());

    /* Куска нет дольше срока: вызов sys.fw.begin остаётся открытым, и каждый
     * такт — это проверка. Сессия OTA освобождается, раздел больше не занят. */
    worker_tick(api, 1000);
    LONGS_EQUAL(1, esp_ota_stub_active());
    worker_tick(api, 20000);
    LONGS_EQUAL(1, esp_ota_stub_abort_calls());
    LONGS_EQUAL(0, esp_ota_stub_active());

    write_image(api, conn, 2);
    STRCMP_CONTAINS("\"ERR\":\"SESSION\"", reply(conn));
}

TEST(ApihFwGuard, ChunkWithinTheLimitKeepsTheUploadAlive) {
    begin_image(api, conn, 1);

    /* Порция приходит до срока: срок отсчитывается от неё, а не от begin. */
    worker_tick(api, 14000);
    write_image(api, conn, 2);
    worker_tick(api, 14000);

    LONGS_EQUAL(0, esp_ota_stub_abort_calls());
    LONGS_EQUAL(1, esp_ota_stub_active());
}

TEST(ApihFwGuard, ClientGoneAbortsTheUpload) {
    begin_image(api, conn, 1);
    LONGS_EQUAL(1, esp_ota_stub_active());

    /* Соединение оборвалось: ядро снимает вызов, обработчик получает
     * call == NULL — заливка не нужна никому и абортится. */
    ws_transport_stub_disconnect(conn);
    worker_tick(api, 10);
    worker_tick(api, 10);

    LONGS_EQUAL(1, esp_ota_stub_abort_calls());
    LONGS_EQUAL(0, esp_ota_stub_active());
}

/* ── Только точка доступа ─────────────────────────────────────────────── */

TEST(ApihFwApOnly, StationCoreHasNoFirmwareService) {
    /* С адреса станционной сети прошивку не принимают: FID на STA-ядре не
     * зарегистрирован, клиент получает NO_HANDLER, а раздел даже не трогается. */
    client_request(api_sta, conn_sta, ESP_WS_API_SYS_FW_BEGIN, 1, "{\"SIZE\":24}");

    LONGS_EQUAL(0, esp_ota_stub_begin_calls());
    STRCMP_CONTAINS("\"STA\":\"0x8000000e\"", reply(conn_sta));

    /* Тот же вызов с точки доступа обслуживается — значит служба не сломана, а
     * именно не оказана станции. */
    begin_image(api_ap, conn_ap, 1);
    LONGS_EQUAL(1, esp_ota_stub_begin_calls());
    STRCMP_CONTAINS("\"SESSION\":1", reply(conn_ap));
}

/* ── Заглушка MD5 ─────────────────────────────────────────────────────── */

/* Тесты сверки MD5 опираются на стаб esp_rom_md5 — значит он обязан считать
 * настоящий MD5. Вектор из RFC 1321: MD5("abc") = 900150983cd24fb0d6963f7d28e17f72. */
TEST(ApihFwBegin, Md5StubComputesTheKnownVector) {
    md5_context_t ctx;
    uint8_t digest[ESP_ROM_MD5_DIGEST_LEN];
    char text[2 * ESP_ROM_MD5_DIGEST_LEN + 1];

    esp_rom_md5_init(&ctx);
    esp_rom_md5_update(&ctx, "abc", 3);
    esp_rom_md5_final(digest, &ctx);

    for (int i = 0; i < ESP_ROM_MD5_DIGEST_LEN; i++)
        snprintf(text + i * 2, 3, "%02x", digest[i]);

    STRCMP_EQUAL("900150983cd24fb0d6963f7d28e17f72", text);
}
