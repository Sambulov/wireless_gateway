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
 * ws_server only forwards a call to its handler when the response comes back
 * from a peripheral queue, so every request-serving FID needs a queue of its
 * own: the request lands here as a webapi_msg_t, this task does the work and
 * posts the answer back into the worker queue, which the framework then
 * delivers to the client as {"FID":..,"SID":..,"ARG":<json>} and completes the
 * call automatically. Answers always carry a "STA" member, error codes
 * included — see web_api.h for the API_CALL_* values. */

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

static ws_queue_t sys_cmd_queue = NULL;

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
 * объект, ключ и значение прежний сборщик делал отдельным malloc, а потом весь
 * ответ печатался ещё одной аллокацией.
 *
 * Буфер статический, а не на стеке: задача обслуживает запросы по одному, а
 * стек у ws_sys всего 5 КБ, тогда как листинг каталога бывает в килобайты.
 * Единственная аллокация за ответ — копия готового текста: владение ею
 * переходит к каркасу, который её освободит. */
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

/* Hands the answer to ws_server, which owns the buffer from here on */
static void send_response(uint32_t id, const char *fid) {
    if (!gw_jsonw_ok(&sys_w)) {
        ESP_LOGE(TAG, "Ответ не влез в %d байт — отвечаю отказом", SYS_RESP_MAX);
        resp_begin();
        resp_error(SYS_NO_MEM, "NOMEM", NULL);
    }
    size_t len = gw_jsonw_finish(&sys_w);

    webapi_msg_t msg = {0};
    /* +1 и ноль на конце — как было у прежнего сборщика: каркас видит в ответе
     * C-строку (в частности, логирует её), а длину берёт отдельно. */
    msg.data = malloc(len + 1);
    if (msg.data) {
        memcpy(msg.data, sys_resp_buf, len);
        msg.data[len] = '\0';
        msg.len = len;
    }
    msg.id  = id;
    bApiFidSet(msg.fid, fid);
    if (!ws_hal_queue_send(get_ws_worker_queue(), &msg, WS_HAL_WAIT_NONE))
        free(msg.data);
}

static void send_verbatim(uint32_t id, const char *fid, const uint8_t *data, uint32_t len) {
    webapi_msg_t msg = {0};
    static const char null_json[] = "null";

    if (data && len) {
        msg.data = malloc(len);
        if (msg.data) {
            memcpy(msg.data, data, len);
            msg.len = len;
        }
    } else {
        msg.data = malloc(sizeof(null_json));
        if (msg.data) {
            memcpy(msg.data, null_json, sizeof(null_json));
            msg.len = sizeof(null_json) - 1;
        }
    }
    msg.id  = id;
    bApiFidSet(msg.fid, fid);
    if (!ws_hal_queue_send(get_ws_worker_queue(), &msg, WS_HAL_WAIT_NONE))
        free(msg.data);
}

/* ── access policy ───────────────────────────────────────────────────────── *
 * Wi-Fi, network and firmware settings can only be changed from the softAP side
 * and only while exactly one station is associated: that lone client is the
 * administrator. A client sitting in the station network cannot reconfigure the
 * gateway, and while two clients are on the AP neither of them can — somebody
 * else could be watching or racing the exchange. The same rule decides whether
 * the stored passwords are disclosed at all.
 */

static uint8_t config_allowed(int fd, uint8_t clients) {
    return request_from_softap(fd) && (clients == 1);
}

/* Refusal that tells the page what exactly is missing */
static void resp_denied(int fd, uint8_t clients) {
    if (request_from_softap(fd))
        resp_error(SYS_NO_ACCESS, "AP_CLIENTS",
                   "Exactly one client must be connected to the softAP");
    else
        resp_error(SYS_NO_ACCESS, "AP_ONLY",
                   "Wi-Fi and network settings can only be changed from the softAP");
    gw_jsonw_int(&sys_w, "CLIENTS", clients);
}

/* ── wifi.sta: station (STA) credentials ────────────────────────────────────── */

