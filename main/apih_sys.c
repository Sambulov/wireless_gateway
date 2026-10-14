#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#include "app.h"
#include "uart.h"
#include "cJSON.h"
#include "cJSON_helpers.h"
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

/* System API (FIDs 0x0001, 0x0010..0x0014, see app.h).
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

static cJSON *make_answer(const char *sta) {
    cJSON *json = cJSON_CreateObject();
    if (json) cJSON_AddStringToObject(json, "STA", sta);
    return json;
}

/* A short machine-readable failure for the configuration page */
static cJSON *make_error(const char *sta, const char *err, const char *msg) {
    cJSON *json = make_answer(sta);
    if (json) {
        cJSON_AddStringToObject(json, "ERR", err);
        if (msg) cJSON_AddStringToObject(json, "MSG", msg);
    }
    return json;
}

/* Hands the answer to ws_server, which owns the buffer from here on */
static void send_response(uint32_t id, uint32_t fid, cJSON *json) {
    webapi_msg_t msg = {0};
    if (json) {
        char *txt = cJSON_PrintUnformatted(json);
        if (txt) {
            msg.data = (uint8_t *)txt;
            msg.len  = strlen(txt);
        }
    }
    msg.id  = id;
    msg.fid = fid;
    if (!ws_hal_queue_send(get_ws_worker_queue(), &msg, WS_HAL_WAIT_NONE))
        free(msg.data);
}

static void send_verbatim(uint32_t id, uint32_t fid, const uint8_t *data, uint32_t len) {
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
    msg.fid = fid;
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
static cJSON *access_denied(int fd, uint8_t clients) {
    cJSON *resp = request_from_softap(fd)
        ? make_error(SYS_NO_ACCESS, "AP_CLIENTS",
                     "Exactly one client must be connected to the softAP")
        : make_error(SYS_NO_ACCESS, "AP_ONLY",
                     "Wi-Fi and network settings can only be changed from the softAP");
    if (resp) cJSON_AddNumberToObject(resp, "CLIENTS", clients);
    return resp;
}

/* ── 0x0010 station (STA) credentials ────────────────────────────────────── */

static cJSON *handle_sta(app_context_t *app, const uint8_t *data, uint32_t len, int fd) {
    uint8_t clients = wifi_ap_clients_count();
    uint8_t allowed = config_allowed(fd, clients);

    /* A request without SSID/PASS — an empty ARG included — reads the stored
     * credentials, which is what the configuration page fills its form with.
     * The password itself is only disclosed under the same conditions that
     * allow changing it. */
    cJSON *req = (data && len) ? json_parse_with_length_opts((char *)data, len, 0, 0) : NULL;
    char *ssid = NULL, *pass = NULL;
    uint8_t write = (req != NULL)
                    && json_string_value(req, "SSID", &ssid)
                    && json_string_value(req, "PASS", &pass);

    if (!write) {
        if (req) cJSON_Delete(req);
        cJSON *resp = make_answer(SYS_OK);
        if (!resp) return NULL;
        cJSON_AddStringToObject(resp, "SSID", (char *)app->sta_cnf.sta.ssid);
        cJSON_AddStringToObject(resp, "PASS", allowed ? (char *)app->sta_cnf.sta.password : "");
        cJSON_AddBoolToObject(resp, "PASS_HIDDEN", !allowed);
        cJSON_AddBoolToObject(resp, "FROM_AP", request_from_softap(fd));
        cJSON_AddNumberToObject(resp, "CLIENTS", clients);
        return resp;
    }

    if (!allowed) {
        cJSON_Delete(req);
        ESP_LOGW(TAG, "STA credentials refused (from_ap=%u clients=%u)",
                 request_from_softap(fd), clients);
        return access_denied(fd, clients);
    }

    if (ssid[0] == '\0' || strlen(ssid) > SSID_MAX) {
        cJSON_Delete(req);
        return make_error(SYS_BAD_ARG, "SSID", "SSID must be 1..31 characters");
    }
    if (!wifi_password_valid(pass)) {
        cJSON_Delete(req);
        return make_error(SYS_BAD_ARG, "PASS", "Password must be 8..63 characters or empty");
    }

    strlcpy((char *)app->sta_cnf.sta.ssid, ssid, sizeof(app->sta_cnf.sta.ssid));
    strlcpy((char *)app->sta_cnf.sta.password, pass, sizeof(app->sta_cnf.sta.password));
    cJSON_Delete(req);

    save_wifi_sta_config((char *)app->sta_cnf.sta.ssid, (char *)app->sta_cnf.sta.password);

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &app->sta_cnf);
    if (err == ESP_OK) {
        esp_wifi_disconnect();
        err = esp_wifi_connect();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "STA configuration failed: %s", esp_err_to_name(err));
        return make_error(SYS_INTERNAL, "WIFI", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "STA set to SSID '%s', connecting...", app->sta_cnf.sta.ssid);
    cJSON *resp = make_answer(SYS_OK);
    if (resp) cJSON_AddStringToObject(resp, "SSID", (char *)app->sta_cnf.sta.ssid);
    return resp;
}

