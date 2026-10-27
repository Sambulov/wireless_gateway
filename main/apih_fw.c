/* Обновление прошивки по API: sys.fw.begin / sys.fw.write / sys.fw.end / sys.fw.abort.
 *
 * Заливка идёт ПОВЫЗОВНО, как у файлового менеджера (apih_files.c): страница присылает
 * порции base64, устройство отвечает числом принятых байт, а порядок порций держит
 * SEQ — потерянная порция иначе записала бы образ со сдвигом. Порциями, а не одним
 * вызовом: транспорт держит сообщение в 16 КБ, а образ — сотни килобайт. Номер порции
 * вместо OFFSET — потому что образ пишется строго последовательно, и «сколько принято»
 * есть сам номер порции.
 *
 * Доступ только с точки доступа: неверный образ запирает оператора снаружи прибора,
 * поэтому служба зарегистрирована лишь на AP-ядре.
 *
 * Состояние заливки статическое, одно на прибор. Пока заливка идёт, вызов begin/write
 * остаётся открытым (обработчик вернул TRUE) и получает такт каждый период — этим
 * служба и сторожит заливку: куска нет FW_STALL_MS или клиент ушёл (call == NULL) →
 * аборт и освобождение OTA-раздела. Без стражи оборванная передача оставляла бы
 * открытую OTA-сессию (в куче esp_ota) до перезагрузки.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_image_format.h"
#include "esp_ota_ops.h"
#include "esp_rom_md5.h"
#include "esp_system.h"

#include "platform.h"

#ifdef APIH_HOST_TEST
#  include "apih_sys_app_types.h"
#else
#  include "app.h"
#endif

#include "apih_sys_internal.h"

#define GW_LOG_MODULE GW_LOG_API
#include "gw_log.h"

/* Ответ должен уйти раньше, чем перезагрузка оборвёт соединение. */
#define FW_REBOOT_DELAY_MS 1500

/* Сколько служба ждёт следующую порцию, мс: страховка от оборванной передачи, а не
 * рабочий темп (страница шлёт порции подряд). */
#define FW_STALL_MS 15000

/* Токенов на разбор ARG: SIZE / SEQ / DATA / MD5 — с запасом */
#define FW_ARG_MAX_TOKENS 8

/* Номер первой порции: он же и есть «сколько порций уже принято» */
#define FW_SEQ_FIRST 1

typedef struct {
    uint8_t                active;    /* 1 — OTA-сессия открыта, приём идёт */
    esp_ota_handle_t       handle;
    const esp_partition_t *part;
    uint32_t               size;      /* заказанный размер образа (SIZE из begin) */
    uint32_t               written;
    uint32_t               next_seq;
    uint32_t               last_tick;
    md5_context_t          md5;       /* MD5 по сырым (не base64) данным */
} fw_upload_t;

static fw_upload_t fw_up;

/* Вызов, который сторожит заливку: последний sys.fw.begin или sys.fw.write. Он и
 * получает такты ядра, пока заливка идёт (см. fw_guard_tick); NULL — сторожить нечего */
static void *fw_guard;

static void fw_state_reset(void) {
    fw_up.active   = 0;
    fw_up.handle   = 0;
    fw_up.part     = NULL;
    fw_up.size     = 0;
    fw_up.written  = 0;
    fw_up.next_seq = FW_SEQ_FIRST;
    fw_up.last_tick = 0;
    memset(&fw_up.md5, 0, sizeof(fw_up.md5));
}

/* Прервать заливку и забыть её: esp_ota_abort освобождает всё, что взял
 * esp_ota_begin. Вызывается на отказе клиента, на обрыве связи и по сроку. */
static void fw_session_abort(void) {
    if (!fw_up.active) return;
    ESP_LOGW(TAG, "Заливка прошивки прервана на %lu байт",
             (unsigned long)fw_up.written);
    esp_ota_abort(fw_up.handle);
    fw_state_reset();
    fw_guard = NULL;
}

/* Куска нет дольше FW_STALL_MS — заливка мертва */
static uint8_t fw_stalled(void) {
    return (platform_tick() - fw_up.last_tick) > platform_ms_to_ticks(FW_STALL_MS);
}