static void handle_sta(app_context_t *app, const uint8_t *data, uint32_t len, int fd) {
    uint8_t clients = wifi_ap_clients_count();
    uint8_t allowed = config_allowed(fd, clients);

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
        gw_jsonw_bool(&sys_w, "FROM_AP", request_from_softap(fd));
        gw_jsonw_int(&sys_w, "CLIENTS", clients);
        return;
    }

    if (!allowed) {
            ESP_LOGW(TAG, "STA credentials refused (from_ap=%u clients=%u)",
                 request_from_softap(fd), clients);
        resp_denied(fd, clients);
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

static void handle_ap(app_context_t *app, const uint8_t *data, uint32_t len, int fd) {
    uint8_t clients = wifi_ap_clients_count();
    uint8_t allowed = config_allowed(fd, clients);

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
        gw_jsonw_bool(&sys_w, "FROM_AP", request_from_softap(fd));
        gw_jsonw_int(&sys_w, "CLIENTS", clients);
        return;
    }

    if (!allowed) {
            ESP_LOGW(TAG, "AP credentials refused (from_ap=%u clients=%u)",
                 request_from_softap(fd), clients);
        resp_denied(fd, clients);
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

static void handle_info(app_context_t *app, int fd) {
    uint8_t mac[6] = {0};
    char mac_ap[18], mac_sta[18], ip_sta[16], ip_ap[16];
    uint8_t clients = wifi_ap_clients_count();
    uint8_t from_ap = request_from_softap(fd);

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
    gw_jsonw_bool(&sys_w, "FROM_AP", from_ap);
    /* The page uses this to enable configuration controls */
    gw_jsonw_bool(&sys_w, "CAN_CONFIGURE", config_allowed(fd, clients));
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

static void handle_reboot(void) {
    resp_ok();
    gw_jsonw_int(&sys_w, "DELAY_MS", REBOOT_DELAY_MS);
    if (xTaskCreate(vRebootTask, "reboot", 2560, (void *)(uintptr_t)REBOOT_DELAY_MS, 5, NULL) != pdPASS)
        ESP_LOGE(TAG, "Failed to schedule reboot");
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

static void handle_net(app_context_t *app, const uint8_t *data, uint32_t len, int fd) {
    uint8_t clients = wifi_ap_clients_count();
    uint8_t allowed = config_allowed(fd, clients);

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
            ESP_LOGW(TAG, "Network settings refused (from_ap=%u clients=%u)",
                 request_from_softap(fd), clients);
        resp_denied(fd, clients);
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

/* ── task ────────────────────────────────────────────────────────────────── */

/* Имя FID сравнивается целиком: "sys.info" и "sys.info2" — разные вызовы, и
 * префиксное совпадение здесь недопустимо (иначе чужой вызов попал бы в этот
 * обработчик). */
static void handle_msg(app_context_t *app, webapi_msg_t *in_msg) {
    const char *fid = in_msg->fid;

    if (strcmp(fid, ESP_WS_API_ECHO_ID) == 0) {
        /* Documented echo test: the argument comes back unchanged */
        send_verbatim(in_msg->id, fid, in_msg->data, in_msg->len);
        return;
    }

    resp_begin();
    if      (strcmp(fid, ESP_WS_API_SYS_WIFI_STA) == 0) handle_sta(app, in_msg->data, in_msg->len, in_msg->fd);
    else if (strcmp(fid, ESP_WS_API_SYS_WIFI_AP)  == 0) handle_ap(app, in_msg->data, in_msg->len, in_msg->fd);
    else if (strcmp(fid, ESP_WS_API_SYS_INFO)     == 0) handle_info(app, in_msg->fd);
    else if (strcmp(fid, ESP_WS_API_SYS_REBOOT)   == 0) handle_reboot();
    else if (strcmp(fid, ESP_WS_API_SYS_FILES)    == 0) handle_files(in_msg->data, in_msg->len);
    else if (strcmp(fid, ESP_WS_API_SYS_NET)      == 0) handle_net(app, in_msg->data, in_msg->len, in_msg->fd);
    else {
        ESP_LOGW(TAG, "No system handler for FID %s", fid);
        return;
    }

    send_response(in_msg->id, fid);
}

static void ws_sys_task(void *param) {
    webapi_msg_t *in_msg = NULL;
    app_context_t *app = (app_context_t *)param;

    for (;;) {
        if (ws_hal_queue_receive(sys_cmd_queue, &in_msg, WS_HAL_WAIT_FOREVER) != 0) {
            handle_msg(app, in_msg);
            free(in_msg->data);
            free(in_msg);
            in_msg = NULL;
        }
    }
}

esp_err_t api_sys_run(app_context_t *app) {
    /* Сборщик ответов должен знать свой буфер до первого ответа: без этого он
     * считает себя переполненным и на КАЖДЫЙ запрос отвечает отказом
     * (проверено: страница конфигурации оставалась с прочерками). */
    gw_jsonw_init(&sys_w, sys_resp_buf, sizeof(sys_resp_buf));

    sys_cmd_queue = ws_hal_queue_create(8, sizeof(void *));
    if (!sys_cmd_queue) {
        ESP_LOGE(TAG, "Cannot create system API queue");
        return ESP_FAIL;
    }

    ws_server_register_fid_queue(ESP_WS_API_ECHO_ID,      sys_cmd_queue);
    ws_server_register_fid_queue(ESP_WS_API_SYS_WIFI_STA, sys_cmd_queue);
    ws_server_register_fid_queue(ESP_WS_API_SYS_WIFI_AP,  sys_cmd_queue);
    ws_server_register_fid_queue(ESP_WS_API_SYS_INFO,     sys_cmd_queue);
    ws_server_register_fid_queue(ESP_WS_API_SYS_REBOOT,   sys_cmd_queue);
    ws_server_register_fid_queue(ESP_WS_API_SYS_FILES,    sys_cmd_queue);
    ws_server_register_fid_queue(ESP_WS_API_SYS_NET,      sys_cmd_queue);

    ws_hal_task_create(ws_sys_task, "ws_sys", 5120, app, 5);
    ESP_LOGI(TAG, "System API ready (echo, wifi.sta, wifi.ap, sys.info, sys.reboot, sys.files, sys.net)");
    return ESP_OK;
}
