// #include <stdio.h>
// #include <string.h>
// #include "esp_system.h"
// #include "esp_log.h"
// #include "esp_event.h"
// #include "nvs_flash.h"
// #include "nvs.h"
// #include "esp_http_server.h"
// #include "driver/uart.h"
// #include "freertos/FreeRTOS.h"
// #include "freertos/task.h"
// #include "freertos/queue.h"
// #include "cJSON.h"

// // Настройки Wi-Fi (AP mode)
// #define WIFI_SSID_AP "ESP32_UART_BRIDGE"    // Имя точки доступа
// #define WIFI_PASS_AP "esp32uart"            // Пароль точки доступа

// // Настройки WebSocket
// #define WS_PORT 8080

// // Настройки UART по умолчанию
// #define UART_PORT UART_NUM_1
// #define UART_RX_PIN 16
// #define UART_TX_PIN 17
// #define UART_BUF_SIZE 1024

// // Ключи для сохранения в NVS
// #define NVS_NAMESPACE "wifi_config"
// #define NVS_KEY_SSID "sta_ssid"
// #define NVS_KEY_PASS "sta_pass"

// // Тэг для логов
// static const char *TAG = "UART_WS_BRIDGE";

// // Очередь для данных UART
// QueueHandle_t uart_queue;

// // WebSocket-клиенты
// httpd_handle_t server = NULL;

// // Функция для инициализации UART
// void uart_init() {
//     uart_config_t uart_config = {
//         .baud_rate = 115200,
//         .data_bits = UART_DATA_8_BITS,
//         .parity = UART_PARITY_DISABLE,
//         .stop_bits = UART_STOP_BITS_1,
//         .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
//         .source_clk = UART_SCLK_APB,
//     };
//     uart_param_config(UART_PORT, &uart_config);
//     uart_set_pin(UART_PORT, UART_TX_PIN, UART_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
//     uart_driver_install(UART_PORT, UART_BUF_SIZE, UART_BUF_SIZE, 10, &uart_queue, 0);
// }

// // Функция для изменения настроек UART
// void uart_reconfigure(int baud_rate) {
//     uart_baud_rate = baud_rate;
//     uart_set_baudrate(UART_PORT, uart_baud_rate);
//     ESP_LOGI(TAG, "UART reconfigured: baud_rate=%d", uart_baud_rate);
// }

// // Функция для отправки данных на UART
// void uart_send_data(const char *data, size_t len) {
//     uart_write_bytes(UART_PORT, data, len);
//     ESP_LOGI(TAG, "Data sent to UART: %.*s", len, data);
// }

// // Обработчик событий Wi-Fi
// static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
//     if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
//         ESP_LOGI(TAG, "Wi-Fi disconnected, trying to reconnect...");
//         esp_wifi_connect();
//     } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
//         ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
//         ESP_LOGI(TAG, "Wi-Fi connected, IP: " IPSTR, IP2STR(&event->ip_info.ip));
//     }
// }

// // Функция для инициализации Wi-Fi в режиме AP+STA
// void wifi_init_ap_sta(const char *sta_ssid, const char *sta_pass) {
//     ESP_ERROR_CHECK(esp_netif_init());
//     ESP_ERROR_CHECK(esp_event_loop_create_default());

//     // Инициализация интерфейса STA
//     esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
//     assert(sta_netif);

//     // Инициализация интерфейса AP
//     esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
//     assert(ap_netif);

//     wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
//     ESP_ERROR_CHECK(esp_wifi_init(&cfg));

//     // Регистрация обработчика событий Wi-Fi
//     ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
//     ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

//     // Настройка режима AP+STA
//     ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

//     // Конфигурация STA (если указаны SSID и пароль)
//     if (sta_ssid != NULL && sta_pass != NULL) {
//         wifi_config_t sta_config = {
//             .sta = {
//                 .ssid = "",
//                 .password = "",
//             },
//         };
//         strncpy((char *)sta_config.sta.ssid, sta_ssid, sizeof(sta_config.sta.ssid));
//         strncpy((char *)sta_config.sta.password, sta_pass, sizeof(sta_config.sta.password));
//         ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &sta_config));
//     }

//     // Конфигурация AP
//     wifi_config_t ap_config = {
//         .ap = {
//             .ssid = WIFI_SSID_AP,
//             .password = WIFI_PASS_AP,
//             .max_connection = 4,
//             .authmode = WIFI_AUTH_WPA2_PSK,
//         },
//     };
//     ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, &ap_config));

//     // Запуск Wi-Fi
//     ESP_ERROR_CHECK(esp_wifi_start());
//     if (sta_ssid != NULL && sta_pass != NULL) {
//         ESP_ERROR_CHECK(esp_wifi_connect());
//     }

