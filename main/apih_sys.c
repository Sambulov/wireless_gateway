#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#include "app.h"
#include "uart.h"
#include "GwJson.h"
#include "web_api.h"
#include "ws_hal.h"
#include "fs_path.h"

#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_littlefs.h"
#include "esp_wifi.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"

#include <lwip/sockets.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* System API (FID names echo, wifi.sta/ap, sys.info/reboot/files/net and the file
 * manager sys.file.write/delete/rename, see app.h).
 *
 * Каждый метод — обработчик, зарегистрированный в ядре API на СВОИ интерфейсы
 * (bApiCallRegister с маской GW_IFACE_*): сервис оказывается не «вообще всем», а
 * тому интерфейсу, с которого пришёл клиент. Методы, которые меняют настройки,
 * зарегистрированы ДВАЖДЫ — для точки доступа (запись разрешена) и для любого
 * другого интерфейса (только чтение); различие передано контекстом регистрации
 * (sys_service_t), и обработчик по нему понимает, что этому клиенту можно.
 * Политики «по номеру сокета» здесь больше нет: сокет — дело транспорта.
 *
 * Ответ отправляется прямо из обработчика (bApiCallSendJson): ядро само
 * оборачивает его в конверт {"FID":..,"SID":..,"ARG":<json>} и завершает вызов.
 * Ответы всегда несут "STA", коды — из web_api.h (API_CALL_*). Своей очереди и
 * задачи у системного API нет: вызов идёт по тому же контракту, что у Modbus. */

#define REBOOT_DELAY_MS 1000

#define SSID_MAX 31

/* Токенов на разбор ARG запросов конфигурации: несколько строковых полей. */
#define SYS_ARG_MAX_TOKENS 24

/* Адрес в виде строки: "255.255.255.255" плюс запас. */
#define NW_ADDR_MAX 24
#define PASS_MIN 8
#define PASS_MAX 63

/* Response codes used by this API */
#define SYS_OK          "0x00000000"
#define SYS_BAD_ARG     "0x80000003"
#define SYS_NO_MEM      "0x80000006"
#define SYS_NO_ACCESS   "0x80000007"
#define SYS_BUSY        "0x80000008"
#define SYS_TIMEOUT     "0x80000009"
#define SYS_INTERNAL    "0x8000000f"

/* ── helpers ─────────────────────────────────────────────────────────────── */

static void netif_ip_str(const char *ifkey, char *out, size_t out_size) {
    out[0] = '\0';
    esp_netif_t *nif = esp_netif_get_handle_from_ifkey(ifkey);
    if (!nif) return;
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(nif, &ip) != ESP_OK || ip.ip.addr == 0) return;
    snprintf(out, out_size, IPSTR, IP2STR(&ip.ip));
}

/* Ответ собирается прямо в буфер, без дерева и без узлов: раньше на каждый
 * объект, ключ и значение прежний сборщик делал отдельный malloc, а потом весь
 * ответ печатался ещё одной аллокацией.
 *
 * Буфер статический, а не на стеке: обработчики обслуживаются по одному, а стек
 * задачи ядра API 8 КБ, тогда как листинг каталога бывает в килобайты. Копию
 * готового текста в конверт ответа делает ядро (bApiCallSendJson). */
/* Хватает с запасом: самый большой ответ — листинг каталога (имя файла до 255
 * байт плюс размер и признак каталога, то есть ~290 байт на запись), остальные
 * ответы в сотни байт. Не влезло — отвечаем отказом NOMEM, за границу буфера не
 * пишем. */
#define SYS_RESP_MAX 8192
static char       sys_resp_buf[SYS_RESP_MAX];
static gw_jsonw_t sys_w;

/* Корень ответа — объект; дальше обработчик дописывает поля */
static void resp_begin(void) {
    gw_jsonw_reset(&sys_w);
    gw_jsonw_obj(&sys_w, NULL);
}

/* A short machine-readable failure for the configuration page */
static void resp_error(const char *sta, const char *err, const char *msg) {
    gw_jsonw_str(&sys_w, "STA", sta);
    if (err) gw_jsonw_str(&sys_w, "ERR", err);
    if (msg) gw_jsonw_str(&sys_w, "MSG", msg);
}

static void resp_ok(void) {
    resp_error(SYS_OK, NULL, NULL);
}

/* Разбор и печать MAC-адреса: сами функции ниже (раздел wifi.mac), но нужны и
 * сервисам Wi-Fi, которые идут раньше по тексту — привязке к выбранной точке. */
#define MAC_TEXT_LEN 18   /* "aa:bb:cc:dd:ee:ff" — 17 символов и нуль */
static uint8_t mac_parse(const char *s, uint8_t *out);
static void    mac_text(const uint8_t *mac, char *out, size_t out_size);

/* ── Экземпляры сервиса ─────────────────────────────────────────────────── *
 *
 * Контекст регистрации: по нему обработчик понимает, ЧЕРЕЗ КАКОЙ ИНТЕРФЕЙС его
 * вызвали и что этому клиенту можно. Один и тот же обработчик регистрируется
 * несколько раз — на разные интерфейсы, с разными контекстами; нужный выбирает
 * ядро по маске регистрации (см. bApiCallRegister).
 *
 * Wi-Fi и сетевые настройки меняет только администратор: он подключён к точке
 * доступа, и она — единственный клиент на радио. Клиент станционной сети
 * перестроить шлюз не может, а пока на точке доступа двое, не может никто:
 * второй мог бы подглядывать или вмешаться в обмен. То же условие решает,
 * показывать ли сохранённые пароли.
 *
 * «Ровно один клиент на точке доступа» берётся с СЕТЕВОГО ДРАЙВЕРА
 * (wifi_ap_clients_count), а не у транспорта: сколько станций на радио — знает
 * Wi-Fi, а не тот, кто обслуживает соединения.
 */

typedef struct {
    app_context_t *app;
    uint8_t from_ap;     /* клиент пришёл через нашу точку доступа */
    uint8_t may_change;  /* ему разрешено менять состояние (настройки, перезагрузка) */
} sys_service_t;

/* Точка доступа: администратор рядом с прибором — менять можно. Это регистрация
 * с маской GW_IFACE_MASK(GW_IFACE_AP). */
static sys_service_t sys_svc_ap;
/* Любой другой интерфейс (и неизвестный): только чтение. Регистрация с маской
 * GW_IFACE_MASK_ANY обслуживает и те соединения, чей интерфейс транспорт не
 * определил: неизвестно, что клиенту можно, — значит показываем, но не меняем. */
static sys_service_t sys_svc_ro;
/* Перезагрузка — не настройка: она и раньше была доступна с любого интерфейса
 * (кнопка на странице не зависит от CAN_CONFIGURE), и снаружи поведение не
 * меняется. Сузить её до точки доступа — поставить здесь may_change = 0:
 * обработчик ответит AP_ONLY. */
static sys_service_t sys_svc_mut;

/* Отдать ответ клиенту. Буфер сборщика остаётся у сервиса, а ядро получает
 * готовый текст: оно само положит его в конверт вызова ({"FID":..,"SID":..,
 * "ARG":<json>}) и завершит вызов. */
static void send_response(void *call) {
    if (!gw_jsonw_ok(&sys_w)) {
        ESP_LOGE(TAG, "Ответ не влез в %d байт — отвечаю отказом", SYS_RESP_MAX);
        resp_begin();
        resp_error(SYS_NO_MEM, "NOMEM", NULL);
    }
    size_t len = gw_jsonw_finish(&sys_w);
    if (!bApiCallSendJson(call, (const uint8_t *)sys_resp_buf, (uint32_t)len))
        ESP_LOGW(TAG, "Ответ клиенту не ушёл");
}

/* Условия на самом приборе: администратор — клиент точки доступа, и он там
 * один. От интерфейса (регистрации) это не зависит: сколько станций на радио,
 * решает драйвер. */
static uint8_t config_allowed(const sys_service_t *svc, uint8_t clients) {
    return svc->from_ap && (clients == 1);
}

/* Можно ли этому клиенту менять состояние: экземпляр сервиса это позволяет
 * (регистрация на интерфейс) и условия на приборе выполнены. */
static uint8_t change_allowed(const sys_service_t *svc, uint8_t clients) {
    return svc->may_change && config_allowed(svc, clients);
}

/* Refusal that tells the page what exactly is missing */
static void resp_denied(const sys_service_t *svc, uint8_t clients) {
    if (svc->from_ap)
        resp_error(SYS_NO_ACCESS, "AP_CLIENTS",
                   "Exactly one client must be connected to the softAP");
    else
        resp_error(SYS_NO_ACCESS, "AP_ONLY",
                   "Wi-Fi and network settings can only be changed from the softAP");
    gw_jsonw_int(&sys_w, "CLIENTS", clients);
}

/* ── wifi.sta: station (STA) credentials ────────────────────────────────────── */

/* Переподключение станции откладывается на полсекунды: сначала должен уйти ответ
 * (см. handle_sta). */
#define STA_APPLY_DELAY_MS 500

static void vStaApplyTask(void *arg) {
    app_context_t *app = (app_context_t *)arg;
    vTaskDelay(pdMS_TO_TICKS(STA_APPLY_DELAY_MS));
    ESP_LOGW(TAG, "Applying station credentials, reconnecting");
    /* Привязка к выбранной точке — часть настроек станции: пусто значит «любая
     * точка с этим именем, сильнейшая». */
    wifi_sta_set_target(&app->sta_cnf, app->sta_bssid);
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &app->sta_cnf);
    if (err == ESP_OK) {
        esp_wifi_disconnect();
        err = esp_wifi_connect();
    }
    if (err != ESP_OK)
        ESP_LOGE(TAG, "STA configuration failed: %s", esp_err_to_name(err));
    vTaskDelete(NULL);
}

