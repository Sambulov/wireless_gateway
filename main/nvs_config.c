#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "app.h"

#define GW_LOG_MODULE GW_LOG_NVS
#include "gw_log.h"

#define NVS_NAMESPACE "wifi_config"
#define NVS_KEY_AP_SSID "ap_ssid"
#define NVS_KEY_AP_PASS "ap_pass"
#define NVS_KEY_STA_SSID "sta_ssid"
#define NVS_KEY_STA_PASS "sta_pass"
#define NVS_KEY_NET_STA "net_sta"
#define NVS_KEY_NET_AP  "net_ap"
/* Выбранная точка доступа (BSSID, 6 байт). Ключа нет — «любая с этим именем». */
#define NVS_KEY_STA_BSSID "sta_bssid"
/* Персональные MAC интерфейсов (6 байт). Ключа нет — MAC заводской. */
#define NVS_KEY_MAC_STA "mac_sta"
#define NVS_KEY_MAC_AP  "mac_ap"

#define AP_MAX_CONNECTIONS 4

/* Default softAP credentials are unique per device: the SSID carries the last
 * three MAC bytes ("WG-310184"), the password is a 10-character digest of the
 * whole MAC. The alphabet leaves out 0/O/1/l so it stays unambiguous in a log. */
#define AP_SSID_PREFIX "WG-"
#define AP_PASS_LEN    10

static const char ap_pass_alphabet[] = "abcdefghijkmnpqrstuvwxyz23456789";

void wifi_ap_credentials_default(char *ssid, size_t ssid_size, char *pass, size_t pass_size) {
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);

    /* Kconfig values, when set, win over the per-device generated ones */
    const char *cnf_ssid = CONFIG_WIFI_AP_DEFAULT_SSID;
    const char *cnf_pass = CONFIG_WIFI_AP_DEFAULT_PASS;

    char gen_ssid[33];
    char gen_pass[AP_PASS_LEN + 1];
    snprintf(gen_ssid, sizeof(gen_ssid), AP_SSID_PREFIX "%02X%02X%02X", mac[3], mac[4], mac[5]);

    uint32_t h = 0x811C9DC5u; /* FNV-1a over the MAC, then LCG-expanded */
    for (int i = 0; i < 6; i++) {
        h ^= mac[i];
        h *= 0x01000193u;
    }
    for (int i = 0; i < AP_PASS_LEN; i++) {
        gen_pass[i] = ap_pass_alphabet[h & 0x1Fu];
        h = h * 1664525u + 1013904223u;
        h ^= h >> 13;
    }
    gen_pass[AP_PASS_LEN] = '\0';

    strlcpy(ssid, (cnf_ssid[0] != '\0') ? cnf_ssid : gen_ssid, ssid_size);
    strlcpy(pass, (cnf_pass[0] != '\0') ? cnf_pass : gen_pass, pass_size);
}

void save_wifi_sta_config(const char *ssid, const char *password) {
    nvs_handle_t nvs_handle;
    ESP_ERROR_CHECK(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, NVS_KEY_STA_SSID, ssid));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, NVS_KEY_STA_PASS, password));
    ESP_ERROR_CHECK(nvs_commit(nvs_handle));
    nvs_close(nvs_handle);
    LOGI(TAG, "Wi-Fi station settings saved to NVS: SSID=%s", ssid);
}

void save_wifi_ap_config(const char *ssid, const char *password) {
    nvs_handle_t nvs_handle;
    ESP_ERROR_CHECK(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, NVS_KEY_AP_SSID, ssid));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, NVS_KEY_AP_PASS, password));
    ESP_ERROR_CHECK(nvs_commit(nvs_handle));
    nvs_close(nvs_handle);
    LOGI(TAG, "Wi-Fi AP settings saved to NVS: SSID=%s", ssid);
}

/* The softAP secret is usable only when 8..63 characters long (WPA2); an empty
 * password means an open network. */
uint8_t wifi_password_valid(const char *pass) {
    size_t len = strlen(pass);
    return (len == 0) || (len >= 8 && len <= 63);
}

/* ── IPv4 configuration ──────────────────────────────────────────────────── */

/* The station asks for an address by default; the access point keeps the esp_netif
 * default of 192.168.4.1/24 (ip == 0 — leave the built-in default alone). */
static void _net_defaults(net_iface_cnf_t *sta, net_iface_cnf_t *ap) {
    memset(sta, 0, sizeof(*sta));
    sta->mode = NET_MODE_DHCP;
    memset(ap, 0, sizeof(*ap));
    ap->mode = NET_MODE_STATIC;
}