static int fw_hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* "9dd4…" (32 hex-цифры) → 16 байт. 0 — это не MD5-строка. */
static uint8_t fw_md5_parse(const char *hex, uint8_t out[ESP_ROM_MD5_DIGEST_LEN]) {
    if (!hex || strlen(hex) != 2 * ESP_ROM_MD5_DIGEST_LEN) return 0;
    for (int i = 0; i < ESP_ROM_MD5_DIGEST_LEN; i++) {
        int hi = fw_hex_nibble(hex[2 * i]), lo = fw_hex_nibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return 0;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return 1;
}

/* 16 байт → 32 hex-цифры и нуль: MD5 показывается клиенту текстом */
static void fw_md5_text(const uint8_t *digest, char *out, size_t out_size) {
    static const char hex[] = "0123456789abcdef";
    if (out_size < 2 * ESP_ROM_MD5_DIGEST_LEN + 1) {
        if (out_size) out[0] = '\0';
        return;
    }
    for (int i = 0; i < ESP_ROM_MD5_DIGEST_LEN; i++) {
        out[2 * i]     = hex[digest[i] >> 4];
        out[2 * i + 1] = hex[digest[i] & 0x0f];
    }
    out[2 * ESP_ROM_MD5_DIGEST_LEN] = '\0';
}

/* ── sys.fw.begin: открыть OTA-сессию ───────────────────────────────────── *
 *
 * ARG: {"SIZE": <байт>} — размер образа. Ответ: {"STA":…,"SESSION":1}. Размер
 * проверяется до esp_ota_begin: раздел стирается под заказанный размер, и промах
 * здесь стоил бы стирания не того объёма. */
static uint8_t fw_handle_begin(const sys_service_t *svc, const uint8_t *data,
                               uint32_t len) {
    uint8_t clients = net_wifi_clients_count();

    if (!change_allowed(svc, clients)) {
        ESP_LOGW(TAG, "Прошивка отклонена (from_ap=%u clients=%u)",
                 svc->from_ap, clients);
        resp_denied(svc, clients);
        return 0;
    }

    if (fw_up.active) {
        resp_error(SYS_BUSY, "SESSION",
                   "Another firmware upload is in progress: finish it with sys.fw.end or drop it with sys.fw.abort");
        gw_jsonw_uint(&sys_w, "WRITTEN", fw_up.written);
        return 0;
    }

    GW_JSON_TOKENS(toks, FW_ARG_MAX_TOKENS);
    gw_json_t json;
    int32_t size = 0;

    if (!((data && len) && gw_json_parse(&json, (const char *)data, len, toks, FW_ARG_MAX_TOKENS))) {
        resp_error(SYS_BAD_ARG, "ARG", "ARG must be an object: {\"SIZE\":<bytes>}");
        return 0;
    }
    if (!gw_json_int(&json, "SIZE", &size) || size <= 0) {
        resp_error(SYS_BAD_ARG, "FW_SIZE", "SIZE is the image size in bytes, e.g. {\"SIZE\":933536}");
        return 0;
    }

    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        ESP_LOGE(TAG, "Нет раздела для обновления");
        resp_error(SYS_INTERNAL, "PART", "No OTA partition available");
        return 0;
    }
    if ((uint32_t)size < sizeof(esp_image_header_t) || (uint32_t)size > part->size) {
        char msg[128];
        snprintf(msg, sizeof(msg), "Firmware must be between %u and %u bytes",
                 (unsigned)sizeof(esp_image_header_t), (unsigned)part->size);
        resp_error(SYS_BAD_ARG, "FW_SIZE", msg);
        return 0;
    }

    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(part, (size_t)size, &handle);
    if (err != ESP_OK) {
        char msg[96];
        snprintf(msg, sizeof(msg), "Cannot start the update: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "%s", msg);
        resp_error(SYS_INTERNAL, "OTA", msg);
        return 0;
    }

    fw_state_reset();
    fw_up.active    = 1;
    fw_up.handle    = handle;
    fw_up.part      = part;
    fw_up.size      = (uint32_t)size;
    fw_up.next_seq  = FW_SEQ_FIRST;
    fw_up.last_tick = platform_tick();
    esp_rom_md5_init(&fw_up.md5);

    LOGI(TAG, "Приём прошивки: %u байт в раздел '%s'",
             (unsigned)size, part->label);
    resp_ok();
    gw_jsonw_uint(&sys_w, "SESSION", 1);
    return 1;
}

/* ── sys.fw.write: принять порцию ───────────────────────────────────────── *
 *
 * ARG: {"SEQ": <номер порции>, "DATA": "<base64>"} — ответ
 * {"STA":…,"WRITTEN":<сколько байт принято всего>}. Данные декодируются в кучу:
 * порция — до 8 КБ (11 КБ base64), на стеке задачи её держать незачем. */
static uint8_t fw_handle_write(const uint8_t *data, uint32_t len) {
    GW_JSON_TOKENS(toks, FW_ARG_MAX_TOKENS);
    gw_json_t json;
    int32_t seq = 0;
    const char *b64 = NULL;
    size_t b64_len = 0;

    if (!fw_up.active) {
        resp_error(SYS_BAD_ARG, "SESSION", "No firmware upload in progress: call sys.fw.begin first");
        return 0;
    }
    if (fw_stalled()) {
        fw_session_abort();
        resp_error(SYS_TIMEOUT, "SESSION", "No chunk arrived in time: the upload was aborted");
        return 0;
    }

    if (!((data && len) && gw_json_parse(&json, (const char *)data, len, toks, FW_ARG_MAX_TOKENS))) {
        resp_error(SYS_BAD_ARG, "ARG",
                   "ARG must be an object: {\"SEQ\":<chunk number>,\"DATA\":\"<base64>\"}");
        return 0;
    }
    if (!gw_json_int(&json, "SEQ", &seq) || seq < 0) {
        resp_error(SYS_BAD_ARG, "SEQ", "SEQ is the chunk number; the first chunk is number 1");
        return 0;
    }
    if ((uint32_t)seq != fw_up.next_seq) {
        /* Порция не та, которую ждём: потерянный кадр записал бы образ со
         * сдвигом. Клиент повторяет порцию с номером EXPECT. */
        resp_error(SYS_BAD_ARG, "SEQ",
                   "SEQ must be the number of the next chunk; resend it with the number from EXPECT");
        gw_jsonw_uint(&sys_w, "EXPECT", fw_up.next_seq);
        return 0;
    }
    if (!gw_json_raw(&json, "DATA", &b64, &b64_len))
        b64_len = 0;                          /* DATA может быть пустой строкой */
    if ((b64_len & 3) != 0) {
        resp_error(SYS_BAD_ARG, "DATA", "DATA must be base64 without spaces or line breaks");
        return 0;
    }

    int32_t decoded_len = b64_len
                          ? base64_decode_buffer_required((const uint8_t *)b64, (uint32_t)b64_len)
                          : 0;
    if (decoded_len < 0) {
        resp_error(SYS_BAD_ARG, "DATA", "DATA must be base64 without spaces or line breaks");
        return 0;
    }
    if (fw_up.written + (uint32_t)decoded_len > fw_up.size) {
        fw_session_abort();
        resp_error(SYS_BAD_ARG, "FW_SIZE", "More data than SIZE announced in sys.fw.begin");
        return 0;
    }

    uint8_t *buf = decoded_len ? malloc((size_t)decoded_len) : NULL;
    if (decoded_len && !buf) {
        resp_error(SYS_NO_MEM, "NOMEM", "Out of memory");
        return 0;
    }
    if (decoded_len) {
        int32_t decoded = base64_decode(buf, (uint32_t)decoded_len,
                                        (const uint8_t *)b64, (uint32_t)b64_len);
        /* decoded != decoded_len — мусор в середине строки: декодер остановился
         * бы на первом '=' и записал обрезанную порцию. */
        if (decoded != decoded_len) {
            free(buf);
            resp_error(SYS_BAD_ARG, "DATA", "DATA must be base64 without spaces or line breaks");
            return 0;
        }
    }

    /* Пустая порция законна: писать в flash нечего, но номер порции и срок стражи
     * она двигает, как любая другая. */
    esp_err_t err = ESP_OK;
    if (decoded_len) {
        err = esp_ota_write(fw_up.handle, buf, (size_t)decoded_len);
        if (err == ESP_OK) {
            esp_rom_md5_update(&fw_up.md5, buf, (uint32_t)decoded_len);
            fw_up.written += (uint32_t)decoded_len;
        }
    }
    free(buf);

    if (err != ESP_OK) {
        fw_session_abort();                   /* сюда попадает и неверный образ */
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            resp_error(SYS_BAD_ARG, "IMAGE", "This file is not a firmware image for this device");
            return 0;
        }
        char msg[96];
        snprintf(msg, sizeof(msg), "Writing failed: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "%s", msg);
        resp_error(SYS_INTERNAL, "OTA", msg);
        return 0;
    }

    fw_up.next_seq += 1;
    fw_up.last_tick = platform_tick();
    resp_ok();
    gw_jsonw_uint(&sys_w, "WRITTEN", fw_up.written);
    return 1;
}

/* ── sys.fw.end: проверить образ и перезагрузиться в него ───────────────── *
 *
 * ARG: {} или {"MD5": "<32 hex>"} — MD5 по самому образу, если клиент его считал.
 * Ответ: {"STA":…,"OK":1,"REBOOT":1}; перезагрузку задача откладывает, чтобы
 * ответ успел уйти. */
static uint8_t fw_handle_end(const uint8_t *data, uint32_t len) {
    GW_JSON_TOKENS(toks, FW_ARG_MAX_TOKENS);
    gw_json_t json;
    char md5_hex[2 * ESP_ROM_MD5_DIGEST_LEN + 1] = {0};
    uint8_t want_md5 = 0;
    uint8_t digest[ESP_ROM_MD5_DIGEST_LEN];

    if (!fw_up.active) {
        resp_error(SYS_BAD_ARG, "SESSION", "No firmware upload in progress: call sys.fw.begin first");
        return 0;
    }
    if (fw_stalled()) {
        fw_session_abort();
        resp_error(SYS_TIMEOUT, "SESSION", "No chunk arrived in time: the upload was aborted");
        return 0;
    }
    if (data && len) {
        if (!gw_json_parse(&json, (const char *)data, len, toks, FW_ARG_MAX_TOKENS)) {
            resp_error(SYS_BAD_ARG, "ARG", "ARG must be an object: {} or {\"MD5\":\"<32 hex>\"}");
            return 0;
        }
        if (gw_json_str(&json, "MD5", md5_hex, sizeof(md5_hex)))
            want_md5 = 1;
    }

    /* Обрывок образа образом не станет: esp_ota_end проверяет заголовок и
     * целостность, но недостачу принятых байт видно только здесь. */
    if (fw_up.written != fw_up.size) {
        char msg[96];
        snprintf(msg, sizeof(msg), "Received %lu of %lu bytes",
                 (unsigned long)fw_up.written, (unsigned long)fw_up.size);
        fw_session_abort();
        resp_error(SYS_BAD_ARG, "FW_SIZE", msg);
        return 0;
    }

    esp_rom_md5_final(digest, &fw_up.md5);
    if (want_md5) {
        uint8_t got[ESP_ROM_MD5_DIGEST_LEN];
        if (!fw_md5_parse(md5_hex, got)) {
            fw_session_abort();
            resp_error(SYS_BAD_ARG, "MD5", "MD5 must be 32 hex digits");
            return 0;
        }
        if (memcmp(got, digest, sizeof(got)) != 0) {
            char text[2 * ESP_ROM_MD5_DIGEST_LEN + 1], msg[128];
            fw_md5_text(digest, text, sizeof(text));
            snprintf(msg, sizeof(msg), "MD5 mismatch: the image is %s", text);
            fw_session_abort();
            resp_error(SYS_BAD_ARG, "MD5", msg);
            return 0;
        }
    }

    esp_ota_handle_t handle = fw_up.handle;
    const esp_partition_t *part = fw_up.part;
    uint32_t written = fw_up.written;
    (void)written;   /* без журнала не используется */

    /* esp_ota_end при отказе освобождает сессию сам: после него abort звать
     * нельзя, поэтому состояние сбрасываем в любом случае. */
    esp_err_t err = esp_ota_end(handle);
    if (err != ESP_OK) {
        char msg[96];
        snprintf(msg, sizeof(msg), "Not a valid firmware image: %s", esp_err_to_name(err));
        fw_state_reset();
        fw_guard = NULL;
        ESP_LOGE(TAG, "%s", msg);
        resp_error(SYS_BAD_ARG, "IMAGE", msg);
        return 0;
    }

    err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) {
        char msg[96];
        snprintf(msg, sizeof(msg), "Cannot select the new firmware: %s", esp_err_to_name(err));
        fw_state_reset();
        fw_guard = NULL;
        ESP_LOGE(TAG, "%s", msg);
        resp_error(SYS_INTERNAL, "OTA", msg);
        return 0;
    }

    fw_state_reset();
    fw_guard = NULL;
    LOGI(TAG, "Прошивка записана в '%s' (%lu байт), перезагрузка через %d мс",
             part->label, (unsigned long)written, FW_REBOOT_DELAY_MS);
    resp_ok();
    gw_jsonw_int(&sys_w, "OK", 1);
    gw_jsonw_int(&sys_w, "REBOOT", 1);
    return 1;
}