static void handle_sta(const sys_service_t *svc, const uint8_t *data, uint32_t len) {
    app_context_t *app = svc->app;
    uint8_t clients = wifi_ap_clients_count();
    uint8_t allowed = config_allowed(svc, clients);

    /* A request without SSID/PASS — an empty ARG included — reads the stored
     * credentials, which is what the configuration page fills its form with.
     * The password itself is only disclosed under the same conditions that
     * allow changing it. */
    /* Поля читаются jsmn'ом сразу в свой буфер: дерева и его узлов нет. */
    GW_JSON_TOKENS(toks, SYS_ARG_MAX_TOKENS);
    gw_json_t json;
    char ssid[SSID_MAX + 1] = {0};
    char pass[PASS_MAX + 1] = {0};
    char bssid_in[MAC_TEXT_LEN + 8] = {0};
    uint8_t bssid_new[6] = {0};
    uint8_t parsed = (data && len)
                     && gw_json_parse(&json, (const char *)data, len, toks, SYS_ARG_MAX_TOKENS);
    uint8_t write = parsed
                    && gw_json_str(&json, "SSID", ssid, sizeof(ssid))
                    && gw_json_str(&json, "PASS", pass, sizeof(pass));

    if (write) {
        /* BSSID — необязательное поле: пусто (или его нет) значит «любая точка с
         * этим именем». Так страница может и привязать к выбранной из таблицы
         * точке, и отказаться от привязки, не меняя остального. */
        uint8_t mac[6];
        gw_json_str(&json, "BSSID", bssid_in, sizeof(bssid_in));
        if (bssid_in[0] == '\0') {
            memset(bssid_new, 0, sizeof(bssid_new));
        } else if (!mac_parse(bssid_in, mac) || !wifi_mac_valid(mac)) {
            resp_error(SYS_BAD_ARG, "BSSID", "BSSID must look like aa:bb:cc:dd:ee:ff");
            return;
        } else {
            memcpy(bssid_new, mac, sizeof(bssid_new));
        }
    }

    if (!write) {
        char pin[MAC_TEXT_LEN];
        mac_text(app->sta_bssid, pin, sizeof(pin));   /* нули печатаются как есть */
        resp_ok();
        gw_jsonw_str(&sys_w, "SSID", (char *)app->sta_cnf.sta.ssid);
        gw_jsonw_str(&sys_w, "PASS", allowed ? (char *)app->sta_cnf.sta.password : "");
        gw_jsonw_bool(&sys_w, "PASS_HIDDEN", !allowed);
        /* Выбранная точка: пусто — привязки нет. BSSID_ACTIVE отвечает, действует
         * ли она прямо сейчас: если выбранной точки не было в эфире, устройство
         * сняло привязку, чтобы не остаться без связи (в NVS выбор сохранён). */
        wifi_config_t live;
        uint8_t pin_active = (esp_wifi_get_config(WIFI_IF_STA, &live) == ESP_OK)
                             && live.sta.bssid_set;
        gw_jsonw_str(&sys_w, "BSSID", wifi_mac_valid(app->sta_bssid) ? pin : "");
        gw_jsonw_bool(&sys_w, "BSSID_SET", wifi_mac_valid(app->sta_bssid));
        gw_jsonw_bool(&sys_w, "BSSID_ACTIVE", pin_active);
        gw_jsonw_bool(&sys_w, "FROM_AP", svc->from_ap);
        gw_jsonw_int(&sys_w, "CLIENTS", clients);
        return;
    }

    if (!change_allowed(svc, clients)) {
        ESP_LOGW(TAG, "STA credentials refused (from_ap=%u clients=%u, may_change=%u)",
                 svc->from_ap, clients, svc->may_change);
        resp_denied(svc, clients);
        return;
    }

    if (ssid[0] == '\0' || strlen(ssid) > SSID_MAX) {
        resp_error(SYS_BAD_ARG, "SSID", "SSID must be 1..31 characters");
        return;
    }
    if (!wifi_password_valid(pass)) {
        resp_error(SYS_BAD_ARG, "PASS", "Password must be 8..63 characters or empty");
        return;
    }

    /* Сменилось имя сети — прежняя привязка к точке относится к другой сети и
     * смысла не имеет. */
    if (strcmp((char *)app->sta_cnf.sta.ssid, ssid) != 0)
        memset(bssid_new, 0, sizeof(bssid_new));

    uint8_t changed = (strcmp((char *)app->sta_cnf.sta.ssid, ssid) != 0)
                   || (strcmp((char *)app->sta_cnf.sta.password, pass) != 0)
                   || (memcmp(app->sta_bssid, bssid_new, 6) != 0);

    strlcpy((char *)app->sta_cnf.sta.ssid, ssid, sizeof(app->sta_cnf.sta.ssid));
    strlcpy((char *)app->sta_cnf.sta.password, pass, sizeof(app->sta_cnf.sta.password));
    memcpy(app->sta_bssid, bssid_new, sizeof(app->sta_bssid));
    if (changed) {
        save_wifi_sta_config(ssid, pass);
        save_wifi_sta_bssid(wifi_mac_valid(bssid_new) ? bssid_new : NULL);
    }

    /* Ответ уходит ДО применения. Станция переподключается (disconnect/connect),
     * а это событие двигает радио: пока ответ не отправлен, клиент рискует его
     * не получить вовсе (проверено: ответ терялся, страница вечно показывала
     * «Сохраняем…»). Так же поступает sys.net с адресом точки доступа.
     * Если ничего не менялось, радио не трогаем совсем. */
    resp_ok();
    gw_jsonw_str(&sys_w, "SSID", (char *)app->sta_cnf.sta.ssid);
    char pin[MAC_TEXT_LEN];
    mac_text(app->sta_bssid, pin, sizeof(pin));
    gw_jsonw_str(&sys_w, "BSSID", wifi_mac_valid(app->sta_bssid) ? pin : "");
    gw_jsonw_bool(&sys_w, "BSSID_SET", wifi_mac_valid(app->sta_bssid));
    gw_jsonw_bool(&sys_w, "CHANGED", changed);

    if (!changed) {
        ESP_LOGI(TAG, "STA credentials unchanged — nothing to apply");
        return;
    }

    gw_jsonw_bool(&sys_w, "RECONNECT", 1);
    if (xTaskCreate(vStaApplyTask, "sta_apply", 3072, app, 5, NULL) != pdPASS)
        ESP_LOGE(TAG, "Failed to schedule station apply");
    ESP_LOGI(TAG, "STA set to SSID '%s', reconnecting...", app->sta_cnf.sta.ssid);
}

/* ── wifi.ap: softAP credentials ───────────────────────────────────────────── */

/* Как и у станции: сначала ответ, потом применение. Запись настроек работающей
 * точки доступа её перезапускает и рассылает deauth (проверено по логу платы),
 * поэтому ответ, отправленный после, до клиента уже не доходит. */
#define AP_APPLY_DELAY_MS 500

static void vApApplyTask(void *arg) {
    app_context_t *app = (app_context_t *)arg;
    vTaskDelay(pdMS_TO_TICKS(AP_APPLY_DELAY_MS));
    ESP_LOGW(TAG, "Applying access point credentials (its clients will be dropped)");
    esp_err_t err = esp_wifi_set_config(WIFI_IF_AP, &app->ap_cnf);
    if (err != ESP_OK)
        ESP_LOGE(TAG, "AP configuration failed: %s", esp_err_to_name(err));
    vTaskDelete(NULL);
}

static void handle_ap(const sys_service_t *svc, const uint8_t *data, uint32_t len) {
    app_context_t *app = svc->app;
    uint8_t clients = wifi_ap_clients_count();
    uint8_t allowed = config_allowed(svc, clients);

    GW_JSON_TOKENS(toks, SYS_ARG_MAX_TOKENS);
    gw_json_t json;
    char ssid[SSID_MAX + 1] = {0};
    char pass[PASS_MAX + 1] = {0};
    uint8_t parsed = (data && len)
                     && gw_json_parse(&json, (const char *)data, len, toks, SYS_ARG_MAX_TOKENS);
    uint8_t write = parsed
                    && gw_json_str(&json, "SSID", ssid, sizeof(ssid))
                    && gw_json_str(&json, "PASS", pass, sizeof(pass));

    if (!write) {
        resp_ok();
        gw_jsonw_str(&sys_w, "SSID", (char *)app->ap_cnf.ap.ssid);
        gw_jsonw_str(&sys_w, "PASS", allowed ? (char *)app->ap_cnf.ap.password : "");
        gw_jsonw_bool(&sys_w, "PASS_HIDDEN", !allowed);
        gw_jsonw_bool(&sys_w, "FROM_AP", svc->from_ap);
        gw_jsonw_int(&sys_w, "CLIENTS", clients);
        return;
    }

    if (!change_allowed(svc, clients)) {
        ESP_LOGW(TAG, "AP credentials refused (from_ap=%u clients=%u, may_change=%u)",
                 svc->from_ap, clients, svc->may_change);
        resp_denied(svc, clients);
        return;
    }

    if (ssid[0] == '\0' || strlen(ssid) > SSID_MAX) {
        resp_error(SYS_BAD_ARG, "SSID", "SSID must be 1..31 characters");
        return;
    }
    if (!wifi_password_valid(pass)) {
        resp_error(SYS_BAD_ARG, "PASS", "Password must be 8..63 characters or empty");
        return;
    }

    uint8_t changed = (strcmp((char *)app->ap_cnf.ap.ssid, ssid) != 0)
                   || (strcmp((char *)app->ap_cnf.ap.password, pass) != 0);

    strlcpy((char *)app->ap_cnf.ap.ssid, ssid, sizeof(app->ap_cnf.ap.ssid));
    app->ap_cnf.ap.ssid_len = strlen(ssid);
    strlcpy((char *)app->ap_cnf.ap.password, pass, sizeof(app->ap_cnf.ap.password));
    app->ap_cnf.ap.authmode = (pass[0] != '\0') ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    /* Ответ уходит ДО применения: точка доступа при смене настроек перезапускает
     * радио и выбивает всех своих клиентов, включая того, кто ждёт этот ответ.
     * Ничего не менялось — радио не трогаем совсем (страница присылает имя и
     * пароль при каждом сохранении, в том числе когда правили только MAC). */
    resp_ok();
    gw_jsonw_str(&sys_w, "SSID", (char *)app->ap_cnf.ap.ssid);
    gw_jsonw_int(&sys_w, "CLIENTS", clients);
    gw_jsonw_bool(&sys_w, "CHANGED", changed);

    if (!changed) {
        ESP_LOGI(TAG, "AP credentials unchanged — nothing to apply");
        return;
    }

    save_wifi_ap_config(ssid, pass);
    ESP_LOGW(TAG, "AP set to SSID '%s' (password %s): its clients will reconnect",
             app->ap_cnf.ap.ssid,
             app->ap_cnf.ap.password[0] ? "changed" : "cleared, network is open");

    gw_jsonw_bool(&sys_w, "RECONNECT", 1);
    if (xTaskCreate(vApApplyTask, "ap_apply", 3072, app, 5, NULL) != pdPASS)
        ESP_LOGE(TAG, "Failed to schedule access point apply");
}

/* ── wifi.mac: MAC адрес интерфейса ────────────────────────────────────────── *
 *
 * Драйвер принимает новый MAC только на ОСТАНОВЛЕННОМ интерфейсе, а интерфейс
 * у нас работает всегда: гасить радио на время правки — значит рвать ту самую
 * связь, по которой пришла команда. Поэтому адрес сохраняется в NVS и
 * применяется при следующей загрузке (wifi_init_ap_sta), а страница честно
 * говорит, что нужна перезагрузка.
 *
 * Заводской адрес берётся не «из ниоткуда»: он прочитан из eFuse при старте
 * (load_config) и лежит в app->mac_*_factory — после подмены адреса спросить
 * его у eFuse ещё раз уже не получится. */

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* "aa:bb:cc:dd:ee:ff", "aa-bb-…", "aabb.ccdd.eeff" или просто 12 hex-цифр.
 * 0 — разобрать не удалось (в out при этом ничего осмысленного). */
static uint8_t mac_parse(const char *s, uint8_t *out) {
    uint8_t nib = 0;
    if (!s) return 0;
    for (; *s; s++) {
        if (*s == ':' || *s == '-' || *s == '.' || *s == ' ') continue;
        int v = hex_nibble(*s);
        if (v < 0 || nib >= 12) return 0;
        if ((nib & 1) == 0) out[nib >> 1] = (uint8_t)(v << 4);
        else                out[nib >> 1] |= (uint8_t)v;
        nib++;
    }
    return nib == 12;
}