/* ── 0x0011 softAP credentials ───────────────────────────────────────────── */

static cJSON *handle_ap(app_context_t *app, const uint8_t *data, uint32_t len, int fd) {
    uint8_t clients = wifi_ap_clients_count();
    uint8_t allowed = config_allowed(fd, clients);

    cJSON *req = (data && len) ? json_parse_with_length_opts((char *)data, len, 0, 0) : NULL;
    char *ssid = NULL, *pass = NULL;
    uint8_t write = (req != NULL)
                    && json_string_value(req, "SSID", &ssid)
                    && json_string_value(req, "PASS", &pass);

    if (!write) {
        if (req) cJSON_Delete(req);
        cJSON *resp = make_answer(SYS_OK);
        if (!resp) return NULL;
        cJSON_AddStringToObject(resp, "SSID", (char *)app->ap_cnf.ap.ssid);
        cJSON_AddStringToObject(resp, "PASS", allowed ? (char *)app->ap_cnf.ap.password : "");
        cJSON_AddBoolToObject(resp, "PASS_HIDDEN", !allowed);
        cJSON_AddBoolToObject(resp, "FROM_AP", request_from_softap(fd));
        cJSON_AddNumberToObject(resp, "CLIENTS", clients);
        return resp;
    }

    if (!allowed) {
        cJSON_Delete(req);
        ESP_LOGW(TAG, "AP credentials refused (from_ap=%u clients=%u)",
                 request_from_softap(fd), clients);
        return access_denied(fd, clients);
    }

    if (ssid[0] == '\0' || strlen(ssid) > SSID_MAX) {
        cJSON_Delete(req);
        return make_error(SYS_BAD_ARG, "SSID", "SSID must be 1..31 characters");
    }
    if (!wifi_password_valid(pass)) {
        cJSON_Delete(req);
        return make_error(SYS_BAD_ARG, "PASS", "Password must be 8..63 characters or empty");
    }

    strlcpy((char *)app->ap_cnf.ap.ssid, ssid, sizeof(app->ap_cnf.ap.ssid));
    app->ap_cnf.ap.ssid_len = strlen(ssid);
    strlcpy((char *)app->ap_cnf.ap.password, pass, sizeof(app->ap_cnf.ap.password));
    app->ap_cnf.ap.authmode = (pass[0] != '\0') ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    cJSON_Delete(req);

    esp_err_t err = esp_wifi_set_config(WIFI_IF_AP, &app->ap_cnf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "AP configuration failed: %s", esp_err_to_name(err));
        return make_error(SYS_INTERNAL, "WIFI", esp_err_to_name(err));
    }

    save_wifi_ap_config((char *)app->ap_cnf.ap.ssid, (char *)app->ap_cnf.ap.password);
    ESP_LOGW(TAG, "AP set to SSID '%s' (password %s)", app->ap_cnf.ap.ssid,
             app->ap_cnf.ap.password[0] ? "changed" : "cleared, network is open");

    cJSON *resp = make_answer(SYS_OK);
    if (resp) {
        cJSON_AddStringToObject(resp, "SSID", (char *)app->ap_cnf.ap.ssid);
        cJSON_AddNumberToObject(resp, "CLIENTS", clients);
    }
    return resp;
}

/* ── 0x0012 device status ────────────────────────────────────────────────── */