/* ── sys.fw.abort: отказаться от заливки ────────────────────────────────── *
 *
 * ARG: {} — ответ {"STA":…,"OK":1}. Отказ идемпотентен: «заливки нет» — тоже
 * успех. */
static void fw_handle_abort(void) {
    fw_session_abort();
    resp_ok();
    gw_jsonw_int(&sys_w, "OK", 1);
}

/* ── Такт сторожа ───────────────────────────────────────────────────────── *
 *
 * Вызов begin/write остаётся открытым (обработчик вернул TRUE), и ядро зовёт его каждый
 * такт с data == NULL. Здесь служба узнаёт, что заливка застряла (куска нет
 * FW_STALL_MS) или что её сменил новый вызов — тогда этот пора закрывать, иначе на
 * каждый кусок висело бы по открытому вызову. */
static uint8_t fw_guard_tick(void *call) {
    if (!fw_up.active || (call != fw_guard)) {
        if (call == fw_guard) fw_guard = NULL;
        return 0;
    }
    if (fw_stalled()) {
        ESP_LOGW(TAG, "Куска нет дольше %d мс — заливка прервана", FW_STALL_MS);
        fw_session_abort();
        return 0;
    }
    return 1;
}

/* ── Обработчики вызовов ─────────────────────────────────────────────────── *
 *
 * Контракт (api.h): обработчик не блокирующий, отвечает сам. FALSE — вызов завершён,
 * TRUE — вызов остаётся открытым и получит такт с data == NULL (этим пользуется только
 * приём порции, см. fw_guard_tick). call == NULL — вызов снят: клиент ушёл, заливка не
 * нужна никому, поэтому она абортится. */