static void mac_text(const uint8_t *mac, char *out, size_t out_size) {
    snprintf(out, out_size, MACSTR, MAC2STR(mac));
}

/* Текущий адрес интерфейса спрашиваем у драйвера, а не у своих записей: после
 * перезагрузки истина только там, а наши записи — это «что заказано». */
static void mac_runtime(wifi_interface_t iface, const uint8_t *factory, char *out, size_t out_size) {
    uint8_t mac[6];
    if (esp_wifi_get_mac(iface, mac) == ESP_OK) mac_text(mac, out, out_size);
    else                                        mac_text(factory, out, out_size);
}

/* Что будет у интерфейса после перезагрузки: личный адрес, а без него — заводской. */
static const uint8_t *mac_effective(const uint8_t *custom, const uint8_t *factory) {
    return wifi_mac_valid(custom) ? custom : factory;
}

/* Заказан ли адрес, который ещё не действует: пока это так, странице есть что
 * сказать про перезагрузку. */
static uint8_t mac_iface_pending(const app_context_t *app, uint8_t ap) {
    uint8_t mac[6];
    const uint8_t *want = mac_effective(ap ? app->mac_ap : app->mac_sta,
                                        ap ? app->mac_ap_factory : app->mac_sta_factory);
    if (esp_wifi_get_mac(ap ? WIFI_IF_AP : WIFI_IF_STA, mac) != ESP_OK)
        return 0;
    return memcmp(mac, want, 6) != 0;
}

static uint8_t mac_pending(const app_context_t *app) {
    if (mac_iface_pending(app, 0)) return 1;
#if CONFIG_WIFI_AP_ENABLE
    if (mac_iface_pending(app, 1)) return 1;
#endif
    return 0;
}

static void handle_mac(const sys_service_t *svc, const uint8_t *data, uint32_t len) {
    app_context_t *app = svc->app;
    uint8_t clients = wifi_ap_clients_count();

    GW_JSON_TOKENS(toks, SYS_ARG_MAX_TOKENS);
    gw_json_t json;
    char iface[8] = {0};
    char mac_in[MAC_TEXT_LEN + 8] = {0};
    uint8_t parsed = (data && len)
                     && gw_json_parse(&json, (const char *)data, len, toks, SYS_ARG_MAX_TOKENS);
    /* Признак записи — поле IF; MAC читается здесь же: пустое значение (или его
     * отсутствие) значит «верни заводской адрес». */
    uint8_t write = parsed && gw_json_str(&json, "IF", iface, sizeof(iface));
    if (write) gw_json_str(&json, "MAC", mac_in, sizeof(mac_in));

    if (!write) {
        char cur_sta[MAC_TEXT_LEN], cur_ap[MAC_TEXT_LEN];
        char fct_sta[MAC_TEXT_LEN], fct_ap[MAC_TEXT_LEN];
        char want_sta[MAC_TEXT_LEN], want_ap[MAC_TEXT_LEN];
        mac_runtime(WIFI_IF_STA, app->mac_sta_factory, cur_sta, sizeof(cur_sta));
        mac_runtime(WIFI_IF_AP,  app->mac_ap_factory,  cur_ap,  sizeof(cur_ap));
        mac_text(app->mac_sta_factory, fct_sta, sizeof(fct_sta));
        mac_text(app->mac_ap_factory,  fct_ap,  sizeof(fct_ap));
        /* WANT_* — что будет действовать после перезагрузки: страница заполняет
         * поля именно этим, а заводской адрес показывает подсказкой. */
        mac_text(mac_effective(app->mac_sta, app->mac_sta_factory), want_sta, sizeof(want_sta));
        mac_text(mac_effective(app->mac_ap,  app->mac_ap_factory),  want_ap,  sizeof(want_ap));

        resp_ok();
        gw_jsonw_str(&sys_w, "MAC_STA", cur_sta);
        gw_jsonw_str(&sys_w, "MAC_AP", cur_ap);
        gw_jsonw_str(&sys_w, "FACTORY_STA", fct_sta);
        gw_jsonw_str(&sys_w, "FACTORY_AP", fct_ap);
        gw_jsonw_str(&sys_w, "WANT_STA", want_sta);
        gw_jsonw_str(&sys_w, "WANT_AP", want_ap);
        gw_jsonw_bool(&sys_w, "CUSTOM_STA", wifi_mac_valid(app->mac_sta));
        gw_jsonw_bool(&sys_w, "CUSTOM_AP", wifi_mac_valid(app->mac_ap));
        gw_jsonw_bool(&sys_w, "PENDING_STA", mac_iface_pending(app, 0));
        gw_jsonw_bool(&sys_w, "PENDING_AP", mac_iface_pending(app, 1));
        gw_jsonw_bool(&sys_w, "RESTART", mac_pending(app));
        gw_jsonw_bool(&sys_w, "CAN_CONFIGURE", change_allowed(svc, clients));
        gw_jsonw_bool(&sys_w, "FROM_AP", svc->from_ap);
        gw_jsonw_int(&sys_w, "CLIENTS", clients);
        return;
    }

    if (!change_allowed(svc, clients)) {
        ESP_LOGW(TAG, "MAC change refused (from_ap=%u clients=%u, may_change=%u)",
                 svc->from_ap, clients, svc->may_change);
        resp_denied(svc, clients);
        return;
    }

    uint8_t is_sta = (strcasecmp(iface, "STA") == 0);
    uint8_t is_ap  = (strcasecmp(iface, "AP") == 0);
    if (!is_sta && !is_ap) {
        resp_error(SYS_BAD_ARG, "IF", "IF must be STA or AP");
        return;
    }
#if !CONFIG_WIFI_AP_ENABLE
    if (is_ap) {
        resp_error(SYS_BAD_ARG, "IF", "The access point is disabled in this build");
        return;
    }
#endif

    /* Пустое поле или слово factory — «верни заводской». Всё остальное — адрес. */
    uint8_t mac[6] = {0};
    uint8_t custom = !(mac_in[0] == '\0'
                       || strcasecmp(mac_in, "factory") == 0
                       || strcasecmp(mac_in, "default") == 0);
    if (custom) {
        if (!mac_parse(mac_in, mac)) {
            resp_error(SYS_BAD_ARG, "MAC", "MAC must look like aa:bb:cc:dd:ee:ff");
            return;
        }
        if (!wifi_mac_valid(mac)) {
            resp_error(SYS_BAD_ARG, "MAC",
                       "MAC must be a unicast address (an even first byte) and not all zeros");
            return;
        }
    }

    uint8_t *slot          = is_sta ? app->mac_sta : app->mac_ap;
    const uint8_t *factory = is_sta ? app->mac_sta_factory : app->mac_ap_factory;
    /* Адрес, совпавший с заводским, — это и есть заводской: так в NVS не
     * появится запись-двойник, а «личный MAC» останется честным признаком. */
    if (custom && memcmp(mac, factory, 6) == 0)
        custom = 0;

    /* Половины радио не могут носить один адрес — драйвер это отвергает. Смотрим
     * на результат применения, а не на то, что прислали. */
    const uint8_t *eff_sta = is_sta ? (custom ? mac : app->mac_sta_factory)
                                    : mac_effective(app->mac_sta, app->mac_sta_factory);
    const uint8_t *eff_ap  = is_ap  ? (custom ? mac : app->mac_ap_factory)
                                    : mac_effective(app->mac_ap, app->mac_ap_factory);
    if (memcmp(eff_sta, eff_ap, 6) == 0) {
        resp_error(SYS_BAD_ARG, "MAC", "The station and the access point cannot share one MAC");
        return;
    }

    if (custom) memcpy(slot, mac, 6);
    else        memset(slot, 0, 6);
    save_wifi_mac_config(is_ap, custom ? mac : NULL);

    /* Ответ говорит и что будет после перезагрузки, и что действует сейчас:
     * страница показывает это пользователю словами («новый адрес такой-то,
     * текущий такой-то, перезагрузите устройство»). Перезагрузку устройство НЕ
     * делает само — адрес ждёт её. */
    char eff[MAC_TEXT_LEN], cur[MAC_TEXT_LEN];
    mac_text(custom ? mac : factory, eff, sizeof(eff));
    mac_runtime(is_ap ? WIFI_IF_AP : WIFI_IF_STA, factory, cur, sizeof(cur));

    resp_ok();
    gw_jsonw_str(&sys_w, "IF", is_sta ? "STA" : "AP");
    gw_jsonw_str(&sys_w, "MAC", eff);
    gw_jsonw_str(&sys_w, "CURRENT", cur);
    gw_jsonw_bool(&sys_w, "FACTORY", !custom);
    /* Действующий адрес меняется только при следующей загрузке драйвера. */
    gw_jsonw_bool(&sys_w, "RESTART", mac_pending(app));
}

/* ── sys.info: device status ────────────────────────────────────────────────── */

static void handle_info(const sys_service_t *svc) {
    app_context_t *app = svc->app;
    char mac_ap[18], mac_sta[18], ip_sta[16], ip_ap[16];
    uint8_t clients = wifi_ap_clients_count();

    /* MAC — у ДРАЙВЕРА, а не из eFuse: esp_read_mac() отдаёт заводской адрес даже
     * когда интерфейсу назначен другой (esp_wifi_set_mac его не меняет), и тогда
     * страница показывала бы не тот адрес, которым прибор работает в эфире. */
    mac_runtime(WIFI_IF_STA, app->mac_sta_factory, mac_sta, sizeof(mac_sta));
    mac_runtime(WIFI_IF_AP,  app->mac_ap_factory,  mac_ap,  sizeof(mac_ap));
    netif_ip_str("WIFI_STA_DEF", ip_sta, sizeof(ip_sta));
    netif_ip_str("WIFI_AP_DEF", ip_ap, sizeof(ip_ap));

    size_t fs_total = 0, fs_used = 0;
    esp_littlefs_info(conf.partition_label, &fs_total, &fs_used);

    resp_ok();

    gw_jsonw_str(&sys_w, "MAC_AP", mac_ap);
    gw_jsonw_str(&sys_w, "MAC_STA", mac_sta);
    gw_jsonw_str(&sys_w, "AP_SSID", (char *)app->ap_cnf.ap.ssid);
    gw_jsonw_str(&sys_w, "AP_IP", ip_ap);
    gw_jsonw_int(&sys_w, "AP_CLIENTS", clients);
    gw_jsonw_bool(&sys_w, "FROM_AP", svc->from_ap);
    /* The page uses this to enable configuration controls */
    gw_jsonw_bool(&sys_w, "CAN_CONFIGURE", change_allowed(svc, clients));
    gw_jsonw_str(&sys_w, "STA_SSID", (char *)app->sta_cnf.sta.ssid);
    gw_jsonw_str(&sys_w, "STA_IP", ip_sta);
    gw_jsonw_bool(&sys_w, "STA_CONNECTED", ip_sta[0] != '\0');
    /* К КАКОЙ точке подключены: имя сети у двух точек одно, различает их BSSID.
     * Страница показывает это рядом с состоянием, чтобы выбор был проверяем. */
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        char ap_bssid[MAC_TEXT_LEN];
        mac_text(ap_info.bssid, ap_bssid, sizeof(ap_bssid));
        gw_jsonw_str(&sys_w, "STA_BSSID", ap_bssid);
        gw_jsonw_int(&sys_w, "STA_RSSI", ap_info.rssi);
        gw_jsonw_int(&sys_w, "STA_CH", ap_info.primary);
    }
    char pin_now[MAC_TEXT_LEN];
    mac_text(app->sta_bssid, pin_now, sizeof(pin_now));
    gw_jsonw_str(&sys_w, "STA_PIN", wifi_mac_valid(app->sta_bssid) ? pin_now : "");
    gw_jsonw_int(&sys_w, "UPTIME", (int64_t)(esp_timer_get_time() / 1000000));
    gw_jsonw_uint(&sys_w, "HEAP", (uint64_t)esp_get_free_heap_size());
    gw_jsonw_uint(&sys_w, "FS_TOTAL", (uint64_t)fs_total);
    gw_jsonw_uint(&sys_w, "FS_USED", (uint64_t)fs_used);

    /* Firmware identity and OTA slots, so the page can show what is running and
     * what an upload would replace */
    const esp_app_desc_t *desc = esp_app_get_description();
    if (desc) {
        gw_jsonw_str(&sys_w, "FW_NAME", desc->project_name);
        gw_jsonw_str(&sys_w, "FW_VERSION", desc->version);
        gw_jsonw_str(&sys_w, "FW_DATE", desc->date);
        gw_jsonw_str(&sys_w, "FW_IDF", desc->idf_ver);
    }
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    gw_jsonw_str(&sys_w, "OTA_RUNNING", running ? running->label : "");
    gw_jsonw_str(&sys_w, "OTA_NEXT", next ? next->label : "");
    gw_jsonw_uint(&sys_w, "OTA_NEXT_SIZE", next ? (uint64_t)next->size : 0);
}