//     ESP_LOGI(TAG, "Wi-Fi initialized in AP+STA mode");
//     if (sta_ssid != NULL) {
//         ESP_LOGI(TAG, "STA: Connecting to %s...", sta_ssid);
//     }
//     ESP_LOGI(TAG, "AP: SSID=%s, Password=%s", WIFI_SSID_AP, WIFI_PASS_AP);
// }

// // Обработчик WebSocket-сообщений
// esp_err_t ws_handler(httpd_req_t *req) {
//     if (req->method == HTTP_GET) {
//         ESP_LOGI(TAG, "WebSocket connection established");
//         return ESP_OK;
//     }

//     uint8_t buf[UART_BUF_SIZE];
//     int len = httpd_ws_recv_frame(req, buf, sizeof(buf));
//     if (len < 0) {
//         ESP_LOGE(TAG, "WebSocket receive failed");
//         return ESP_FAIL;
//     }

//     // Обработка полученного сообщения
//     buf[len] = '\0';
//     ESP_LOGI(TAG, "WebSocket message received: %s", buf);

//     // Парсинг JSON
//     cJSON *json = cJSON_Parse((char *)buf);
//     if (json == NULL) {
//         ESP_LOGE(TAG, "Invalid JSON");
//         return ESP_FAIL;
//     }

//     // Обработка команды для настройки UART
//     cJSON *baud_rate_json = cJSON_GetObjectItem(json, "baud_rate");
//     if (baud_rate_json != NULL) {
//         int baud_rate = baud_rate_json->valueint;
//         uart_reconfigure(baud_rate);
//     }

//     // Обработка команды для настройки Wi-Fi STA
//     cJSON *wifi_ssid_json = cJSON_GetObjectItem(json, "wifi_ssid");
//     cJSON *wifi_pass_json = cJSON_GetObjectItem(json, "wifi_pass");
//     if (wifi_ssid_json != NULL && wifi_pass_json != NULL) {
//         const char *ssid = wifi_ssid_json->valuestring;
//         const char *pass = wifi_pass_json->valuestring;
//         save_wifi_config(ssid, pass);
//         wifi_init_ap_sta(ssid, pass);
//     }

//     // Обработка команды для отправки данных на UART
//     cJSON *uart_data_json = cJSON_GetObjectItem(json, "uart_data");
//     if (uart_data_json != NULL) {
//         const char *uart_data = uart_data_json->valuestring;
//         uart_send_data(uart_data, strlen(uart_data));
//     }

//     cJSON_Delete(json);
//     return ESP_OK;
// }

// // Задача для чтения данных с UART и отправки через WebSocket
// void uart_to_websocket_task(void *pvParameters) {
//     uint8_t *data = (uint8_t *)malloc(UART_BUF_SIZE);
//     while (1) {
//         int len = uart_read_bytes(UART_PORT, data, UART_BUF_SIZE, pdMS_TO_TICKS(100));
//         if (len > 0) {
//             ESP_LOGI(TAG, "UART Data Received: %.*s", len, data);
//             // Отправляем данные всем подключенным WebSocket-клиентам
//             if (server != NULL) {
//                 httpd_ws_frame_t ws_pkt = {
//                     .final = true,
//                     .fragmented = false,
//                     .type = HTTPD_WS_TYPE_TEXT,
//                     .payload = data,
//                     .len = len,
//                 };
//                 httpd_ws_send_frame_async(server, httpd_req_to_sockfd(server->hd_req), &ws_pkt);
//             }
//         }
//     }
//     free(data);
// }

// void app_main() {
//     // Инициализация NVS
//     esp_err_t ret = nvs_flash_init();
//     if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
//         ESP_ERROR_CHECK(nvs_flash_erase());
//         ret = nvs_flash_init();
//     }
//     ESP_ERROR_CHECK(ret);

//     // Загрузка настроек Wi-Fi из NVS
//     bool has_wifi_config = load_wifi_config(sta_ssid, sta_pass, sizeof(sta_ssid));

//     // Инициализация Wi-Fi в режиме AP+STA
//     wifi_init_ap_sta(has_wifi_config ? sta_ssid : NULL, has_wifi_config ? sta_pass : NULL);

//     // Инициализация UART
//     uart_init();

//     // Инициализация WebSocket-сервера
//     websocket_server_init();

//     // Создаем задачу для чтения UART и отправки через WebSocket
//     xTaskCreate(uart_to_websocket_task, "uart_to_ws", 4096, NULL, 5, NULL);

//     ESP_LOGI(TAG, "UART to WebSocket bridge started!");
// }
