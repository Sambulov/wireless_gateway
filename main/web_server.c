#include <esp_log.h>
#include "esp_littlefs.h"
#include <esp_http_server.h>
#include <esp_wifi.h>

#include "app.h"

httpd_handle_t server = NULL;

//static uint32_t ws_connections_left = CONFIG_WEB_SERVER_MAX_CLIENTS;
/* start_ws_server()'s httpd instance has max_open_sockets=5, and httpd
 * reserves 3 of those for itself (listen + ctrl + spare) -> only 2 WS
 * clients actually fit; a 3rd connection overflows the instance's socket
 * pool (httpd_queue_work failures cascading into NIC buffer exhaustion). */
static uint32_t ws_connections_left = 2; /* internally limited */

static void httpd_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    switch (event_id) {
        case HTTP_SERVER_EVENT_ERROR : //httpd_err_code_t
          ESP_LOGW(TAG, "error: '%08lx'", (uint32_t)event_data);
          break;
        case HTTP_SERVER_EVENT_START : //NULL
          ESP_LOGW(TAG, "started");
          break;
        case HTTP_SERVER_EVENT_ON_CONNECTED : //int
          if (ws_connections_left > 0)
              ws_connections_left--;
          ESP_LOGW(TAG, "new connection: '%08x'; connections left %lu", *(int *)event_data, ws_connections_left);
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
        case HTTP_SERVER_EVENT_SENT_DATA : { //esp_http_server_event_data
          wifi_ap_record_t ap_info;
          int8_t rssi = (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) ? ap_info.rssi : 0;
          ESP_LOGW(TAG, "data sent: fd: '%u' len:'%u' rssi:%d dBm",
            ((esp_http_server_event_data *)event_data)->fd, ((esp_http_server_event_data *)event_data)->data_len, rssi);
          break;
        }
        case HTTP_SERVER_EVENT_DISCONNECTED : //int
          ws_connections_left++;
          ESP_LOGW(TAG, "disconnected: '%08x'", *(int *)event_data);
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

    /* CONFIG_LWIP_MAX_SOCKETS is a single global pool shared by this
     * instance, start_ws_server()'s instance, and the TFTP server. Each
     * httpd instance also reserves 3 sockets internally (listen + ctrl +
     * one spare), so max_open_sockets=6 here leaves 3 usable HTTP clients. */
    config.max_open_sockets = 6;

    //config.open_fn = ws_open_fd;
    //config.close_fn = ws_close_fd;

    esp_event_handler_register(ESP_HTTP_SERVER_EVENT ,ESP_EVENT_ANY_ID, &httpd_event_handler, NULL);

    // Start the httpd server
    ESP_LOGI(TAG, "Starting server on port: '%d'", config.server_port);
    if (httpd_start(&server, &config) == ESP_OK) return server;
    ESP_LOGI(TAG, "Error starting server!");
    return NULL;
}

/* Own httpd instance (own FreeRTOS task + select() loop) for the WebSocket
 * API, kept off the HTTP file server's port. esp_http_server processes all
 * sessions of one instance serially in a single task, so a slow file
 * transfer on start_webserver()'s instance would otherwise starve WS
 * handshakes/frames queued on the same task. */
httpd_handle_t start_ws_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    config.server_port = CONFIG_WEB_SOCKET_SERVER_PORT;
    config.keep_alive_enable = true;
    config.keep_alive_idle = 30;
    config.keep_alive_interval = 5;
    config.keep_alive_count = 3;

    /* Same shared-socket-pool budget as start_webserver() above; 5 leaves
     * 2 usable WS clients. Total across both instances (6+5=11) plus TFTP
     * and lwIP/Wi-Fi internals must stay under CONFIG_LWIP_MAX_SOCKETS. */
    config.max_open_sockets = 5;

    /* Every httpd instance defaults to the SAME ctrl_port (internal UDP
     * control channel for cross-task signalling); two instances sharing
     * one ctrl_port would cross-talk. Give this instance its own. */
    config.ctrl_port = ESP_HTTPD_DEF_CTRL_PORT + 1;

    /* Event handler for ESP_HTTP_SERVER_EVENT is already registered in
     * start_webserver() above; that registration is instance-agnostic and
     * picks up events from this server too, so it must not be repeated
     * here (esp_event_handler_register is not idempotent). */

    ESP_LOGI(TAG, "Starting WS server on port: '%d'", config.server_port);
    httpd_handle_t ws_server = NULL;
    if (httpd_start(&ws_server, &config) == ESP_OK) return ws_server;
    ESP_LOGI(TAG, "Error starting WS server!");
    return NULL;
}

uint8_t webserver_register_handler(httpd_handle_t server, httpd_uri_t *uri_handler) {
  return httpd_register_uri_handler(server, uri_handler) == ESP_OK;
}