static uint8_t sys_fw_begin_handler(void *call, void **context, uint8_t *data, uint32_t data_len) {
    if (!call) { fw_session_abort(); return 0; }
    if (data == NULL) return fw_guard_tick(call);
    const sys_service_t *svc = svc_of(context);
    if (!svc) return 0;
    resp_begin();
    uint8_t keep = fw_handle_begin(svc, data, data_len);
    send_response(call);
    if (!keep) return 0;
    fw_guard = call;
    return 1;
}

static uint8_t sys_fw_write_handler(void *call, void **context, uint8_t *data, uint32_t data_len) {
    (void)context;
    if (!call) { fw_session_abort(); return 0; }
    if (data == NULL) return fw_guard_tick(call);
    resp_begin();
    uint8_t keep = fw_handle_write(data, data_len);
    send_response(call);
    if (!keep) return 0;
    fw_guard = call;
    return 1;
}

static void fw_reboot_task(void *arg) {
    (void)arg;
    platform_delay_ms(FW_REBOOT_DELAY_MS);
    LOGI(TAG, "Перезагрузка в принятую прошивку");
    esp_restart();
}

static uint8_t sys_fw_end_handler(void *call, void **context, uint8_t *data, uint32_t data_len) {
    (void)context;
    if (!call) { fw_session_abort(); return 0; }
    resp_begin();
    uint8_t reboot = fw_handle_end(data, data_len);
    /* Ответ уходит клиенту до перезагрузки: задача её откладывает, а отправка
     * асинхронна — соединение успевает получить конверт ответа. */
    send_response(call);
    if (reboot)
        platform_task_create(fw_reboot_task, "fw_reboot", 2048, NULL, 5);
    return 0;
}