/* ── sys.reboot ───────────────────────────────────────────────────────── */

static void vRebootTask(void *arg) {
    vTaskDelay(pdMS_TO_TICKS((uint32_t)(uintptr_t)arg));
    ESP_LOGW(TAG, "Rebooting on client request");
    esp_restart();
}

/* 1 — перезагрузка разрешена этому клиенту (ответ уже собран), 0 — отказано. */
static uint8_t handle_reboot(const sys_service_t *svc) {
    /* Перезагрузку разрешает экземпляр сервиса (регистрация на интерфейс): у
     * точки доступа и у «любого интерфейса» она разрешена — как было и раньше
     * (см. sys_svc_mut). Условие про единственного клиента к ней не относится:
     * перезагрузка настройки не меняет. */
    if (!svc->may_change) {
        ESP_LOGW(TAG, "Reboot refused (from_ap=%u)", svc->from_ap);
        resp_denied(svc, wifi_ap_clients_count());
        return 0;
    }
    resp_ok();
    gw_jsonw_int(&sys_w, "DELAY_MS", REBOOT_DELAY_MS);
    return 1;
}

/* ── sys.files: filesystem listing ───────────────────────────────────────────── */

/* Путь внутри файловой системы ("/maps/x.jsonc") и он же с базовым префиксом
 * монтирования ("/lfs/maps/x.jsonc"): и листингу, и записи нужны оба. Разбор
 * клиентского пути — общий для всех файловых сервисов, он в fs_path.c: своей,
 * более мягкой проверки здесь быть не должно. */
#define FS_PATH_MAX 256
#define FS_FULL_MAX (FS_PATH_MAX + 64)

/* Путь целиком в смонтированной ФС: base_path + нормализованный путь. */
static uint8_t fs_full_path(const char *path, char *out, size_t out_size) {
    int written = snprintf(out, out_size, "%s%s", conf.base_path, path);
    return (written > 0) && (written < (int)out_size);
}

/* Проверить путь из ARG и собрать отказ в ответ, если он недопустим. field —
 * имя поля запроса (PATH, FROM, TO): по нему страница и человек понимают, что
 * именно не понравилось. */
static uint8_t fs_path_check(const char *requested, const char *field,
                             char *path, size_t path_size) {
    if (!requested || !fs_path_sanitize(requested, path, path_size) ||
        strcmp(path, "/") == 0) {
        resp_error(SYS_BAD_ARG, field,
                   "An absolute path inside the filesystem is required, without '.' or '..', e.g. /maps/x.jsonc");
        return 0;
    }
    return 1;
}

/* Разбирает ARG: {"PATH": "/regmaps"} — каталог для листинга (по умолчанию корень) */
static void fs_arg_path(const uint8_t *data, uint32_t len, char *out, size_t out_size) {
    out[0] = '\0';
    GW_JSON_TOKENS(toks, SYS_ARG_MAX_TOKENS);
    gw_json_t json;
    if ((data && len) && gw_json_parse(&json, (const char *)data, len, toks, SYS_ARG_MAX_TOKENS))
        gw_json_str(&json, "PATH", out, out_size);
    if (out[0] == '\0')
        strlcpy(out, "/", out_size);
}

static void handle_files(const uint8_t *data, uint32_t len) {
    size_t fs_total = 0, fs_used = 0;
    esp_littlefs_info(conf.partition_label, &fs_total, &fs_used);

    char requested[FS_PATH_MAX], path[FS_PATH_MAX];
    fs_arg_path(data, len, requested, sizeof(requested));

    resp_ok();
    gw_jsonw_uint(&sys_w, "TOTAL", (uint64_t)fs_total);
    gw_jsonw_uint(&sys_w, "USED", (uint64_t)fs_used);

    if (!fs_path_sanitize(requested, path, sizeof(path))) {
        gw_jsonw_str(&sys_w, "ERR", "PATH");
        gw_jsonw_str(&sys_w, "STA", SYS_BAD_ARG);
        return;
    }
    gw_jsonw_str(&sys_w, "PATH", path);

    /* Каталог выше по дереву, чтобы страница могла нарисовать «наверх» */
    char parent[FS_PATH_MAX];
    strlcpy(parent, path, sizeof(parent));
    if (strcmp(parent, "/") != 0) {
        char *slash = strrchr(parent, '/');
        if (slash == parent)
            strlcpy(parent, "/", sizeof(parent));
        else if (slash)
            *slash = '\0';
        gw_jsonw_str(&sys_w, "PARENT", parent);
    }

    char full[FS_FULL_MAX];
    if (!fs_full_path(path, full, sizeof(full))) {
        gw_jsonw_str(&sys_w, "ERR", "PATH");
        return;
    }

    gw_jsonw_arr(&sys_w, "FILES");

    DIR *dir = opendir(full);
    if (!dir) {
        ESP_LOGW(TAG, "Cannot open %s", full);
        gw_jsonw_str(&sys_w, "ERR", "FS");
        gw_jsonw_str(&sys_w, "STA", SYS_BAD_ARG);
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.')
            continue;
        char item_path[600]; /* base path + каталог + '/' + NAME_MAX */
        int n = snprintf(item_path, sizeof(item_path), "%s/%s", full, entry->d_name);
        if (n < 0 || n >= (int)sizeof(item_path))
            continue;
        struct stat st;
        if (stat(item_path, &st) != 0)
            continue;

        /* Буфер не резиновый: как только он кончился, прекращаем перечисление
         * (раньше на этом месте заканчивалась память под очередной узел) */
        if (!gw_jsonw_ok(&sys_w))
            break;

        gw_jsonw_obj(&sys_w, NULL);
        gw_jsonw_str(&sys_w, "NAME", entry->d_name);
        gw_jsonw_int(&sys_w, "SIZE", (int64_t)st.st_size);
        gw_jsonw_bool(&sys_w, "DIR", S_ISDIR(st.st_mode) ? 1 : 0);
        gw_jsonw_end(&sys_w);
    }
    closedir(dir);
}

/* ── sys.file.write / sys.file.delete / sys.file.rename ────────────────────── *
 *
 * Файловый менеджер целиком переведён на WebSocket: HTTP-методы изменения файлов
 * (POST /upload/…, /delete/…, /rename/…) убраны вместе со своими обработчиками,
 * а их работу делают эти три сервиса. Отдача файлов по HTTP GET не тронута —
 * именно так браузер получает страницы и карты регистров. Листинг — sys.files.
 *
 * Файл в одно сообщение не влезает: транспорт держит кадр и собранное сообщение
 * в 16 КБ (WS_TRANSPORT_MESSAGE_MAX_LEN), а двоичные данные идут ещё и в base64
 * (4/3). Поэтому файл передаётся порциями: ARG
 * {"PATH":"/maps/x.jsonc","OFFSET":<сколько байт уже принято>,"DATA":"<base64>",
 * "LAST":0|1}. Порция с OFFSET 0 начинает запись ЗАНОВО, LAST 1 завершает приём;
 * OFFSET каждой следующей порции обязан совпасть с числом принятых байт — иначе
 * порция потерялась бы молча.
 *
 * Пишем не в цель, а во ВРЕМЕННЫЙ файл рядом с ней (".<имя>.part"; листинг имена
 * с точки не показывает) и только на LAST переименовываем его в цель.
 * Переименование заменяет файл целиком, поэтому оборванная передача не оставляет
 * полуфайл вместо рабочего, а прежний файл живёт до последней порции. Недописанный
 * полуфабрикат убирается, когда начинается следующая запись того же пути.
 *
 * Состояние приёма — одно на прибор: обработчики ядра API гоняет одна задача, по
 * одному вызову за такт (см. sys_resp_buf), а файловый менеджер открыт только из
 * точки доступа — как и раньше. Порция пишется синхронно в этой же задаче, но
 * порция маленькая (в кадр больше 12 КБ данных не влезет), поэтому задержка для
 * остальных вызовов — на время записи в flash.
 *
 * Порция, на которую рассчитан сервис, — около 6 КБ данных: это 8 КБ base64, и
 * запрос вместе с путём и обёрткой влезает в предел сообщения с запасом (страница
 * шлёт ровно столько, см. FILE_CHUNK в config_page.html). Больше тоже можно —
 * сколько влезет в кадр: своего предела здесь нет, его держит транспорт.
 */

/* Раскодируем порцию блоками по 4 символа base64: память под данные не зависит
 * от размера порции. */
#define FS_B64_BLOCK   512
/* Предел размера файла — тот же, что был у загрузки по HTTP: файловая система
 * одна на страницы, карты и настройки, забить её целиком нельзя. */
#define FS_FILE_MAX     (500 * 1024)
#define FS_FILE_MAX_STR "500 KB"

typedef struct {
    uint8_t  active;              /* 1 — файл открыт, приём идёт */
    char     path[FS_PATH_MAX];   /* нормализованный путь цели */
    char     tmp[FS_PATH_MAX];    /* временный файл рядом с целью */
    FILE    *fp;
    uint32_t written;             /* сколько байт уже принято */
} fs_upload_t;

static fs_upload_t fs_up;

/* Закончить приём и забыть состояние. unlink_tmp — убрать временный файл:
 * оборванная передача не должна оставлять его до следующей попытки. */
static void fs_upload_close(uint8_t unlink_tmp) {
    if (fs_up.fp) {
        fclose(fs_up.fp);
        fs_up.fp = NULL;
    }
    if (unlink_tmp && fs_up.active && fs_up.tmp[0]) {
        char full[FS_FULL_MAX];
        if (fs_full_path(fs_up.tmp, full, sizeof(full)))
            unlink(full);
    }
    fs_up.active  = 0;
    fs_up.written = 0;
    fs_up.path[0] = '\0';
    fs_up.tmp[0]  = '\0';
}

