#include <esp_log.h>
#include "esp_littlefs.h"
#include <esp_http_server.h>

#include "app.h"

static const char *TAG = "web_server";

httpd_handle_t start_webserver(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    config.uri_match_fn = httpd_uri_match_wildcard;
    config.server_port = CONFIG_WEB_SERVER_PORT;

    // Start the httpd server
    ESP_LOGI(TAG, "Starting server on port: '%d'", config.server_port);
    if (httpd_start(&server, &config) == ESP_OK) {
        ESP_LOGI(TAG, "Registering URI handlers");
        httpd_register_uri_handler(server, &ws);
        httpd_register_uri_handler(server, &dir_list);
        httpd_register_uri_handler(server, &file_upload);
        httpd_register_uri_handler(server, &file_delete);
        httpd_register_uri_handler(server, &file_server);        
        return server;
    }

    ESP_LOGI(TAG, "Error starting server!");
    return NULL;
}