static cJSON *handle_info(app_context_t *app, int fd) {
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

    cJSON *resp = make_answer(SYS_OK);
    if (!resp) return NULL;

    cJSON_AddStringToObject(resp, "MAC_AP", mac_ap);
    cJSON_AddStringToObject(resp, "MAC_STA", mac_sta);
    cJSON_AddStringToObject(resp, "AP_SSID", (char *)app->ap_cnf.ap.ssid);
    cJSON_AddStringToObject(resp, "AP_IP", ip_ap);
    cJSON_AddNumberToObject(resp, "AP_CLIENTS", clients);
    cJSON_AddBoolToObject(resp, "FROM_AP", from_ap);
    /* The page uses this to enable configuration controls */
    cJSON_AddBoolToObject(resp, "CAN_CONFIGURE", config_allowed(fd, clients));
    cJSON_AddStringToObject(resp, "STA_SSID", (char *)app->sta_cnf.sta.ssid);
    cJSON_AddStringToObject(resp, "STA_IP", ip_sta);
    cJSON_AddBoolToObject(resp, "STA_CONNECTED", ip_sta[0] != '\0');
    cJSON_AddNumberToObject(resp, "UPTIME", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddNumberToObject(resp, "HEAP", (double)esp_get_free_heap_size());
    cJSON_AddNumberToObject(resp, "FS_TOTAL", (double)fs_total);
    cJSON_AddNumberToObject(resp, "FS_USED", (double)fs_used);

    /* Firmware identity and OTA slots, so the page can show what is running and
     * what an upload would replace */
    const esp_app_desc_t *desc = esp_app_get_description();
    if (desc) {
        cJSON_AddStringToObject(resp, "FW_NAME", desc->project_name);
        cJSON_AddStringToObject(resp, "FW_VERSION", desc->version);
        cJSON_AddStringToObject(resp, "FW_DATE", desc->date);
        cJSON_AddStringToObject(resp, "FW_IDF", desc->idf_ver);
    }
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    cJSON_AddStringToObject(resp, "OTA_RUNNING", running ? running->label : "");
    cJSON_AddStringToObject(resp, "OTA_NEXT", next ? next->label : "");
    cJSON_AddNumberToObject(resp, "OTA_NEXT_SIZE", next ? (double)next->size : 0);
    return resp;
}

/* ── 0x0013 reboot ───────────────────────────────────────────────────────── */

static void vRebootTask(void *arg) {
    vTaskDelay(pdMS_TO_TICKS((uint32_t)(uintptr_t)arg));
    ESP_LOGW(TAG, "Rebooting on client request");
    esp_restart();
}

static cJSON *handle_reboot(void) {
    cJSON *resp = make_answer(SYS_OK);
    if (resp) cJSON_AddNumberToObject(resp, "DELAY_MS", REBOOT_DELAY_MS);
    if (xTaskCreate(vRebootTask, "reboot", 2560, (void *)(uintptr_t)REBOOT_DELAY_MS, 5, NULL) != pdPASS)
        ESP_LOGE(TAG, "Failed to schedule reboot");
    return resp;
}

/* ── 0x0014 filesystem listing ───────────────────────────────────────────── */

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
    cJSON *req = (data && len) ? json_parse_with_length_opts((char *)data, len, 0, 0) : NULL;
    if (req) {
        char *path = NULL;
        if (json_string_value(req, "PATH", &path) && path)
            strlcpy(out, path, out_size);
        cJSON_Delete(req);
    }
    if (out[0] == '\0')
        strlcpy(out, "/", out_size);
}

static cJSON *handle_files(const uint8_t *data, uint32_t len) {
    size_t fs_total = 0, fs_used = 0;
    esp_littlefs_info(conf.partition_label, &fs_total, &fs_used);

    char requested[256], path[256];
    fs_arg_path(data, len, requested, sizeof(requested));

    cJSON *resp = make_answer(SYS_OK);
    if (!resp) return NULL;
    cJSON_AddNumberToObject(resp, "TOTAL", (double)fs_total);
    cJSON_AddNumberToObject(resp, "USED", (double)fs_used);

    if (!fs_path_sanitize(requested, path, sizeof(path))) {
        cJSON_AddStringToObject(resp, "ERR", "PATH");
        cJSON_AddStringToObject(resp, "STA", SYS_BAD_ARG);
        return resp;
    }
    cJSON_AddStringToObject(resp, "PATH", path);

    /* Каталог выше по дереву, чтобы страница могла нарисовать «наверх» */
    char parent[256];
    strlcpy(parent, path, sizeof(parent));
    if (strcmp(parent, "/") != 0) {
        char *slash = strrchr(parent, '/');
        if (slash == parent)
            strlcpy(parent, "/", sizeof(parent));
        else if (slash)
            *slash = '\0';
        cJSON_AddStringToObject(resp, "PARENT", parent);
    }

    char full[400];
    int written = snprintf(full, sizeof(full), "%s%s", conf.base_path, path);
    if (written < 0 || written >= (int)sizeof(full)) {
        cJSON_AddStringToObject(resp, "ERR", "PATH");
        return resp;
    }

    cJSON *files = cJSON_AddArrayToObject(resp, "FILES");
    if (!files) return resp;

    DIR *dir = opendir(full);
    if (!dir) {
        ESP_LOGW(TAG, "Cannot open %s", full);
        cJSON_AddStringToObject(resp, "ERR", "FS");
        cJSON_AddStringToObject(resp, "STA", SYS_BAD_ARG);
        return resp;
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

        cJSON *item = cJSON_CreateObject();
        if (!item) break;
        cJSON_AddStringToObject(item, "NAME", entry->d_name);
        cJSON_AddNumberToObject(item, "SIZE", (double)st.st_size);
        cJSON_AddBoolToObject(item, "DIR", S_ISDIR(st.st_mode) ? 1 : 0);
        cJSON_AddItemToArray(files, item);
    }
    closedir(dir);
    return resp;
}