/* Начать приём в path (уже проверенный): временный файл создаётся заново. */
static uint8_t fs_upload_begin(const char *path) {
    char tmp[FS_PATH_MAX];
    char full[FS_FULL_MAX];

    if (!fs_path_temp(path, tmp, sizeof(tmp)) || !fs_full_path(tmp, full, sizeof(full)))
        return 0;

    FILE *fp = fopen(full, "wb");
    if (!fp) {
        ESP_LOGE(TAG, "Не удалось создать %s", full);
        return 0;
    }

    strlcpy(fs_up.path, path, sizeof(fs_up.path));
    strlcpy(fs_up.tmp, tmp, sizeof(fs_up.tmp));
    fs_up.fp      = fp;
    fs_up.written = 0;
    fs_up.active  = 1;
    return 1;
}

/* Дописать порцию base64 в открытый файл. 0 — данные не base64 или запись не
 * удалась (вызывающий убирает полуфабрикат). */
static uint8_t fs_upload_append(const char *b64, size_t b64_len) {
    uint8_t buf[FS_B64_BLOCK];
    size_t done = 0;

    while (done < b64_len) {
        size_t n = b64_len - done;
        if (n > sizeof(buf))
            n = sizeof(buf);
        n &= ~(size_t)3;                     /* блок base64 — 4 символа */
        if (n == 0)
            return 0;

        const uint8_t *src = (const uint8_t *)b64 + done;
        int32_t expect  = base64_decode_buffer_required(src, (uint32_t)n);
        int32_t decoded = base64_decode(buf, sizeof(buf), src, (uint32_t)n);
        /* decoded != expect означает мусор в середине строки: декодер
         * останавливается на первом '=' и молча отдал бы обрезанный файл. */
        if (expect < 0 || decoded != expect)
            return 0;
        if (decoded > 0 && fwrite(buf, 1, (size_t)decoded, fs_up.fp) != (size_t)decoded)
            return 0;

        fs_up.written += (uint32_t)decoded;
        done += n;
    }
    return 1;
}

/* Запись файла порциями: {"PATH":"/a.txt","OFFSET":0,"DATA":"<base64>","LAST":0}
 * Ответ: {"STA":"0x00000000","WRITTEN":<сколько байт принято всего>} */
static void handle_file_write(const uint8_t *data, uint32_t len) {
    char requested[FS_PATH_MAX];
    char path[FS_PATH_MAX];
    GW_JSON_TOKENS(toks, SYS_ARG_MAX_TOKENS);
    gw_json_t json;
    int32_t offset = 0, last = 0;
    const char *b64 = NULL;
    size_t b64_len = 0;

    if (!((data && len) && gw_json_parse(&json, (const char *)data, len, toks, SYS_ARG_MAX_TOKENS))) {
        resp_error(SYS_BAD_ARG, "ARG",
                   "ARG must be an object: {\"PATH\":\"/a.txt\",\"OFFSET\":0,\"DATA\":\"<base64>\",\"LAST\":1}");
        return;
    }
    if (!gw_json_str(&json, "PATH", requested, sizeof(requested))) {
        resp_error(SYS_BAD_ARG, "PATH", "PATH is required, e.g. /maps/x.jsonc");
        return;
    }
    if (!fs_path_check(requested, "PATH", path, sizeof(path)))
        return;
    if (!gw_json_int(&json, "OFFSET", &offset) || offset < 0 || (uint32_t)offset > FS_FILE_MAX) {
        resp_error(SYS_BAD_ARG, "OFFSET",
                   "OFFSET is the number of bytes already written; 0 starts a new file");
        return;
    }
    /* LAST в запросе может отсутствовать — это обычная порция */
    gw_json_int(&json, "LAST", &last);
    if (!gw_json_raw(&json, "DATA", &b64, &b64_len))
        b64_len = 0;                         /* DATA может быть пустой строкой */

    if ((b64_len & 3) != 0) {
        resp_error(SYS_BAD_ARG, "DATA", "DATA must be base64 without spaces or line breaks");
        return;
    }
    int32_t decoded_len = b64_len
                          ? base64_decode_buffer_required((const uint8_t *)b64, (uint32_t)b64_len)
                          : 0;
    if (decoded_len < 0) {
        resp_error(SYS_BAD_ARG, "DATA", "DATA must be base64 without spaces or line breaks");
        return;
    }

    if (offset == 0) {
        char dst_full[FS_FULL_MAX];
        struct stat st;
        /* Прежняя недописанная передача (любого пути) — прочь: порция с OFFSET 0
         * начинает файл заново. */
        fs_upload_close(1);
        /* Каталог файлом не заменится: отказываем до записи, а не на LAST */
        if (fs_full_path(path, dst_full, sizeof(dst_full)) &&
            stat(dst_full, &st) == 0 && S_ISDIR(st.st_mode)) {
            resp_error(SYS_BAD_ARG, "PATH", "PATH points to a directory");
            return;
        }
        if (!fs_upload_begin(path)) {
            resp_error(SYS_INTERNAL, "FS", "Failed to create the file");
            return;
        }
    } else if (!fs_up.active || strcmp(fs_up.path, path) != 0) {
        resp_error(SYS_BAD_ARG, "OFFSET", "No upload of this PATH in progress: start with OFFSET 0");
        return;
    } else if ((uint32_t)offset != fs_up.written) {
        resp_error(SYS_BAD_ARG, "OFFSET",
                   "OFFSET must equal the number of bytes the device has already accepted");
        return;
    }

    if (fs_up.written + (uint32_t)decoded_len > FS_FILE_MAX) {
        fs_upload_close(1);
        resp_error(SYS_BAD_ARG, "SIZE", "File must be no larger than " FS_FILE_MAX_STR);
        return;
    }

    if (b64_len && !fs_upload_append(b64, b64_len)) {
        fs_upload_close(1);                  /* цель не тронута: пишем во временный */
        resp_error(SYS_INTERNAL, "FS", "Failed to write the file");
        return;
    }

    if (!last) {
        resp_ok();
        gw_jsonw_uint(&sys_w, "WRITTEN", (uint64_t)fs_up.written);
        return;
    }

    /* LAST: временный файл встаёт на место цели. Существующий файл переименование
     * заменяет целиком — до этого места он не изменён ни на байт. */
    char tmp_full[FS_FULL_MAX], dst_full[FS_FULL_MAX];
    if (!fs_full_path(fs_up.tmp, tmp_full, sizeof(tmp_full)) ||
        !fs_full_path(fs_up.path, dst_full, sizeof(dst_full))) {
        fs_upload_close(1);
        resp_error(SYS_INTERNAL, "FS", "Failed to finish writing the file");
        return;
    }
    uint32_t written = fs_up.written;
    fclose(fs_up.fp);
    fs_up.fp = NULL;
    if (rename(tmp_full, dst_full) != 0) {
        ESP_LOGE(TAG, "Не удалось переименовать %s в %s", tmp_full, dst_full);
        fs_upload_close(1);
        resp_error(SYS_INTERNAL, "FS", "Failed to finish writing the file");
        return;
    }
    fs_upload_close(0);
    ESP_LOGI(TAG, "Файл %s записан (%lu байт)", dst_full, (unsigned long)written);
    resp_ok();
    gw_jsonw_uint(&sys_w, "WRITTEN", (uint64_t)written);
}

/* Удаление файла: ARG {"PATH":"/maps/x.jsonc"}
 * Ответ: {"STA":"0x00000000","PATH":"/maps/x.jsonc"} */
static void handle_file_delete(const uint8_t *data, uint32_t len) {
    char requested[FS_PATH_MAX];
    char path[FS_PATH_MAX];
    char full[FS_FULL_MAX];
    struct stat st;
    GW_JSON_TOKENS(toks, SYS_ARG_MAX_TOKENS);
    gw_json_t json;

    if (!((data && len) && gw_json_parse(&json, (const char *)data, len, toks, SYS_ARG_MAX_TOKENS) &&
          gw_json_str(&json, "PATH", requested, sizeof(requested)))) {
        resp_error(SYS_BAD_ARG, "PATH", "PATH is required, e.g. {\"PATH\":\"/maps/x.jsonc\"}");
        return;
    }
    if (!fs_path_check(requested, "PATH", path, sizeof(path)))
        return;
    if (!fs_full_path(path, full, sizeof(full))) {
        resp_error(SYS_BAD_ARG, "PATH", "PATH is too long");
        return;
    }
    if (stat(full, &st) != 0) {
        resp_error(SYS_BAD_ARG, "PATH", "File does not exist");
        return;
    }
    if (S_ISDIR(st.st_mode)) {
        resp_error(SYS_BAD_ARG, "PATH", "PATH points to a directory");
        return;
    }
    /* Незавершённая передача этого же файла уходит вместе с ним: иначе её
     * полуфабрикат остался бы и «дошёл» до файла следующей загрузкой. */
    if (fs_up.active && strcmp(fs_up.path, path) == 0)
        fs_upload_close(1);

    if (unlink(full) != 0) {
        ESP_LOGE(TAG, "Не удалось удалить %s", full);
        resp_error(SYS_INTERNAL, "FS", "Failed to delete the file");
        return;
    }
    ESP_LOGI(TAG, "Файл %s удалён", full);
    resp_ok();
    gw_jsonw_str(&sys_w, "PATH", path);
}

/* Переименование: ARG {"FROM":"/maps/x.jsonc","TO":"/maps/y.jsonc"}.
 * TO может быть и коротким именем — тогда файл остаётся в каталоге FROM (так
 * работал прежний HTTP-обработчик, и ручной запрос это переживёт).
 * Переименование на существующее имя — отказ, как было и в HTTP-обработчике.
 * Ответ: {"STA":"0x00000000","FROM":"...","TO":"..."} */
