#include <esp_log.h>
#include "esp_littlefs.h"
#include <esp_http_server.h>

#include "app.h"

static const char *TAG = "web_server";

void connect_handler(void* arg, esp_event_base_t event_base,
                            int32_t event_id, void* event_data)
{
    app_context_t* app = (app_context_t*) arg;
    if (app->web_server == NULL) {
        ESP_LOGI(TAG, "Starting webserver");
        app->web_server = start_webserver();
    }
}

void disconnect_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    app_context_t* app = (app_context_t*) arg;
    if (app->web_server) {
        ESP_LOGI(TAG, "Stopping webserver");
        if (httpd_stop(app->web_server) == ESP_OK) {
            app->web_server = NULL;
        } else {
            ESP_LOGE(TAG, "Failed to stop http server");
        }
    }
}

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

