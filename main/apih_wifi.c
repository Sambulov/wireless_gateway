/* Wi-Fi как таковой: wifi.sta, wifi.ap, wifi.mac, wifi.scan. Разбор ARG,
 * применение настроек и сборка ответов — здесь; общая обвязка — apih_sys_common.c,
 * точка сборки — apih_sys.c.
 *
 * Политика выражена ТОЛЬКО регистрацией (apih_wifi_run): смена точки доступа,
 * станции и MAC — на обоих ядрах с разными контекстами (администратор на AP-ядре,
 * только чтение на STA-ядре), скан — без контекста на обоих, потому что он ничего
 * не меняет.
 */
#include <string.h>

#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_wifi.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app.h"
#include "connection.h"

#include "apih_sys_internal.h"

#define GW_LOG_MODULE GW_LOG_API
#include "gw_log.h"

#define SSID_MAX 31

#define PASS_MIN 8
#define PASS_MAX 63

/* Переподключение откладывается на полсекунды: сначала должен уйти ответ. */
#define STA_APPLY_DELAY_MS 500

static void sta_apply_task(void *arg) {
    app_context_t *app = (app_context_t *)arg;
    vTaskDelay(pdMS_TO_TICKS(STA_APPLY_DELAY_MS));
    LOGI(TAG, "Applying station credentials, reconnecting");
    /* пусто в привязке значит «любая точка с этим именем, сильнейшая» */
    net_wifi_sta_set_target(&app->sta_cnf, app->sta_bssid);
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
    uint8_t clients = net_wifi_clients_count();
    uint8_t allowed = config_allowed(svc, clients);

    /* Запрос без SSID/PASS (и пустой ARG) читает сохранённое — так страница
     * заполняет форму; пароль выдаётся только при тех же условиях, что и смена.
     * Поля читаются jsmn'ом сразу в свой буфер: дерева и его узлов нет. */
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
        /* BSSID необязателен: пусто (или его нет) значит «любая точка с этим
         * именем». */
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
         * ли она сейчас: если выбранной точки не было в эфире, устройство сняло
         * привязку, чтобы не остаться без связи. */
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

    /* Сменилось имя сети — сохранённая привязка относится к другой сети и смысла
     * не имеет. */
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
     * а это событие двигает радио: пока ответ не отправлен, клиент рискует его не
     * получить вовсе. Так же поступает sys.net с адресом точки доступа. */
    resp_ok();
    gw_jsonw_str(&sys_w, "SSID", (char *)app->sta_cnf.sta.ssid);
    char pin[MAC_TEXT_LEN];
    mac_text(app->sta_bssid, pin, sizeof(pin));
    gw_jsonw_str(&sys_w, "BSSID", wifi_mac_valid(app->sta_bssid) ? pin : "");
    gw_jsonw_bool(&sys_w, "BSSID_SET", wifi_mac_valid(app->sta_bssid));
    gw_jsonw_bool(&sys_w, "CHANGED", changed);

    if (!changed) {
        LOGI(TAG, "STA credentials unchanged — nothing to apply");
        return;
    }

    gw_jsonw_bool(&sys_w, "RECONNECT", 1);
    if (xTaskCreate(sta_apply_task, "sta_apply", 3072, app, 5, NULL) != pdPASS)
        ESP_LOGE(TAG, "Failed to schedule station apply");
    LOGI(TAG, "STA set to SSID '%s', reconnecting...", app->sta_cnf.sta.ssid);
}

/* Как и у станции: сначала ответ, потом применение. Запись настроек работающей
 * точки доступа её перезапускает и рассылает deauth, поэтому ответ, отправленный
 * после, до клиента уже не доходит. */
#define AP_APPLY_DELAY_MS 500

static void ap_apply_task(void *arg) {
    app_context_t *app = (app_context_t *)arg;
    vTaskDelay(pdMS_TO_TICKS(AP_APPLY_DELAY_MS));
    LOGI(TAG, "Applying access point credentials (its clients will be dropped)");
    esp_err_t err = esp_wifi_set_config(WIFI_IF_AP, &app->ap_cnf);
    if (err != ESP_OK)
        ESP_LOGE(TAG, "AP configuration failed: %s", esp_err_to_name(err));
    vTaskDelete(NULL);
}