static void handle_file_rename(const uint8_t *data, uint32_t len) {
    char from_req[FS_PATH_MAX], to_req[FS_PATH_MAX], to_arg[FS_PATH_MAX];
    char from[FS_PATH_MAX], to[FS_PATH_MAX];
    char src[FS_FULL_MAX], dst[FS_FULL_MAX];
    struct stat st;
    GW_JSON_TOKENS(toks, SYS_ARG_MAX_TOKENS);
    gw_json_t json;

    if (!((data && len) && gw_json_parse(&json, (const char *)data, len, toks, SYS_ARG_MAX_TOKENS))) {
        resp_error(SYS_BAD_ARG, "ARG",
                   "ARG must be an object: {\"FROM\":\"/a.txt\",\"TO\":\"/b.txt\"}");
        return;
    }
    if (!gw_json_str(&json, "FROM", from_req, sizeof(from_req))) {
        resp_error(SYS_BAD_ARG, "FROM", "FROM is required, e.g. /maps/x.jsonc");
        return;
    }
    if (!fs_path_check(from_req, "FROM", from, sizeof(from)))
        return;
    if (!gw_json_str(&json, "TO", to_req, sizeof(to_req)) || to_req[0] == '\0') {
        resp_error(SYS_BAD_ARG, "TO", "TO is required, e.g. /maps/y.jsonc");
        return;
    }

    if (to_req[0] == '/') {
        strlcpy(to_arg, to_req, sizeof(to_arg));
    } else {
        /* Короткое имя: тот же каталог, что у FROM */
        const char *slash = strrchr(from, '/');
        size_t dir_len = slash ? (size_t)(slash - from) + 1 : 0;
        if (dir_len + strlen(to_req) + 1 > sizeof(to_arg)) {
            resp_error(SYS_BAD_ARG, "TO", "TO is too long");
            return;
        }
        memcpy(to_arg, from, dir_len);
        strlcpy(to_arg + dir_len, to_req, sizeof(to_arg) - dir_len);
    }
    if (!fs_path_check(to_arg, "TO", to, sizeof(to)))
        return;

    if (!fs_full_path(from, src, sizeof(src)) || !fs_full_path(to, dst, sizeof(dst))) {
        resp_error(SYS_BAD_ARG, "TO", "Path is too long");
        return;
    }
    if (stat(src, &st) != 0) {
        ESP_LOGW(TAG, "Переименование: нет исходного файла %s", src);
        resp_error(SYS_BAD_ARG, "FROM", "File does not exist");
        return;
    }
    if (stat(dst, &st) == 0) {
        ESP_LOGW(TAG, "Переименование: цель уже существует %s", dst);
        resp_error(SYS_BAD_ARG, "EXIST", "Target name already exists");
        return;
    }
    if (rename(src, dst) != 0) {
        ESP_LOGE(TAG, "Не удалось переименовать %s в %s", src, dst);
        resp_error(SYS_INTERNAL, "FS", "Failed to rename the file");
        return;
    }
    ESP_LOGI(TAG, "Файл %s переименован в %s", src, dst);
    resp_ok();
    gw_jsonw_str(&sys_w, "FROM", from);
    gw_jsonw_str(&sys_w, "TO", to);
}

/* ── sys.file.mkdir: создание каталога ─────────────────────────────────────── */

static void handle_file_mkdir(const uint8_t *data, uint32_t len) {
    char path_req[FS_PATH_MAX], path[FS_PATH_MAX];
    char full[FS_FULL_MAX];
    struct stat st;
    GW_JSON_TOKENS(toks, SYS_ARG_MAX_TOKENS);
    gw_json_t json;

    if (!((data && len) && gw_json_parse(&json, (const char *)data, len, toks, SYS_ARG_MAX_TOKENS))) {
        resp_error(SYS_BAD_ARG, "ARG", "ARG must be an object: {\"PATH\":\"/maps\"}");
        return;
    }
    if (!gw_json_str(&json, "PATH", path_req, sizeof(path_req))) {
        resp_error(SYS_BAD_ARG, "PATH", "PATH is required, e.g. /maps");
        return;
    }
    if (!fs_path_check(path_req, "PATH", path, sizeof(path)))
        return;
    if (path[0] == '/' && path[1] == '\0') {
        resp_error(SYS_BAD_ARG, "EXIST", "Root directory already exists");
        return;
    }
    if (!fs_full_path(path, full, sizeof(full))) {
        resp_error(SYS_BAD_ARG, "PATH", "Path is too long");
        return;
    }
    if (stat(full, &st) == 0) {
        resp_error(SYS_BAD_ARG, "EXIST", st.st_mode & S_IFDIR ? "Directory already exists"
                                                              : "A file with this name exists");
        return;
    }
    if (mkdir(full, 0777) != 0) {
        ESP_LOGE(TAG, "Не удалось создать каталог %s", full);
        resp_error(SYS_INTERNAL, "FS", "Failed to create the directory");
        return;
    }
    ESP_LOGI(TAG, "Каталог %s создан", path);
    resp_ok();
    gw_jsonw_str(&sys_w, "PATH", path);
}

/* ── sys.file.rmdir: удаление пустого каталога ─────────────────────────────── */

static void handle_file_rmdir(const uint8_t *data, uint32_t len) {
    char path_req[FS_PATH_MAX], path[FS_PATH_MAX];
    char full[FS_FULL_MAX];
    struct stat st;
    GW_JSON_TOKENS(toks, SYS_ARG_MAX_TOKENS);
    gw_json_t json;

    if (!((data && len) && gw_json_parse(&json, (const char *)data, len, toks, SYS_ARG_MAX_TOKENS))) {
        resp_error(SYS_BAD_ARG, "ARG", "ARG must be an object: {\"PATH\":\"/maps\"}");
        return;
    }
    if (!gw_json_str(&json, "PATH", path_req, sizeof(path_req))) {
        resp_error(SYS_BAD_ARG, "PATH", "PATH is required, e.g. /maps");
        return;
    }
    if (!fs_path_check(path_req, "PATH", path, sizeof(path)))
        return;
    if (path[0] == '/' && path[1] == '\0') {
        resp_error(SYS_BAD_ARG, "PATH", "The root directory cannot be removed");
        return;
    }
    if (!fs_full_path(path, full, sizeof(full))) {
        resp_error(SYS_BAD_ARG, "PATH", "Path is too long");
        return;
    }
    if (stat(full, &st) != 0 || !(st.st_mode & S_IFDIR)) {
        resp_error(SYS_BAD_ARG, "PATH", "Directory does not exist");
        return;
    }
    /* rmdir сам откажет, если каталог не пуст: так и не нужно отдельно считать
     * содержимое — файловая система это уже знает. */
    if (rmdir(full) != 0) {
        ESP_LOGW(TAG, "Не удалось удалить каталог %s (возможно, он не пуст)", full);
        resp_error(SYS_BAD_ARG, "NOTEMPTY", "Directory is not empty");
        return;
    }
    ESP_LOGI(TAG, "Каталог %s удалён", path);
    resp_ok();
    gw_jsonw_str(&sys_w, "PATH", path);
}

/* ── sys.net: interface IPv4 settings ──────────────────────────────────────── */

#define NET_APPLY_DELAY_MS 1000

static void net_cfg_to_json(const char *name, const net_iface_cnf_t *cfg) {
    char ip[16], mask[16], gw[16], dns1[16], dns2[16];

    net_format_ip4(cfg->ip, ip, sizeof(ip));
    net_format_ip4(cfg->mask, mask, sizeof(mask));
    net_format_ip4(cfg->gw, gw, sizeof(gw));
    net_format_ip4(cfg->dns1, dns1, sizeof(dns1));
    net_format_ip4(cfg->dns2, dns2, sizeof(dns2));

    gw_jsonw_obj(&sys_w, name);
    gw_jsonw_str(&sys_w, "MODE", (cfg->mode == NET_MODE_STATIC) ? "static" : "dhcp");
    gw_jsonw_str(&sys_w, "IP", ip);
    gw_jsonw_str(&sys_w, "MASK", mask);
    gw_jsonw_str(&sys_w, "GW", gw);
    gw_jsonw_str(&sys_w, "DNS1", dns1);
    gw_jsonw_str(&sys_w, "DNS2", dns2);
    gw_jsonw_end(&sys_w);
}

/* A netmask has to be a run of ones followed by a run of zeros */
static uint8_t net_mask_valid(uint32_t mask) {
    uint32_t host = ntohl(mask);
    if (host == 0 || host == 0xFFFFFFFFu)
        return 0;
    uint32_t inv = ~host;
    return (inv & (inv + 1)) == 0;
}

static void vNetApplyTask(void *arg) {
    vTaskDelay(pdMS_TO_TICKS(NET_APPLY_DELAY_MS));
    ESP_LOGW(TAG, "Applying network settings");
    net_apply_config((const app_context_t *)arg);
    vTaskDelete(NULL);
}

static void handle_net(const sys_service_t *svc, const uint8_t *data, uint32_t len) {
    app_context_t *app = svc->app;
    uint8_t clients = wifi_ap_clients_count();
    uint8_t allowed = change_allowed(svc, clients);

    GW_JSON_TOKENS(toks, SYS_ARG_MAX_TOKENS);
    gw_json_t json;
    char iface[8] = {0};
    uint8_t parsed = (data && len)
                     && gw_json_parse(&json, (const char *)data, len, toks, SYS_ARG_MAX_TOKENS);
    uint8_t write = parsed && gw_json_str(&json, "IF", iface, sizeof(iface));

    if (!write) {
        char ip_sta[16], ip_ap[16];
        netif_ip_str("WIFI_STA_DEF", ip_sta, sizeof(ip_sta));
        netif_ip_str("WIFI_AP_DEF", ip_ap, sizeof(ip_ap));

        resp_ok();
        net_cfg_to_json("STA_CFG", &app->net_sta);

        /* Nothing configured yet means the built-in default is in force — show
         * the address actually in use so the form can be edited as is. */
        net_iface_cnf_t ap_show = app->net_ap;
        if (ap_show.ip == 0) {
            esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
            esp_netif_ip_info_t ip_info;
            if (ap && esp_netif_get_ip_info(ap, &ip_info) == ESP_OK) {
                ap_show.ip   = ip_info.ip.addr;
                ap_show.mask = ip_info.netmask.addr;
                ap_show.gw   = ip_info.gw.addr;
            }
        }
        net_cfg_to_json("AP_CFG", &ap_show);
        gw_jsonw_str(&sys_w, "STA_ADDR", ip_sta);
        gw_jsonw_str(&sys_w, "AP_ADDR", ip_ap);
        gw_jsonw_bool(&sys_w, "CAN_CONFIGURE", allowed);
        gw_jsonw_int(&sys_w, "CLIENTS", clients);
        return;
    }

    if (!allowed) {
        ESP_LOGW(TAG, "Network settings refused (from_ap=%u clients=%u, may_change=%u)",
                 svc->from_ap, clients, svc->may_change);
        resp_denied(svc, clients);
        return;
    }

    uint8_t is_sta = (strcasecmp(iface, "STA") == 0);
    uint8_t is_ap  = (strcasecmp(iface, "AP") == 0);
    if (!is_sta && !is_ap) {
        resp_error(SYS_BAD_ARG, "IF", "IF must be STA or AP");
        return;
    }

    char mode[16] = {0}, s_ip[NW_ADDR_MAX] = {0}, s_mask[NW_ADDR_MAX] = {0};
    char s_gw[NW_ADDR_MAX] = {0}, s_dns1[NW_ADDR_MAX] = {0}, s_dns2[NW_ADDR_MAX] = {0};
    gw_json_str(&json, "MODE", mode, sizeof(mode));
    gw_json_str(&json, "IP", s_ip, sizeof(s_ip));
    gw_json_str(&json, "MASK", s_mask, sizeof(s_mask));
    gw_json_str(&json, "GW", s_gw, sizeof(s_gw));
    gw_json_str(&json, "DNS1", s_dns1, sizeof(s_dns1));
    gw_json_str(&json, "DNS2", s_dns2, sizeof(s_dns2));

    /* The access point always has a fixed address of its own; only the station
     * can be told to use DHCP. */
    uint8_t want_static = is_ap || (mode[0] != '\0' && strcasecmp(mode, "static") == 0);

    net_iface_cnf_t cfg = is_sta ? app->net_sta : app->net_ap;
    uint8_t bad = 0;

    if (!want_static) {
        cfg.mode = NET_MODE_DHCP;
        cfg.ip = cfg.mask = cfg.gw = cfg.dns1 = cfg.dns2 = 0;
    } else {
        uint32_t ip = 0, mask = 0, gw = 0, dns1 = 0, dns2 = 0;

        if (s_ip[0] == '\0' || net_parse_ip4(s_ip, &ip) || ip == 0) {
            resp_error(SYS_BAD_ARG, "IP", "A valid IPv4 address is required");
            bad = 1;
        } else if (s_mask[0] == '\0' || net_parse_ip4(s_mask, &mask) || !net_mask_valid(mask)) {
            resp_error(SYS_BAD_ARG, "MASK", "A contiguous netmask is required, e.g. 255.255.255.0");
            bad = 1;
        } else if (s_gw[0] && (net_parse_ip4(s_gw, &gw) || (gw & mask) != (ip & mask))) {
            resp_error(SYS_BAD_ARG, "GW", "The gateway must be inside the IP/mask subnet");
            bad = 1;
        } else if (s_dns1[0] && net_parse_ip4(s_dns1, &dns1)) {
            resp_error(SYS_BAD_ARG, "DNS1", "Invalid DNS address");
            bad = 1;
        } else if (s_dns2[0] && net_parse_ip4(s_dns2, &dns2)) {
            resp_error(SYS_BAD_ARG, "DNS2", "Invalid DNS address");
            bad = 1;
        }

        if (!bad) {
            cfg.mode = NET_MODE_STATIC;
            cfg.ip = ip;
            cfg.mask = mask;
            /* The access point is the gateway for its own clients */
            cfg.gw = is_ap ? ip : gw;
            cfg.dns1 = is_ap ? 0 : dns1;
            cfg.dns2 = is_ap ? 0 : dns2;
        }
    }

    if (bad) return;

    /* Сравниваем с прежними настройками: страница присылает адрес при каждом
     * сохранении карточки, в том числе когда правили только MAC или пароль.
     * Перезапускать из-за этого DHCP точки доступа (а он у неё останавливается
     * на время смены адреса) незачем. */
    const net_iface_cnf_t *cur = is_sta ? &app->net_sta : &app->net_ap;
    uint8_t changed = memcmp(cur, &cfg, sizeof(cfg)) != 0;

    if (is_sta) app->net_sta = cfg;
    else        app->net_ap  = cfg;

    resp_ok();
    gw_jsonw_str(&sys_w, "IF", is_sta ? "STA" : "AP");
    net_cfg_to_json(is_sta ? "STA_CFG" : "AP_CFG", &cfg);
    gw_jsonw_bool(&sys_w, "CHANGED", changed);

    if (!changed) {
        ESP_LOGI(TAG, "%s address unchanged — nothing to apply", is_sta ? "STA" : "AP");
        return;
    }

    net_save_config(app);

    if (is_ap) {
        /* Changing the access point address drops the very connection this answer
         * travels over, so apply it once the response has gone out. */
        gw_jsonw_bool(&sys_w, "RECONNECT", 1);
        if (xTaskCreate(vNetApplyTask, "net_apply", 2560, app, 5, NULL) != pdPASS)
            ESP_LOGE(TAG, "Failed to schedule network apply");
    } else {
        net_apply_config(app);
    }
}

