/* Служба прибора: sys.info, sys.net, sys.reboot — состояние прибора, сетевые
 * адреса его интерфейсов и перезагрузка. Общая обвязка — apih_sys_common.c,
 * точка сборки — apih_sys.c.
 */
#include <stdio.h>
#include <string.h>

#include <lwip/sockets.h>

#include "esp_app_desc.h"
#include "esp_littlefs.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "esp_wifi.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app.h"

#include "apih_sys_internal.h"

#define GW_LOG_MODULE GW_LOG_API
#include "gw_log.h"

#define REBOOT_DELAY_MS 1000

/* Адрес в виде строки: "255.255.255.255" плюс запас. */
#define NW_ADDR_MAX 24

#define NET_APPLY_DELAY_MS 1000

/* ── helpers ─────────────────────────────────────────────────────────────── */

static void netif_ip_str(const char *ifkey, char *out, size_t out_size) {
    out[0] = '\0';
    esp_netif_t *nif = esp_netif_get_handle_from_ifkey(ifkey);
    if (!nif) return;
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(nif, &ip) != ESP_OK || ip.ip.addr == 0) return;
    snprintf(out, out_size, IPSTR, IP2STR(&ip.ip));
}

/* ── sys.info: device status ────────────────────────────────────────────────── */

static void handle_info(const sys_service_t *svc) {
    app_context_t *app = svc->app;
    char mac_ap[18], mac_sta[18], ip_sta[16], ip_ap[16];
    uint8_t clients = net_wifi_clients_count();

    /* MAC — у ДРАЙВЕРА, а не из eFuse: esp_read_mac() отдаёт заводской адрес даже
     * когда интерфейсу назначен другой (esp_wifi_set_mac его не меняет). */
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
    gw_jsonw_bool(&sys_w, "CAN_CONFIGURE", change_allowed(svc, clients));
    gw_jsonw_str(&sys_w, "STA_SSID", (char *)app->sta_cnf.sta.ssid);
    gw_jsonw_str(&sys_w, "STA_IP", ip_sta);
    gw_jsonw_bool(&sys_w, "STA_CONNECTED", ip_sta[0] != '\0');
    /* к какой точке подключены: имя сети у двух точек одно, различает их BSSID —
     * страница показывает это рядом с состоянием, чтобы выбор был проверяем. */
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

static void reboot_task(void *arg) {
    vTaskDelay(pdMS_TO_TICKS((uint32_t)(uintptr_t)arg));
    LOGI(TAG, "Rebooting on client request");
    esp_restart();
}

/* 1 — перезагрузка разрешена этому клиенту (ответ уже собран), 0 — отказано */
static uint8_t handle_reboot(const sys_service_t *svc) {
    /* перезагрузку разрешает экземпляр сервиса (регистрация на ядро): на STA-ядре
     * у неё свой контекст (см. sys_svc_mut). Условие про единственного клиента к
     * ней не относится — настройки она не меняет. */
    if (!svc->may_change) {
        ESP_LOGW(TAG, "Reboot refused (from_ap=%u)", svc->from_ap);
        resp_denied(svc, net_wifi_clients_count());
        return 0;
    }
    resp_ok();
    gw_jsonw_int(&sys_w, "DELAY_MS", REBOOT_DELAY_MS);
    return 1;
}

/* ── sys.net: interface IPv4 settings ──────────────────────────────────────── */

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

/* маска — непрерывная последовательность единиц, затем нулей */
static uint8_t net_mask_valid(uint32_t mask) {
    uint32_t host = ntohl(mask);
    if (host == 0 || host == 0xFFFFFFFFu)
        return 0;
    uint32_t inv = ~host;
    return (inv & (inv + 1)) == 0;
}

static void net_apply_task(void *arg) {
    vTaskDelay(pdMS_TO_TICKS(NET_APPLY_DELAY_MS));
    LOGI(TAG, "Applying network settings");
    net_apply_config((const app_context_t *)arg);
    vTaskDelete(NULL);
}

static void handle_net(const sys_service_t *svc, const uint8_t *data, uint32_t len) {
    app_context_t *app = svc->app;
    uint8_t clients = net_wifi_clients_count();
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

        /* Ничего не настроено — в силе встроенный адрес по умолчанию: показываем
         * действующий, чтобы форму можно было править как есть. */
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

    /* у точки доступа адрес всегда фиксированный; на DHCP переводится только
     * станция */
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
            /* точка доступа — шлюз для своих клиентов */
            cfg.gw = is_ap ? ip : gw;
            cfg.dns1 = is_ap ? 0 : dns1;
            cfg.dns2 = is_ap ? 0 : dns2;
        }
    }

    if (bad) return;

    /* Сравниваем с текущими настройками: страница присылает адрес при каждом
     * сохранении карточки, а перезапускать из-за этого DHCP точки доступа незачем. */
    const net_iface_cnf_t *cur = is_sta ? &app->net_sta : &app->net_ap;
    uint8_t changed = memcmp(cur, &cfg, sizeof(cfg)) != 0;

    if (is_sta) app->net_sta = cfg;
    else        app->net_ap  = cfg;

    resp_ok();
    gw_jsonw_str(&sys_w, "IF", is_sta ? "STA" : "AP");
    net_cfg_to_json(is_sta ? "STA_CFG" : "AP_CFG", &cfg);
    gw_jsonw_bool(&sys_w, "CHANGED", changed);

    if (!changed) {
        LOGI(TAG, "%s address unchanged — nothing to apply", is_sta ? "STA" : "AP");
        return;
    }

    net_save_config(app);

    if (is_ap) {
        /* Смена адреса точки доступа рвёт то самое соединение, по которому уйдёт
         * ответ, поэтому применяем его после отправки. */
        gw_jsonw_bool(&sys_w, "RECONNECT", 1);
        if (xTaskCreate(net_apply_task, "net_apply", 2560, app, 5, NULL) != pdPASS)
            ESP_LOGE(TAG, "Failed to schedule network apply");
    } else {
        net_apply_config(app);
    }
}