static void handle_ap(const sys_service_t *svc, const uint8_t *data, uint32_t len) {
    app_context_t *app = svc->app;
    uint8_t clients = net_wifi_clients_count();
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

    /* Ответ уходит ДО применения: смена настроек перезапускает радио и выбивает
     * всех клиентов точки доступа, включая того, кто ждёт этот ответ. Ничего не
     * менялось — радио не трогаем совсем. */
    resp_ok();
    gw_jsonw_str(&sys_w, "SSID", (char *)app->ap_cnf.ap.ssid);
    gw_jsonw_int(&sys_w, "CLIENTS", clients);
    gw_jsonw_bool(&sys_w, "CHANGED", changed);

    if (!changed) {
        LOGI(TAG, "AP credentials unchanged — nothing to apply");
        return;
    }

    save_wifi_ap_config(ssid, pass);
    LOGI(TAG, "AP set to SSID '%s' (password %s): its clients will reconnect",
             app->ap_cnf.ap.ssid,
             app->ap_cnf.ap.password[0] ? "changed" : "cleared, network is open");

    gw_jsonw_bool(&sys_w, "RECONNECT", 1);
    if (xTaskCreate(ap_apply_task, "ap_apply", 3072, app, 5, NULL) != pdPASS)
        ESP_LOGE(TAG, "Failed to schedule access point apply");
}

/* ── wifi.mac: MAC адрес интерфейса ────────────────────────────────────────── *
 *
 * Драйвер принимает новый MAC только на ОСТАНОВЛЕННОМ интерфейсе, а интерфейс у нас
 * работает всегда: гасить радио на время правки — значит рвать ту самую связь, по
 * которой пришла команда. Поэтому адрес сохраняется в NVS и применяется при
 * следующей загрузке (net_wifi_init), а страница честно говорит про перезагрузку.
 *
 * Заводской адрес прочитан из eFuse при старте (load_config) и лежит в
 * app->mac_*_factory: после подмены адреса спросить его у eFuse уже не получится.
 */

static const uint8_t *mac_effective(const uint8_t *custom, const uint8_t *factory) {
    return wifi_mac_valid(custom) ? custom : factory;
}

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
    uint8_t clients = net_wifi_clients_count();

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

    /* Пустое поле или слово factory — «верни заводской», остальное — адрес */
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
    /* Адрес, совпавший с заводским, — это и есть заводской: так в NVS не появится
     * запись-двойник, а «личный MAC» останется честным признаком. */
    if (custom && memcmp(mac, factory, 6) == 0)
        custom = 0;

    /* Половины радио не могут носить один адрес — драйвер это отвергает: проверяем
     * результат применения, а не то, что прислали. */
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
     * перезагрузку устройство НЕ делает само — адрес ждёт её. */
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

/* ── wifi.scan: сети вокруг ────────────────────────────────────────────────── *
 *
 * Скан ничего не меняет, поэтому есть на любом интерфейсе. Драйвер сканирует в фоне
 * (esp_wifi_scan_start(..., false)), готовность ставит событие SCAN_DONE
 * (net_scan.c); ожиданий и своих задач внутри нет — вызов держится возвратом TRUE.
 */

/* двадцать строк — уже длинная таблица на странице; записи драйвера копируются в
 * статический массив */
#define SCAN_MAX_NETS   20
/* активный скан 13 каналов занимает 1.5–3 с (120 мс на канал), запас — на занятый
 * эфир */
#define SCAN_TIMEOUT_MS 12000
/* станция в момент подключения не даёт запустить скан: повторяем попытку сами */
#define SCAN_RETRY_MS   500

static wifi_ap_record_t scan_recs[SCAN_MAX_NETS];

/* метка «этот вызов ведём мы»: отличает первичный вызов от такта работы */
static uint8_t scan_token;

static struct {
    uint8_t    starting;   /* скан ещё не запустился: ждём, пока станция отпустит радио */
    uint32_t   asked_ms;   /* когда клиент попросил скан — для поля MS в ответе */
    TickType_t deadline;
    TickType_t retry_at;
} scan_req;

/* вид защиты — коротким словом: числовой код драйвера человеку ничего не говорит */
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

/* сильные сети — вверх: в таблице глазами ищут ту, к которой подключаются */
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

/* забрать список у драйвера можно ровно один раз: net_scan_take освобождает его
 * память */
