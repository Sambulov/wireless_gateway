#include <esp_log.h>
#include "esp_littlefs.h"
#include <esp_http_server.h>

#include "app.h"

static const char *TAG = "web_server";
httpd_handle_t server = NULL;

static void httpd_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    switch (event_id) {
        case HTTP_SERVER_EVENT_ERROR : //httpd_err_code_t
          ESP_LOGW(TAG, "error: '%lu'", (uint32_t)event_data);
          break;
        case HTTP_SERVER_EVENT_START : //NULL
          ESP_LOGW(TAG, "started");
          break;
        case HTTP_SERVER_EVENT_ON_CONNECTED : //int
          ESP_LOGW(TAG, "new connection: '%lu'", (uint32_t)event_data);
          break;
        case HTTP_SERVER_EVENT_ON_HEADER : //int
          ESP_LOGW(TAG, "on header: '%lu'", (uint32_t)event_data);
          break;
        case HTTP_SERVER_EVENT_HEADERS_SENT : //int
          ESP_LOGW(TAG, "headers sent: '%lu'", (uint32_t)event_data);
          break;
        case HTTP_SERVER_EVENT_ON_DATA : //esp_http_server_event_data        
          ESP_LOGW(TAG, "on data: fd: '%u' len:'%u'", 
            ((esp_http_server_event_data *)event_data)->fd, ((esp_http_server_event_data *)event_data)->data_len);
          break;
        case HTTP_SERVER_EVENT_SENT_DATA : //esp_http_server_event_data        
          ESP_LOGW(TAG, "data sent: fd: '%u' len:'%u'", 
            ((esp_http_server_event_data *)event_data)->fd, ((esp_http_server_event_data *)event_data)->data_len);
          break;
        case HTTP_SERVER_EVENT_DISCONNECTED : //int
          ESP_LOGW(TAG, "disconnected: '%lu'", (uint32_t)event_data);
          break;
        case HTTP_SERVER_EVENT_STOP : //NULL
          ESP_LOGW(TAG, "stopped");
            break;
        default:
            break;
    }
}

httpd_handle_t start_webserver(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    config.uri_match_fn = httpd_uri_match_wildcard;
    config.server_port = CONFIG_WEB_SERVER_PORT;
    config.keep_alive_enable = true;
    config.keep_alive_idle = 30;
    config.keep_alive_interval = 5;
    config.keep_alive_count = 3;
    
    esp_event_handler_register(ESP_HTTP_SERVER_EVENT ,ESP_EVENT_ANY_ID, &httpd_event_handler, NULL);

    // Start the httpd server
    ESP_LOGI(TAG, "Starting server on port: '%d'", config.server_port);
    if (httpd_start(&server, &config) == ESP_OK) return server;
    ESP_LOGI(TAG, "Error starting server!");
    return NULL;
}

uint8_t webserver_register_handler(httpd_handle_t server, httpd_uri_t *uri_handler) {
  return httpd_register_uri_handler(server, uri_handler) == ESP_OK;
}
