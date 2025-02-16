#include <stdio.h>
#include <string.h>
#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_http_server.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "cJSON.h"

#include "app.h"

static const char *TAG = "connection";

// Обработчик событий Wi-Fi
static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    //app_context_t* app = (app_context_t*) arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "Wi-Fi disconnected, trying to reconnect...");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Wi-Fi connected, IP: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

// Функция для инициализации Wi-Fi в режиме AP+STA
void wifi_init_ap_sta(wifi_config_t *ap_cnf, wifi_config_t *sta_cnf) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Инициализация интерфейса STA
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    assert(sta_netif);

    // Инициализация интерфейса AP
    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    assert(ap_netif);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Регистрация обработчика событий Wi-Fi
    // ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, &app_context));
    // ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, &app_context));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    // Настройка режима AP+STA
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    // Конфигурация STA (если указаны SSID и пароль)
    if ((sta_cnf != NULL) && (sta_cnf->sta.ssid[0] != '\0') && (sta_cnf->sta.password[0] != '\0')) {
        ESP_LOGI(TAG, "Station connecting: SSID=%s, Password=%s", sta_cnf->sta.ssid, sta_cnf->sta.password);
        ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, sta_cnf));
    }

    ESP_LOGI(TAG, "Starting AP: SSID=%s, Password=%s", ap_cnf->ap.ssid, ap_cnf->ap.password);
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, ap_cnf));

    // Запуск Wi-Fi
    ESP_ERROR_CHECK(esp_wifi_start());
    if ((sta_cnf != NULL) && (sta_cnf->sta.ssid[0] != '\0')) {
        ESP_ERROR_CHECK(esp_wifi_connect());
    }

    ESP_LOGI(TAG, "Wi-Fi initialized in AP+STA mode");
    if ((sta_cnf != NULL) && (sta_cnf->sta.ssid[0] != '\0')) {
        ESP_LOGI(TAG, "STA: Connecting to %s...", sta_cnf->sta.ssid);
    }
}
