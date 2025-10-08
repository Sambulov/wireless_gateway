#include <esp_log.h>
#include "esp_littlefs.h"
#include <esp_http_server.h>

#include "app.h"

httpd_handle_t server = NULL;

static uint32_t ws_connections = CONFIG_WEB_SOCKET_MAX_CLIENTS;

static void httpd_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    switch (event_id) {
        case HTTP_SERVER_EVENT_ERROR : //httpd_err_code_t
          ESP_LOGW(TAG, "error: '%08lx'", (uint32_t)event_data);
          break;
        case HTTP_SERVER_EVENT_START : //NULL
          ESP_LOGW(TAG, "started");
          break;
        case HTTP_SERVER_EVENT_ON_CONNECTED : //int
          ws_connections--;
          ESP_LOGW(TAG, "new connection: '%08lx'; connections left %lu", (uint32_t)event_data, ws_connections);
          break;
        case HTTP_SERVER_EVENT_ON_HEADER : //int
          ESP_LOGW(TAG, "on header: '%08lx'", (uint32_t)event_data);
          break;
        case HTTP_SERVER_EVENT_HEADERS_SENT : //int
          ESP_LOGW(TAG, "headers sent: '%08lx'", (uint32_t)event_data);
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
          ws_connections++;
          ESP_LOGW(TAG, "disconnected: '%08lx'", (uint32_t)event_data);
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
    config.max_uri_handlers = 20;
    config.server_port = CONFIG_WEB_SERVER_PORT;
    config.keep_alive_enable = true;
    config.keep_alive_idle = 30;
    config.keep_alive_interval = 5;
    config.keep_alive_count = 3;
    //config.open_fn = ws_open_fd;
    //config.close_fn = ws_close_fd;

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
