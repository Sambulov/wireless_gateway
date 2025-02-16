#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "app.h"

#define NVS_NAMESPACE "wifi_config"
#define NVS_KEY_AP_SSID "ap_ssid"
#define NVS_KEY_AP_PASS "ap_pass"
#define NVS_KEY_STA_SSID "sta_ssid"
#define NVS_KEY_STA_PASS "sta_pass"

static const char *TAG = "nvs_config";


void save_wifi_sta_config(const char *ssid, const char *password) {
    nvs_handle_t nvs_handle;
    ESP_ERROR_CHECK(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, NVS_KEY_STA_SSID, ssid));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, NVS_KEY_STA_PASS, password));
    ESP_ERROR_CHECK(nvs_commit(nvs_handle));
    nvs_close(nvs_handle);
    ESP_LOGI(TAG, "Wi-Fi station settings saved to NVS: SSID=%s", ssid);
}

void save_wifi_ap_config(const char *ssid, const char *password) {
    nvs_handle_t nvs_handle;
    ESP_ERROR_CHECK(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, NVS_KEY_AP_SSID, ssid));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, NVS_KEY_AP_PASS, password));
    ESP_ERROR_CHECK(nvs_commit(nvs_handle));
    nvs_close(nvs_handle);
    ESP_LOGI(TAG, "Wi-Fi AP settings saved to NVS: SSID=%s", ssid);
}

static bool _load_config_default(app_context_t *app) {
    app->ap_cnf.ap.ssid[0] = '\0';
    app->ap_cnf.ap.ssid_len = min(sizeof(CONFIG_WIFI_AP_DEFAULT_SSID), sizeof(app->ap_cnf.ap.ssid));
    strlcpy((char *)app->ap_cnf.ap.ssid, CONFIG_WIFI_AP_DEFAULT_SSID, app->ap_cnf.ap.ssid_len);

    app->ap_cnf.ap.password[0] = '\0';
    size_t pass_len = min(sizeof(CONFIG_WIFI_AP_DEFAULT_PASS), sizeof(app->ap_cnf.ap.password));
    strlcpy((char *)app->ap_cnf.ap.password, CONFIG_WIFI_AP_DEFAULT_PASS, pass_len);

    app->ap_cnf.ap.max_connection = 4;
    app->ap_cnf.ap.authmode = WIFI_AUTH_WPA2_PSK;

    app->sta_cnf.sta.ssid[0] = '\0';
    app->sta_cnf.sta.password[0] = '\0';

    ESP_LOGI(TAG, "App config loaded default");
    return true;
}

bool load_config(app_context_t *app) {
    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle) != ESP_OK)
        return _load_config_default(app);

    app->ap_cnf.ap.ssid_len = sizeof(app->ap_cnf.ap.ssid);
    size_t len = sizeof(app->ap_cnf.ap.ssid);
    app->ap_cnf.ap.ssid_len = 0;
    if(nvs_get_str(nvs_handle, NVS_KEY_AP_SSID, (char *)app->ap_cnf.ap.ssid, &len) == ESP_OK)
        app->ap_cnf.ap.ssid_len = len;

    len = sizeof(app->ap_cnf.ap.password);
    if(nvs_get_str(nvs_handle, NVS_KEY_AP_PASS, (char *)app->ap_cnf.ap.password, &len) != ESP_OK)
        app->ap_cnf.ap.password[0] = '\0';

    len = sizeof(app->sta_cnf.sta.ssid);
    if(nvs_get_str(nvs_handle, NVS_KEY_STA_SSID, (char *)app->sta_cnf.sta.ssid, &len) != ESP_OK)
        app->sta_cnf.sta.ssid[0] = '\0';

    len = sizeof(app->sta_cnf.sta.password);
    if(nvs_get_str(nvs_handle, NVS_KEY_STA_PASS, (char *)app->sta_cnf.sta.password, &len) != ESP_OK)
        app->sta_cnf.sta.password[0] = '\0';

    nvs_close(nvs_handle);
    ESP_LOGI(TAG, "App config loaded from NVS");
    return true;
}