static void _net_load(nvs_handle_t nvs_handle, const char *key, net_iface_cnf_t *cfg) {
    net_iface_cnf_t tmp;
    size_t len = sizeof(tmp);
    if (nvs_get_blob(nvs_handle, key, &tmp, &len) == ESP_OK && len == sizeof(tmp))
        *cfg = tmp;
}

void net_save_config(const app_context_t *app) {
    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) != ESP_OK) {
        ESP_LOGE(TAG, "Cannot open NVS to store network settings");
        return;
    }
    nvs_set_blob(nvs_handle, NVS_KEY_NET_STA, &app->net_sta, sizeof(app->net_sta));
    nvs_set_blob(nvs_handle, NVS_KEY_NET_AP, &app->net_ap, sizeof(app->net_ap));
    nvs_commit(nvs_handle);
    nvs_close(nvs_handle);

    char ip[16];
    net_format_ip4(app->net_sta.ip, ip, sizeof(ip));
    LOGI(TAG, "Network settings saved: station %s, AP %s",
             app->net_sta.mode == NET_MODE_STATIC ? ip : "dhcp",
             app->net_ap.ip ? "static" : "default");
}

/* ── Выбранная точка доступа ─────────────────────────────────────────────── */

void save_wifi_sta_bssid(const uint8_t *bssid) {
    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) != ESP_OK) {
        ESP_LOGE(TAG, "Cannot open NVS to store the selected access point");
        return;
    }
    if (bssid) {
        esp_err_t err = nvs_set_blob(nvs_handle, NVS_KEY_STA_BSSID, bssid, 6);
        if (err != ESP_OK) ESP_LOGE(TAG, "BSSID store failed: %s", esp_err_to_name(err));
    } else {
        esp_err_t err = nvs_erase_key(nvs_handle, NVS_KEY_STA_BSSID);
        if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND)
            ESP_LOGE(TAG, "BSSID erase failed: %s", esp_err_to_name(err));
    }
    nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
    LOGI(TAG, "Selected access point %s", bssid ? "stored in NVS" : "cleared");
}

/* ── MAC адреса интерфейсов ──────────────────────────────────────────────── */

/* Значение, которое драйвер примет: первый байт без бита групповой адресации (иначе
 * ESP_ERR_WIFI_MAC) и не все нули. */
uint8_t wifi_mac_valid(const uint8_t *mac) {
    static const uint8_t zero[6] = {0};
    if (!mac) return 0;
    if (mac[0] & 0x01) return 0;
    return memcmp(mac, zero, sizeof(zero)) != 0;
}

void save_wifi_mac_config(uint8_t ap, const uint8_t *mac) {
    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) != ESP_OK) {
        ESP_LOGE(TAG, "Cannot open NVS to store the MAC address");
        return;
    }
    const char *key = ap ? NVS_KEY_MAC_AP : NVS_KEY_MAC_STA;

    if (wifi_mac_valid(mac)) {
        esp_err_t err = nvs_set_blob(nvs_handle, key, mac, 6);
        if (err != ESP_OK) ESP_LOGE(TAG, "MAC store failed: %s", esp_err_to_name(err));
    } else {
        /* «Заводской» — отсутствие записи, а не шесть нулей: так его не спутать с адресом. */
        esp_err_t err = nvs_erase_key(nvs_handle, key);
        if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND)
            ESP_LOGE(TAG, "MAC erase failed: %s", esp_err_to_name(err));
    }
    nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
    LOGI(TAG, "%s MAC %s", ap ? "AP" : "STA",
             wifi_mac_valid(mac) ? "stored in NVS (applied on reboot)" : "reset to the factory value");
}

/* Заводские MAC читаются из eFuse ДО первого esp_wifi_set_mac: после подмены адреса
 * взять их будет уже неоткуда. */
static void _mac_factory_read(app_context_t *app) {
    if (esp_read_mac(app->mac_sta_factory, ESP_MAC_WIFI_STA) != ESP_OK)
        memset(app->mac_sta_factory, 0, sizeof(app->mac_sta_factory));
    if (esp_read_mac(app->mac_ap_factory, ESP_MAC_WIFI_SOFTAP) != ESP_OK)
        memset(app->mac_ap_factory, 0, sizeof(app->mac_ap_factory));
}