static void scan_respond(void) {
    uint16_t found = 0;
    uint16_t n = net_scan_take(scan_recs, SCAN_MAX_NETS, &found);
    scan_sort(scan_recs, n);

    resp_ok();
    gw_jsonw_int(&sys_w, "COUNT", found);
    gw_jsonw_int(&sys_w, "SHOWN", n);
    gw_jsonw_int(&sys_w, "MS", (int64_t)((uint32_t)(esp_timer_get_time() / 1000) - scan_req.asked_ms));
    gw_jsonw_arr(&sys_w, "NETS");
    for (uint16_t i = 0; i < n; i++) {
        /* буфер конечен: не влезло — дальше не пишем, SHOWN скажет, сколько строк
         * на самом деле в ответе */
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
 * Контракт (api.h): обработчик не блокирующий, отвечает сам и возвращает 0; call ==
 * NULL — вызов сняли. Ответ отправляется ровно один раз за вызов. */

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

static uint8_t sys_mac_handler(void *call, void **context, uint8_t *data, uint32_t data_len) {
    if (!call) return 0;
    const sys_service_t *svc = svc_of(context);
    if (!svc) return 0;
    resp_begin();
    handle_mac(svc, data, data_len);
    send_response(call);
    return 0;
}

/* Сервис сканирования — единственный здесь, кто живёт НЕСКОЛЬКО тактов: скан идёт
 * в драйвере, а вызов ждёт его окончания. Поэтому у него есть своё состояние вызова:
 *   data != NULL и метки нет — первичный вызов, запускаем скан;
 *   метка стоит              — такт работы: проверяем готовность и таймаут;
 *   call == NULL             — вызов сняли: гасим скан, читателя у результата нет.
 * *context перед завершением возвращается в исходное состояние (здесь он пустой) */
static uint8_t sys_scan_handler(void *call, void **context, uint8_t *data, uint32_t data_len) {
    (void)data; (void)data_len;

    if (!call) {
        if (*context == &scan_token) {
            if (net_scan_busy()) net_scan_cancel();
            *context = NULL;
        }
        return 0;
    }

    if (*context != &scan_token) {
        /* Первичный вызов: чужой обход не перебиваем — клиенту честнее отказать.
         * Готовый результат в кэше новому скану не мешает: клиент по API всегда
         * форсирует свежий обход, кэш — для системы. */
        if (net_scan_busy()) {
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
        LOGI(TAG, "wifi.scan: запрос принят");
    }

    TickType_t now = xTaskGetTickCount();

    if (scan_req.starting && (int32_t)(now - scan_req.retry_at) >= 0) {
        if (net_scan_start())
            scan_req.starting = 0;
        else
            scan_req.retry_at = now + pdMS_TO_TICKS(SCAN_RETRY_MS);
    }

    if (!scan_req.starting && net_scan_ready()) {
        resp_begin();
        scan_respond();
        *context = NULL;
        send_response(call);
        return 0;
    }

    if ((int32_t)(now - scan_req.deadline) >= 0) {
        ESP_LOGW(TAG, "wifi.scan: драйвер не ответил за %d мс", SCAN_TIMEOUT_MS);
        if (net_scan_busy()) net_scan_cancel();
        resp_begin();
        resp_error(SYS_TIMEOUT, "TIMEOUT", "The scan did not finish in time");
        *context = NULL;
        send_response(call);
        return 0;
    }

    return 1;
}

esp_err_t apih_wifi_run(app_context_t *app) {
    uint8_t ok = 1;
    api_t *ap  = app->api_ap;    /* ядро точки доступа: администратор */
    api_t *sta = app->api_sta;   /* ядро станции: только чтение */

    /* Методы, которые МЕНЯЮТ настройки: один и тот же обработчик регистрируется на
     * ОБОИХ ядрах с разным контекстом — на AP-ядре «администратор», на STA-ядре
     * «только чтение». */
    ok &= register_service(ap,  sys_sta_handler, ESP_WS_API_SYS_WIFI_STA, &sys_svc_ap);
    ok &= register_service(sta, sys_sta_handler, ESP_WS_API_SYS_WIFI_STA, &sys_svc_ro);
    ok &= register_service(ap,  sys_ap_handler,  ESP_WS_API_SYS_WIFI_AP,  &sys_svc_ap);
    ok &= register_service(sta, sys_ap_handler,  ESP_WS_API_SYS_WIFI_AP,  &sys_svc_ro);
    ok &= register_service(ap,  sys_mac_handler, ESP_WS_API_SYS_WIFI_MAC, &sys_svc_ap);
    ok &= register_service(sta, sys_mac_handler, ESP_WS_API_SYS_WIFI_MAC, &sys_svc_ro);

    /* Скан эфира — чтение: ни настроек, ни паролей он не трогает, поэтому есть на
     * обоих ядрах. */
    ok &= register_service(ap,  sys_scan_handler, ESP_WS_API_SYS_WIFI_SCAN, NULL);
    ok &= register_service(sta, sys_scan_handler, ESP_WS_API_SYS_WIFI_SCAN, NULL);

    return ok ? ESP_OK : ESP_FAIL;
}
