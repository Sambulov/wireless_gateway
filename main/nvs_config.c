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

static bool _load_config_default(app_context_t *cnf) {

    strlcpy(cnf->ap_ssid, CONFIG_WIFI_AP_DEFAULT_SSID, sizeof(cnf->ap_ssid));
    cnf->ap_ssid_len = sizeof(CONFIG_WIFI_AP_DEFAULT_SSID) - 1;

    strlcpy(cnf->ap_pass, CONFIG_WIFI_AP_DEFAULT_PASS, sizeof(cnf->ap_pass));
    cnf->ap_pass_len = sizeof(CONFIG_WIFI_AP_DEFAULT_PASS) - 1;

    cnf->sta_ssid_len = 0;
    cnf->sta_pass_len = 0;
    ESP_LOGI(TAG, "App config loaded default");
    return true;
}

bool load_config(app_context_t *cnf) {
    nvs_handle_t nvs_handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle) != ESP_OK)
        return _load_config_default(cnf);

    cnf->ap_ssid_len = sizeof(cnf->ap_ssid);
    if(nvs_get_str(nvs_handle, NVS_KEY_AP_SSID, cnf->ap_ssid, &cnf->ap_ssid_len) != ESP_OK)
        cnf->ap_ssid_len = 0;

    cnf->ap_pass_len = sizeof(cnf->ap_pass);
    if(nvs_get_str(nvs_handle, NVS_KEY_AP_PASS, cnf->ap_pass, &cnf->ap_pass_len) != ESP_OK)
        cnf->ap_pass_len = 0;

    cnf->sta_ssid_len = sizeof(cnf->sta_ssid);
    if(nvs_get_str(nvs_handle, NVS_KEY_STA_SSID, cnf->sta_ssid, &cnf->sta_ssid_len) != ESP_OK)
      cnf->sta_ssid_len = 0;

    cnf->sta_pass_len = sizeof(cnf->sta_pass);
    if(nvs_get_str(nvs_handle, NVS_KEY_STA_PASS, cnf->sta_pass, &cnf->sta_pass_len) != ESP_OK)
        cnf->sta_pass_len = 0;

    nvs_close(nvs_handle);
    ESP_LOGI(TAG, "App config loaded from NVS");
    return true;
}