/* ── wifi.scan: сети вокруг ────────────────────────────────────────────────── *
 *
 * Скан — вспомогательный инструмент: имя сети и пароль пользователь по-прежнему
 * вводит руками, таблица найденных сетей лишь избавляет от опечаток. Менять он
 * ничего не может, поэтому оказывается любому интерфейсу (см. регистрацию).
 *
 * Драйвер сканирует в фоне (esp_wifi_scan_start(..., false)), а признак
 * готовности ставит событие SCAN_DONE — его обрабатывает connection.c.
 * Обработчик вызывается и в такте работы, поэтому ожиданий и своих задач внутри
 * нет: вызов держится возвратом TRUE, пока скан идёт.
 */

/* Двадцать строк — это уже длинная таблица на странице, а записи драйвера
 * копируются в статический массив (своей памяти сервису не надо). */
#define SCAN_MAX_NETS   20
/* Активный скан 13 каналов занимает 1.5–3 с (по 120 мс на канал плюс
 * перестройка), запас — на занятый эфир. */
#define SCAN_TIMEOUT_MS 12000
/* Станция в момент подключения не даёт запустить скан: повторяем попытку, а не
 * просим клиента жать кнопку ещё раз. */
#define SCAN_RETRY_MS   500

static wifi_ap_record_t scan_recs[SCAN_MAX_NETS];

/* Метка «этот вызов ведём мы»: своей памяти сервису не нужно, а по метке
 * обработчик отличает первичный вызов от такта работы. */
static uint8_t scan_token;

static struct {
    uint8_t    starting;   /* скан ещё не запустился: ждём, пока станция отпустит радио */
    uint32_t   asked_ms;   /* когда клиент попросил скан — для поля MS в ответе */
    TickType_t deadline;   /* когда перестаём ждать драйвер */
    TickType_t retry_at;   /* когда снова пробовать запуск */
} scan_req;

/* Вид защиты — коротким словом: странице нужно показать, нужен ли пароль, а
 * числовой код драйвера ничего не говорит человеку. */
static const char *scan_auth_str(wifi_auth_mode_t mode) {
    switch (mode) {
    case WIFI_AUTH_OPEN:            return "open";
    case WIFI_AUTH_WEP:             return "wep";
    case WIFI_AUTH_WPA_PSK:         return "wpa";
    case WIFI_AUTH_WPA2_PSK:        return "wpa2";
    case WIFI_AUTH_WPA_WPA2_PSK:    return "wpa/wpa2";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "wpa2-ent";
    case WIFI_AUTH_WPA3_PSK:        return "wpa3";
    case WIFI_AUTH_WPA2_WPA3_PSK:   return "wpa2/wpa3";
    case WIFI_AUTH_WAPI_PSK:        return "wapi";
    default:                        return "?";
    }
}

/* Сильные сети — вверх: в таблице глазами ищут ту, к которой подключаются. */
static void scan_sort(wifi_ap_record_t *recs, uint16_t n) {
    for (uint16_t i = 1; i < n; i++) {
        wifi_ap_record_t key = recs[i];
        int16_t j = (int16_t)i - 1;
        while (j >= 0 && recs[j].rssi < key.rssi) {
            recs[j + 1] = recs[j];
            j--;
        }
        recs[j + 1] = key;
    }
}

/* Ответ собираем из того, что драйвер положил в свой список. Забрать его можно
 * ровно один раз — wifi_scan_take освобождает память драйвера. */
static void scan_respond(void) {
    uint16_t found = 0;
    uint16_t n = wifi_scan_take(scan_recs, SCAN_MAX_NETS, &found);
    scan_sort(scan_recs, n);

    resp_ok();
    gw_jsonw_int(&sys_w, "COUNT", found);
    gw_jsonw_int(&sys_w, "SHOWN", n);
    gw_jsonw_int(&sys_w, "MS", (int64_t)((uint32_t)(esp_timer_get_time() / 1000) - scan_req.asked_ms));
    gw_jsonw_arr(&sys_w, "NETS");
    for (uint16_t i = 0; i < n; i++) {
        /* Буфер ответа конечен: не влезло — дальше не пишем, SHOWN скажет,
         * сколько строк на самом деле в ответе. */
        if (!gw_jsonw_ok(&sys_w)) break;
        const wifi_ap_record_t *r = &scan_recs[i];
        char bssid[MAC_TEXT_LEN];
        mac_text(r->bssid, bssid, sizeof(bssid));

        gw_jsonw_obj(&sys_w, NULL);
        gw_jsonw_str(&sys_w, "SSID", (const char *)r->ssid);
        gw_jsonw_int(&sys_w, "RSSI", r->rssi);
        gw_jsonw_int(&sys_w, "CH", r->primary);
        gw_jsonw_str(&sys_w, "AUTH", scan_auth_str(r->authmode));
        gw_jsonw_str(&sys_w, "BSSID", bssid);
        gw_jsonw_end(&sys_w);
    }
    gw_jsonw_end(&sys_w);
}

/* ── Обработчики вызовов ─────────────────────────────────────────────────── *
 *
 * Контракт (web_api.h): обработчик не блокирующий, отвечает сам и возвращает 0 —
 * вызов завершён. call == NULL — принудительное завершение: освобождать нечего
 * (контекст — статический экземпляр сервиса), поэтому просто сообщаем, что
 * завершились. Ответ отправляется ровно один раз за вызов. */

/* Контекст регистрации: по нему обработчик понимает, через какой интерфейс его
 * вызвали. У методов без политики (echo, sys.files) контекста нет — NULL. */
static const sys_service_t *svc_of(void **context) {
    return (context != NULL) ? (const sys_service_t *)*context : NULL;
}

static uint8_t sys_echo_handler(void *call, void **context, uint8_t *data, uint32_t data_len) {
    (void)context;
    if (!call) return 0;
    /* Documented echo test: the argument comes back unchanged. Пустой запрос —
     * ответ "null", как и раньше, когда присылалось пустое дерево. */
    if (data && data_len) bApiCallSendJson(call, data, data_len);
    else                  bApiCallSendJson(call, (const uint8_t *)"null", 4);
    return 0;
}

static uint8_t sys_sta_handler(void *call, void **context, uint8_t *data, uint32_t data_len) {
    if (!call) return 0;
    const sys_service_t *svc = svc_of(context);
    if (!svc) return 0;
    resp_begin();
    handle_sta(svc, data, data_len);
    send_response(call);
    return 0;
}

static uint8_t sys_ap_handler(void *call, void **context, uint8_t *data, uint32_t data_len) {
    if (!call) return 0;
    const sys_service_t *svc = svc_of(context);
    if (!svc) return 0;
    resp_begin();
    handle_ap(svc, data, data_len);
    send_response(call);
    return 0;
}

static uint8_t sys_info_handler(void *call, void **context, uint8_t *data, uint32_t data_len) {
    (void)data; (void)data_len;
    if (!call) return 0;
    const sys_service_t *svc = svc_of(context);
    if (!svc) return 0;
    resp_begin();
    handle_info(svc);
    send_response(call);
    return 0;
}

static uint8_t sys_reboot_handler(void *call, void **context, uint8_t *data, uint32_t data_len) {
    (void)data; (void)data_len;
    if (!call) return 0;
    const sys_service_t *svc = svc_of(context);
    if (!svc) return 0;
    resp_begin();
    uint8_t armed = handle_reboot(svc);
    /* Ответ уходит клиенту до перезагрузки: задача её откладывает, а отправка
     * асинхронна — соединение успевает получить конверт ответа. */
    send_response(call);
    if (armed && (xTaskCreate(vRebootTask, "reboot", 2560,
                              (void *)(uintptr_t)REBOOT_DELAY_MS, 5, NULL) != pdPASS))
        ESP_LOGE(TAG, "Failed to schedule reboot");
    return 0;
}

static uint8_t sys_files_handler(void *call, void **context, uint8_t *data, uint32_t data_len) {
    (void)context;
    if (!call) return 0;
    resp_begin();
    handle_files(data, data_len);
    send_response(call);
    return 0;
}