static uint8_t sys_fw_abort_handler(void *call, void **context, uint8_t *data, uint32_t data_len) {
    (void)context; (void)data; (void)data_len;
    if (!call) { fw_session_abort(); return 0; }
    resp_begin();
    fw_handle_abort();
    send_response(call);
    return 0;
}

esp_err_t apih_fw_run(app_context_t *app) {
    /* Только AP-ядро: прошивку принимают лишь с точки доступа, когда
     * администратор рядом с прибором. Контекст регистрации — sys_svc_ap: по нему
     * обработчик отвечает AP_CLIENTS, пока на точке доступа не ровно один клиент. */
    api_t *ap = app->api_ap;
    uint8_t ok = 1;
    ok &= register_service(ap, sys_fw_begin_handler, ESP_WS_API_SYS_FW_BEGIN, &sys_svc_ap);
    ok &= register_service(ap, sys_fw_write_handler, ESP_WS_API_SYS_FW_WRITE, &sys_svc_ap);
    ok &= register_service(ap, sys_fw_end_handler,   ESP_WS_API_SYS_FW_END,   &sys_svc_ap);
    ok &= register_service(ap, sys_fw_abort_handler, ESP_WS_API_SYS_FW_ABORT, &sys_svc_ap);
    return ok ? ESP_OK : ESP_FAIL;
}

/* ── Хостовые тесты ─────────────────────────────────────────────────────── *
 *
 * Состояние службы статическое (как и на приборе), поэтому тестам нужен сброс между
 * случаями: иначе OTA-сессия одного теста видна другому. */
#ifdef APIH_HOST_TEST
void apih_fw_test_reset(void) {
    fw_session_abort();
    fw_state_reset();
    fw_guard = NULL;
}
#endif