/* ── 0x0015 interface IPv4 settings ──────────────────────────────────────── */

#define NET_APPLY_DELAY_MS 1000

static void net_cfg_to_json(cJSON *parent, const char *name, const net_iface_cnf_t *cfg) {
    char ip[16], mask[16], gw[16], dns1[16], dns2[16];

    net_format_ip4(cfg->ip, ip, sizeof(ip));
    net_format_ip4(cfg->mask, mask, sizeof(mask));
    net_format_ip4(cfg->gw, gw, sizeof(gw));
    net_format_ip4(cfg->dns1, dns1, sizeof(dns1));
    net_format_ip4(cfg->dns2, dns2, sizeof(dns2));

    cJSON *o = cJSON_AddObjectToObject(parent, name);
    if (!o) return;
    cJSON_AddStringToObject(o, "MODE", (cfg->mode == NET_MODE_STATIC) ? "static" : "dhcp");
    cJSON_AddStringToObject(o, "IP", ip);
    cJSON_AddStringToObject(o, "MASK", mask);
    cJSON_AddStringToObject(o, "GW", gw);
    cJSON_AddStringToObject(o, "DNS1", dns1);
    cJSON_AddStringToObject(o, "DNS2", dns2);
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

static cJSON *handle_net(app_context_t *app, const uint8_t *data, uint32_t len, int fd) {
    uint8_t clients = wifi_ap_clients_count();
    uint8_t allowed = config_allowed(fd, clients);

    cJSON *req = (data && len) ? json_parse_with_length_opts((char *)data, len, 0, 0) : NULL;
    char *iface = NULL;
    uint8_t write = (req != NULL) && json_string_value(req, "IF", &iface);

    if (!write) {
        if (req) cJSON_Delete(req);
        char ip_sta[16], ip_ap[16];
        netif_ip_str("WIFI_STA_DEF", ip_sta, sizeof(ip_sta));
        netif_ip_str("WIFI_AP_DEF", ip_ap, sizeof(ip_ap));

        cJSON *resp = make_answer(SYS_OK);
        if (!resp) return NULL;
        net_cfg_to_json(resp, "STA_CFG", &app->net_sta);

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
        net_cfg_to_json(resp, "AP_CFG", &ap_show);
        cJSON_AddStringToObject(resp, "STA_ADDR", ip_sta);
        cJSON_AddStringToObject(resp, "AP_ADDR", ip_ap);
        cJSON_AddBoolToObject(resp, "CAN_CONFIGURE", allowed);
        cJSON_AddNumberToObject(resp, "CLIENTS", clients);
        return resp;
    }

    if (!allowed) {
        cJSON_Delete(req);
        ESP_LOGW(TAG, "Network settings refused (from_ap=%u clients=%u)",
                 request_from_softap(fd), clients);
        return access_denied(fd, clients);
    }

    uint8_t is_sta = (strcasecmp(iface, "STA") == 0);
    uint8_t is_ap  = (strcasecmp(iface, "AP") == 0);
    if (!is_sta && !is_ap) {
        cJSON_Delete(req);
        return make_error(SYS_BAD_ARG, "IF", "IF must be STA or AP");
    }

    char *mode = NULL, *s_ip = NULL, *s_mask = NULL, *s_gw = NULL, *s_dns1 = NULL, *s_dns2 = NULL;
    json_string_value(req, "MODE", &mode);
    json_string_value(req, "IP", &s_ip);
    json_string_value(req, "MASK", &s_mask);
    json_string_value(req, "GW", &s_gw);
    json_string_value(req, "DNS1", &s_dns1);
    json_string_value(req, "DNS2", &s_dns2);

    /* The access point always has a fixed address of its own; only the station
     * can be told to use DHCP. */
    uint8_t want_static = is_ap || (mode && strcasecmp(mode, "static") == 0);

    net_iface_cnf_t cfg = is_sta ? app->net_sta : app->net_ap;
    cJSON *err = NULL;

    if (!want_static) {
        cfg.mode = NET_MODE_DHCP;
        cfg.ip = cfg.mask = cfg.gw = cfg.dns1 = cfg.dns2 = 0;
    } else {
        uint32_t ip = 0, mask = 0, gw = 0, dns1 = 0, dns2 = 0;

        if (!s_ip || net_parse_ip4(s_ip, &ip) || ip == 0)
            err = make_error(SYS_BAD_ARG, "IP", "A valid IPv4 address is required");
        else if (!s_mask || net_parse_ip4(s_mask, &mask) || !net_mask_valid(mask))
            err = make_error(SYS_BAD_ARG, "MASK", "A contiguous netmask is required, e.g. 255.255.255.0");
        else if (s_gw && s_gw[0] && (net_parse_ip4(s_gw, &gw) || (gw & mask) != (ip & mask)))
            err = make_error(SYS_BAD_ARG, "GW", "The gateway must be inside the IP/mask subnet");
        else if (s_dns1 && s_dns1[0] && net_parse_ip4(s_dns1, &dns1))
            err = make_error(SYS_BAD_ARG, "DNS1", "Invalid DNS address");
        else if (s_dns2 && s_dns2[0] && net_parse_ip4(s_dns2, &dns2))
            err = make_error(SYS_BAD_ARG, "DNS2", "Invalid DNS address");

        if (!err) {
            cfg.mode = NET_MODE_STATIC;
            cfg.ip = ip;
            cfg.mask = mask;
            /* The access point is the gateway for its own clients */
            cfg.gw = is_ap ? ip : gw;
            cfg.dns1 = is_ap ? 0 : dns1;
            cfg.dns2 = is_ap ? 0 : dns2;
        }
    }

    cJSON_Delete(req);
    if (err) return err;

    if (is_sta) app->net_sta = cfg;
    else        app->net_ap  = cfg;
    net_save_config(app);

    cJSON *resp = make_answer(SYS_OK);
    if (resp) {
        cJSON_AddStringToObject(resp, "IF", is_sta ? "STA" : "AP");
        net_cfg_to_json(resp, is_sta ? "STA_CFG" : "AP_CFG", &cfg);
    }

    if (is_ap) {
        /* Changing the access point address drops the very connection this answer
         * travels over, so apply it once the response has gone out. */
        if (resp) cJSON_AddBoolToObject(resp, "RECONNECT", true);
        if (xTaskCreate(vNetApplyTask, "net_apply", 2560, app, 5, NULL) != pdPASS)
            ESP_LOGE(TAG, "Failed to schedule network apply");
    } else {
        net_apply_config(app);
    }
    return resp;
}

/* ── task ────────────────────────────────────────────────────────────────── */

static void handle_msg(app_context_t *app, webapi_msg_t *in_msg) {
    cJSON *resp = NULL;

    switch (in_msg->fid) {
    case ESP_WS_API_ECHO_ID:
        /* Documented echo test: the argument comes back unchanged */
        send_verbatim(in_msg->id, in_msg->fid, in_msg->data, in_msg->len);
        return;
    case ESP_WS_API_SYS_WIFI_STA:
        resp = handle_sta(app, in_msg->data, in_msg->len, in_msg->fd);
        break;
    case ESP_WS_API_SYS_WIFI_AP:
        resp = handle_ap(app, in_msg->data, in_msg->len, in_msg->fd);
        break;
    case ESP_WS_API_SYS_INFO:
        resp = handle_info(app, in_msg->fd);
        break;
    case ESP_WS_API_SYS_REBOOT:
        resp = handle_reboot();
        break;
    case ESP_WS_API_SYS_FILES:
        resp = handle_files(in_msg->data, in_msg->len);
        break;
    case ESP_WS_API_SYS_NET:
        resp = handle_net(app, in_msg->data, in_msg->len, in_msg->fd);
        break;
    default:
        ESP_LOGW(TAG, "No system handler for FID %d", in_msg->fid);
        return;
    }

    if (!resp)
        resp = make_error(SYS_NO_MEM, "NOMEM", NULL);
    if (resp) {
        send_response(in_msg->id, in_msg->fid, resp);
        cJSON_Delete(resp);
    }
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
    ESP_LOGI(TAG, "System API ready (FIDs 0x0001, 0x0010..0x0015)");
    return ESP_OK;
}