/* Файловые сервисы изменения — без политики на клиента: их, как и листинг,
 * оказывают только точке доступа (маска регистрации), а условия «сколько
 * клиентов на точке доступа» к правке файлов не относятся. */
static uint8_t sys_file_write_handler(void *call, void **context, uint8_t *data, uint32_t data_len) {
    (void)context;
    if (!call) return 0;
    resp_begin();
    handle_file_write(data, data_len);
    send_response(call);
    return 0;
}

static uint8_t sys_file_delete_handler(void *call, void **context, uint8_t *data, uint32_t data_len) {
    (void)context;
    if (!call) return 0;
    resp_begin();
    handle_file_delete(data, data_len);
    send_response(call);
    return 0;
}

static uint8_t sys_file_rename_handler(void *call, void **context, uint8_t *data, uint32_t data_len) {
    (void)context;
    if (!call) return 0;
    resp_begin();
    handle_file_rename(data, data_len);
    send_response(call);
    return 0;
}

static uint8_t sys_file_mkdir_handler(void *call, void **context, uint8_t *data, uint32_t data_len) {
    (void)context;
    if (!call) return 0;
    resp_begin();
    handle_file_mkdir(data, data_len);
    send_response(call);
    return 0;
}

static uint8_t sys_file_rmdir_handler(void *call, void **context, uint8_t *data, uint32_t data_len) {
    (void)context;
    if (!call) return 0;
    resp_begin();
    handle_file_rmdir(data, data_len);
    send_response(call);
    return 0;
}

static uint8_t sys_net_handler(void *call, void **context, uint8_t *data, uint32_t data_len) {
    if (!call) return 0;
    const sys_service_t *svc = svc_of(context);
    if (!svc) return 0;
    resp_begin();
    handle_net(svc, data, data_len);
    send_response(call);
    return 0;
}

static uint8_t sys_mac_handler(void *call, void **context, uint8_t *data, uint32_t data_len) {
    if (!call) return 0;
    const sys_service_t *svc = svc_of(context);
    if (!svc) return 0;
    resp_begin();
    handle_mac(svc, data, data_len);
    send_response(call);
    return 0;
}

/* Сервис сканирования — единственный здесь, кто живёт НЕСКОЛЬКО тактов: скан
 * идёт в драйвере, а вызов ждёт его окончания. Поэтому у него есть своё
 * состояние вызова, а не только контекст регистрации:
 *   data != NULL и метки нет — первичный вызов, запускаем скан;
 *   метка стоит              — такт работы: проверяем готовность и таймаут;
 *   call == NULL             — вызов сняли: гасим скан, читателя у результата
 *                              больше нет.
 * Ответ отправляется ровно один раз за вызов, а *context возвращается в
 * исходное состояние (у этой регистрации он пустой) перед завершением. */
static uint8_t sys_scan_handler(void *call, void **context, uint8_t *data, uint32_t data_len) {
    (void)data; (void)data_len;

    if (!call) {
        if (*context == &scan_token) {
            if (wifi_scan_busy()) wifi_scan_cancel();
            *context = NULL;
        }
        return 0;
    }

    if (*context != &scan_token) {
        /* Первичный вызов: чужой скан не перебиваем — клиенту честнее отказать. */
        if (wifi_scan_busy() || wifi_scan_ready()) {
            resp_begin();
            resp_error(SYS_BUSY, "BUSY", "Another scan is already running, try again in a second");
            send_response(call);
            return 0;
        }
        *context = &scan_token;
        scan_req.starting = 1;
        scan_req.asked_ms = (uint32_t)(esp_timer_get_time() / 1000);
        scan_req.deadline = xTaskGetTickCount() + pdMS_TO_TICKS(SCAN_TIMEOUT_MS);
        scan_req.retry_at = xTaskGetTickCount();
        ESP_LOGI(TAG, "wifi.scan: запрос принят");
    }

    TickType_t now = xTaskGetTickCount();

    if (scan_req.starting && (int32_t)(now - scan_req.retry_at) >= 0) {
        if (wifi_scan_start())
            scan_req.starting = 0;
        else
            scan_req.retry_at = now + pdMS_TO_TICKS(SCAN_RETRY_MS);
    }

    if (!scan_req.starting && wifi_scan_ready()) {
        resp_begin();
        scan_respond();
        *context = NULL;
        send_response(call);
        return 0;
    }

    if ((int32_t)(now - scan_req.deadline) >= 0) {
        ESP_LOGW(TAG, "wifi.scan: драйвер не ответил за %d мс", SCAN_TIMEOUT_MS);
        if (wifi_scan_busy()) wifi_scan_cancel();
        resp_begin();
        resp_error(SYS_TIMEOUT, "TIMEOUT", "The scan did not finish in time");
        *context = NULL;
        send_response(call);
        return 0;
    }

    return 1;
}

/* ── Регистрация сервисов ────────────────────────────────────────────────── */

/* Регистрация экземпляра сервиса: что за метод, каким интерфейсам он оказан и с
 * каким контекстом. Один FID регистрируется по разу на интерфейс — этим и
 * выражена политика (см. sys_service_t). */
static uint8_t register_service(api_handler_t handler, const char *fid,
                                uint32_t iface_mask, void *context) {
    if (bApiCallRegister(handler, fid, iface_mask, context)) return 1;
    ESP_LOGE(TAG, "Не удалось зарегистрировать %s (маска интерфейсов 0x%08lx)", fid,
             (unsigned long)iface_mask);
    return 0;
}

esp_err_t api_sys_run(app_context_t *app) {
    /* Сборщик ответов должен знать свой буфер до первого ответа: без этого он
     * считает себя переполненным и на КАЖДЫЙ запрос отвечает отказом
     * (проверено: страница конфигурации оставалась с прочерками). */
    gw_jsonw_init(&sys_w, sys_resp_buf, sizeof(sys_resp_buf));

    /* Один и тот же контекст описывает «кто спрашивает»: приложение, интерфейс
     * (из точки доступа или нет) и что этому клиенту можно. */
    sys_svc_ap.app      = app; sys_svc_ap.from_ap      = 1; sys_svc_ap.may_change      = 1;
    sys_svc_ro.app      = app; sys_svc_ro.from_ap      = 0; sys_svc_ro.may_change      = 0;
    sys_svc_mut.app     = app; sys_svc_mut.from_ap     = 0; sys_svc_mut.may_change     = 1;

    uint8_t ok = 1;
    const uint32_t ap_mask = GW_IFACE_MASK(GW_IFACE_AP);

    /* Методы, которые МЕНЯЮТ настройки: две регистрации одного обработчика —
     * для точки доступа (запись разрешена) и для любого другого интерфейса
     * (только чтение). Точная маска выигрывает у «любой», поэтому клиент точки
     * доступа получает первый экземпляр, а станционный — второй. */
    ok &= register_service(sys_sta_handler,  ESP_WS_API_SYS_WIFI_STA, ap_mask, &sys_svc_ap);
    ok &= register_service(sys_sta_handler,  ESP_WS_API_SYS_WIFI_STA, GW_IFACE_MASK_ANY, &sys_svc_ro);
    ok &= register_service(sys_ap_handler,   ESP_WS_API_SYS_WIFI_AP,  ap_mask, &sys_svc_ap);
    ok &= register_service(sys_ap_handler,   ESP_WS_API_SYS_WIFI_AP,  GW_IFACE_MASK_ANY, &sys_svc_ro);
    ok &= register_service(sys_net_handler,  ESP_WS_API_SYS_NET,      ap_mask, &sys_svc_ap);
    ok &= register_service(sys_net_handler,  ESP_WS_API_SYS_NET,      GW_IFACE_MASK_ANY, &sys_svc_ro);
    ok &= register_service(sys_mac_handler,  ESP_WS_API_SYS_WIFI_MAC, ap_mask, &sys_svc_ap);
    ok &= register_service(sys_mac_handler,  ESP_WS_API_SYS_WIFI_MAC, GW_IFACE_MASK_ANY, &sys_svc_ro);

    /* Скан эфира — чтение и вспомогательный инструмент: ни настроек, ни паролей
     * он не трогает, поэтому оказывается любому интерфейсу. */
    ok &= register_service(sys_scan_handler, ESP_WS_API_SYS_WIFI_SCAN, GW_IFACE_MASK_ANY, NULL);

    /* sys.info — чтение, но страница по нему решает, можно ли ей показывать
     * кнопки записи: FROM_AP и CAN_CONFIGURE берутся из контекста регистрации. */
    ok &= register_service(sys_info_handler, ESP_WS_API_SYS_INFO,     ap_mask, &sys_svc_ap);
    ok &= register_service(sys_info_handler, ESP_WS_API_SYS_INFO,     GW_IFACE_MASK_ANY, &sys_svc_ro);

    /* Перезагрузка: интерфейс различается регистрацией, но разрешена с любого —
     * как было снаружи (см. sys_svc_mut). */
    ok &= register_service(sys_reboot_handler, ESP_WS_API_SYS_REBOOT, ap_mask, &sys_svc_ap);
    ok &= register_service(sys_reboot_handler, ESP_WS_API_SYS_REBOOT, GW_IFACE_MASK_ANY, &sys_svc_mut);

    /* Методы без политики: политики у них нет, поэтому и контекста нет — один
     * экземпляр на все интерфейсы. */
    ok &= register_service(sys_echo_handler,  ESP_WS_API_ECHO_ID,     GW_IFACE_MASK_ANY, NULL);

    /* Файловый менеджер — листинг, запись порциями, удаление, переименование:
     * доступен с ЛЮБОГО интерфейса, как и отдача файлов по HTTP. Открыт им не
     * из-за невнимательности: страницы, карты регистров и прошивку кладут и
     * правят как раз с рабочего места в сети, а не только с самой точки доступа
     * (раньше это была пара HTTP-методов /upload/…, /delete/…, /rename/… —
     * теперь те же действия идут вызовами по WebSocket). Перестройка самого
     * шлюза по-прежнему только у администратора точки доступа: wifi.sta/ap и
     * sys.net зарегистрированы с «только чтение» для остальных. */
    ok &= register_service(sys_files_handler,       ESP_WS_API_SYS_FILES,       GW_IFACE_MASK_ANY, NULL);
    ok &= register_service(sys_file_write_handler,  ESP_WS_API_SYS_FILE_WRITE,  GW_IFACE_MASK_ANY, NULL);
    ok &= register_service(sys_file_delete_handler, ESP_WS_API_SYS_FILE_DELETE, GW_IFACE_MASK_ANY, NULL);
    ok &= register_service(sys_file_rename_handler, ESP_WS_API_SYS_FILE_RENAME, GW_IFACE_MASK_ANY, NULL);
    ok &= register_service(sys_file_mkdir_handler,  ESP_WS_API_SYS_FILE_MKDIR,  GW_IFACE_MASK_ANY, NULL);
    ok &= register_service(sys_file_rmdir_handler,  ESP_WS_API_SYS_FILE_RMDIR,  GW_IFACE_MASK_ANY, NULL);

    if (!ok) return ESP_FAIL;

    ESP_LOGI(TAG, "System API ready (echo, wifi.sta, wifi.ap, wifi.mac, wifi.scan, "
                  "sys.info, sys.reboot, sys.files, sys.file.write/delete/rename, sys.net)");
    return ESP_OK;
}
