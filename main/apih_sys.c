#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#include "app.h"
#include "uart.h"
#include "GwJson.h"
#include "web_api.h"
#include "ws_hal.h"

#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_littlefs.h"
#include "esp_wifi.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"

#include <lwip/sockets.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* System API (FID names echo, wifi.sta/ap, sys.info/reboot/files/net, see app.h).
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
    uint8_t parsed = (data && len)
                     && gw_json_parse(&json, (const char *)data, len, toks, SYS_ARG_MAX_TOKENS);
    uint8_t write = parsed
                    && gw_json_str(&json, "SSID", ssid, sizeof(ssid))
                    && gw_json_str(&json, "PASS", pass, sizeof(pass));

    if (!write) {
        resp_ok();
        gw_jsonw_str(&sys_w, "SSID", (char *)app->sta_cnf.sta.ssid);
        gw_jsonw_str(&sys_w, "PASS", allowed ? (char *)app->sta_cnf.sta.password : "");
        gw_jsonw_bool(&sys_w, "PASS_HIDDEN", !allowed);
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

    strlcpy((char *)app->sta_cnf.sta.ssid, ssid, sizeof(app->sta_cnf.sta.ssid));
    strlcpy((char *)app->sta_cnf.sta.password, pass, sizeof(app->sta_cnf.sta.password));

    save_wifi_sta_config((char *)app->sta_cnf.sta.ssid, (char *)app->sta_cnf.sta.password);

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &app->sta_cnf);
    if (err == ESP_OK) {
        esp_wifi_disconnect();
        err = esp_wifi_connect();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "STA configuration failed: %s", esp_err_to_name(err));
        resp_error(SYS_INTERNAL, "WIFI", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "STA set to SSID '%s', connecting...", app->sta_cnf.sta.ssid);
    resp_ok();
    gw_jsonw_str(&sys_w, "SSID", (char *)app->sta_cnf.sta.ssid);
}

/* ── wifi.ap: softAP credentials ───────────────────────────────────────────── */

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

    strlcpy((char *)app->ap_cnf.ap.ssid, ssid, sizeof(app->ap_cnf.ap.ssid));
    app->ap_cnf.ap.ssid_len = strlen(ssid);
    strlcpy((char *)app->ap_cnf.ap.password, pass, sizeof(app->ap_cnf.ap.password));
    app->ap_cnf.ap.authmode = (pass[0] != '\0') ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    esp_err_t err = esp_wifi_set_config(WIFI_IF_AP, &app->ap_cnf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "AP configuration failed: %s", esp_err_to_name(err));
        resp_error(SYS_INTERNAL, "WIFI", esp_err_to_name(err));
        return;
    }

    save_wifi_ap_config((char *)app->ap_cnf.ap.ssid, (char *)app->ap_cnf.ap.password);
    ESP_LOGW(TAG, "AP set to SSID '%s' (password %s)", app->ap_cnf.ap.ssid,
             app->ap_cnf.ap.password[0] ? "changed" : "cleared, network is open");

    resp_ok();
    gw_jsonw_str(&sys_w, "SSID", (char *)app->ap_cnf.ap.ssid);
    gw_jsonw_int(&sys_w, "CLIENTS", clients);
}

/* ── sys.info: device status ────────────────────────────────────────────────── */

static void handle_info(const sys_service_t *svc) {
    app_context_t *app = svc->app;
    uint8_t mac[6] = {0};
    char mac_ap[18], mac_sta[18], ip_sta[16], ip_ap[16];
    uint8_t clients = wifi_ap_clients_count();

    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(mac_ap, sizeof(mac_ap), MACSTR, MAC2STR(mac));
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(mac_sta, sizeof(mac_sta), MACSTR, MAC2STR(mac));
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

/* Приводит запрошенный путь к безопасному виду: только внутри файловой системы,
 * без "..", без двойных и хвостовых слэшей. Возвращает нормализованный путь
 * ("/" для корня) либо NULL, если путь недопустим. */
static const char *fs_path_sanitize(const char *in, char *out, size_t out_size) {
    if (!in || in[0] != '/')
        return NULL;
    size_t o = 0;
    const char *p = in;
    while (*p) {
        while (*p == '/') p++;                    /* схлопываем слэши */
        const char *seg = p;
        while (*p && *p != '/') p++;
        size_t len = (size_t)(p - seg);
        if (len == 0)
            break;
        if ((len == 1 && seg[0] == '.') ||
            (len == 2 && seg[0] == '.' && seg[1] == '.'))
            return NULL;                          /* выход за пределы корня */
        if (o + len + 2 >= out_size)
            return NULL;
        out[o++] = '/';
        memcpy(out + o, seg, len);
        o += len;
    }
    if (o == 0)
        out[o++] = '/';
    out[o] = '\0';
    return out;
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

    char requested[256], path[256];
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
    char parent[256];
    strlcpy(parent, path, sizeof(parent));
    if (strcmp(parent, "/") != 0) {
        char *slash = strrchr(parent, '/');
        if (slash == parent)
            strlcpy(parent, "/", sizeof(parent));
        else if (slash)
            *slash = '\0';
        gw_jsonw_str(&sys_w, "PARENT", parent);
    }

    char full[400];
    int written = snprintf(full, sizeof(full), "%s%s", conf.base_path, path);
    if (written < 0 || written >= (int)sizeof(full)) {
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

    if (is_sta) app->net_sta = cfg;
    else        app->net_ap  = cfg;
    net_save_config(app);

    resp_ok();
    gw_jsonw_str(&sys_w, "IF", is_sta ? "STA" : "AP");
    net_cfg_to_json(is_sta ? "STA_CFG" : "AP_CFG", &cfg);

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

static uint8_t sys_net_handler(void *call, void **context, uint8_t *data, uint32_t data_len) {
    if (!call) return 0;
    const sys_service_t *svc = svc_of(context);
    if (!svc) return 0;
    resp_begin();
    handle_net(svc, data, data_len);
    send_response(call);
    return 0;
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
    ok &= register_service(sys_files_handler, ESP_WS_API_SYS_FILES,   GW_IFACE_MASK_ANY, NULL);

    if (!ok) return ESP_FAIL;

    ESP_LOGI(TAG, "System API ready (echo, wifi.sta, wifi.ap, sys.info, sys.reboot, sys.files, sys.net)");
    return ESP_OK;
}