static void _mac_load(nvs_handle_t nvs_handle, const char *key, uint8_t *dst) {
    uint8_t tmp[6];
    size_t len = sizeof(tmp);
    memset(dst, 0, 6);
    if (nvs_get_blob(nvs_handle, key, tmp, &len) == ESP_OK && len == sizeof(tmp)
        && wifi_mac_valid(tmp))
        memcpy(dst, tmp, sizeof(tmp));
}

static void _fill_ap_default(app_context_t *app) {
#if CONFIG_WIFI_AP_ENABLE
    char ssid[sizeof(app->ap_cnf.ap.ssid)];
    char pass[sizeof(app->ap_cnf.ap.password)];
    wifi_ap_credentials_default(ssid, sizeof(ssid), pass, sizeof(pass));

    memset(&app->ap_cnf, 0, sizeof(app->ap_cnf));
    strlcpy((char *)app->ap_cnf.ap.ssid, ssid, sizeof(app->ap_cnf.ap.ssid));
    app->ap_cnf.ap.ssid_len = strlen(ssid);
    strlcpy((char *)app->ap_cnf.ap.password, pass, sizeof(app->ap_cnf.ap.password));
    app->ap_cnf.ap.max_connection = AP_MAX_CONNECTIONS;
    app->ap_cnf.ap.authmode = (pass[0] != '\0') ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
#endif
}

static bool _load_config_default(app_context_t *app) {
    _fill_ap_default(app);
    _net_defaults(&app->net_sta, &app->net_ap);

    strlcpy((char *)app->sta_cnf.sta.ssid,     CONFIG_WIFI_STA_DEFAULT_SSID, sizeof(app->sta_cnf.sta.ssid));
    strlcpy((char *)app->sta_cnf.sta.password, CONFIG_WIFI_STA_DEFAULT_PASS, sizeof(app->sta_cnf.sta.password));

    LOGI(TAG, "App config loaded default");
    return true;
}

bool load_config(app_context_t *app) {
    /* Заводские MAC — первым делом: дальше их не восстановить (см. _mac_factory_read). */
    _mac_factory_read(app);
    memset(app->mac_sta, 0, sizeof(app->mac_sta));
    memset(app->mac_ap, 0, sizeof(app->mac_ap));

    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle) != ESP_OK)
        return _load_config_default(app);

    size_t len;

    _net_defaults(&app->net_sta, &app->net_ap);
    _net_load(nvs_handle, NVS_KEY_NET_STA, &app->net_sta);
    _net_load(nvs_handle, NVS_KEY_NET_AP, &app->net_ap);

    _mac_load(nvs_handle, NVS_KEY_MAC_STA, app->mac_sta);
    _mac_load(nvs_handle, NVS_KEY_MAC_AP, app->mac_ap);
    _mac_load(nvs_handle, NVS_KEY_STA_BSSID, app->sta_bssid);

#if CONFIG_WIFI_AP_ENABLE
    _fill_ap_default(app);

    len = sizeof(app->ap_cnf.ap.ssid);
    if (nvs_get_str(nvs_handle, NVS_KEY_AP_SSID, (char *)app->ap_cnf.ap.ssid, &len) == ESP_OK) {
        /* nvs_get_str() counts the terminator, while ssid_len must not */
        app->ap_cnf.ap.ssid_len = strlen((char *)app->ap_cnf.ap.ssid);
    }

    len = sizeof(app->ap_cnf.ap.password);
    if (nvs_get_str(nvs_handle, NVS_KEY_AP_PASS, (char *)app->ap_cnf.ap.password, &len) == ESP_OK) {
        app->ap_cnf.ap.authmode = (app->ap_cnf.ap.password[0] != '\0')
                                  ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    }
#endif

    len = sizeof(app->sta_cnf.sta.ssid);
    if(nvs_get_str(nvs_handle, NVS_KEY_STA_SSID, (char *)app->sta_cnf.sta.ssid, &len) != ESP_OK)
        strlcpy((char *)app->sta_cnf.sta.ssid, CONFIG_WIFI_STA_DEFAULT_SSID, sizeof(app->sta_cnf.sta.ssid));

    len = sizeof(app->sta_cnf.sta.password);
    if(nvs_get_str(nvs_handle, NVS_KEY_STA_PASS, (char *)app->sta_cnf.sta.password, &len) != ESP_OK)
        strlcpy((char *)app->sta_cnf.sta.password, CONFIG_WIFI_STA_DEFAULT_PASS, sizeof(app->sta_cnf.sta.password));

    nvs_close(nvs_handle);

    LOGI(TAG, "App config loaded from NVS");
    return true;
}