/* ── Обработчики вызовов ─────────────────────────────────────────────────── *
 *
 * Контракт (api.h): обработчик не блокирующий, отвечает сам и возвращает 0 —
 * вызов завершён; call == NULL — вызов сняли. Ответ отправляется ровно один раз. */

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
    /* ответ уходит до перезагрузки: задача её откладывает, а отправка асинхронна */
    send_response(call);
    if (armed && (xTaskCreate(reboot_task, "reboot", 2560,
                              (void *)(uintptr_t)REBOOT_DELAY_MS, 5, NULL) != pdPASS))
        ESP_LOGE(TAG, "Failed to schedule reboot");
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

/* ── Регистрация методов прибора ─────────────────────────────────────────── */

esp_err_t apih_device_run(app_context_t *app) {
    uint8_t ok = 1;
    api_t *ap  = app->api_ap;    /* ядро точки доступа: администратор */
    api_t *sta = app->api_sta;   /* ядро станции: только чтение */

    /* sys.net меняет настройки: на AP-ядре администратор, на STA-ядре только чтение */
    ok &= register_service(ap,  sys_net_handler, ESP_WS_API_SYS_NET, &sys_svc_ap);
    ok &= register_service(sta, sys_net_handler, ESP_WS_API_SYS_NET, &sys_svc_ro);

    /* sys.info — чтение, но по FROM_AP и CAN_CONFIGURE страница решает, показывать
     * ли кнопки записи: они берутся из контекста того ядра, куда пришёл вызов. */
    ok &= register_service(ap,  sys_info_handler, ESP_WS_API_SYS_INFO, &sys_svc_ap);
    ok &= register_service(sta, sys_info_handler, ESP_WS_API_SYS_INFO, &sys_svc_ro);

    /* перезагрузка — не настройка, доступна с любого интерфейса: на STA-ядре у неё
     * контекст, который менять разрешает, но клиент не на точке доступа */
    ok &= register_service(ap,  sys_reboot_handler, ESP_WS_API_SYS_REBOOT, &sys_svc_ap);
    ok &= register_service(sta, sys_reboot_handler, ESP_WS_API_SYS_REBOOT, &sys_svc_mut);

    return ok ? ESP_OK : ESP_FAIL;
}
