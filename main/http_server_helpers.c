#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_system.h>
#include <nvs_flash.h>
#include <sys/param.h>
#include <sys/stat.h>
#include "esp_netif.h"
#include "esp_eth.h"
#include "protocol_examples_common.h"
#include "ws_api.h"
#include "esp_littlefs.h"
#include <tftp_server_wg.h>

#include <esp_http_server.h>
#include <stdio.h>
#include "app.h"

static const char *TAG = "http_server_helpers";


/* Copies the full path into destination buffer and returns
 * pointer to path (skipping the preceding base path) */
static const char* get_path_from_uri(char *dest, const char *base_path, const char *uri, size_t destsize)
{
    const size_t base_pathlen = strlen(base_path);
    size_t pathlen = strlen(uri);

    const char *quest = strchr(uri, '?');
    if (quest) {
        pathlen = MIN(pathlen, quest - uri);
    }
    const char *hash = strchr(uri, '#');
    if (hash) {
        pathlen = MIN(pathlen, hash - uri);
    }

    if (base_pathlen + pathlen + 1 > destsize) {
        /* Full path string won't fit into destination buffer */
        return NULL;
    }

    /* Construct full path (base + path) */
    strcpy(dest, base_path);
    strlcpy(dest + base_pathlen, uri, pathlen + 1);

    /* Return pointer to path, skipping the base */
    return dest + base_pathlen;
}

#define IS_FILE_EXT(filename, ext) \
    (strcasecmp(&filename[strlen(filename) - sizeof(ext) + 1], ext) == 0)

/* Set HTTP response content type according to file extension */
static esp_err_t set_content_type_from_file(httpd_req_t *req, const char *filename)
{
    if (IS_FILE_EXT(filename, ".pdf")) {
        return httpd_resp_set_type(req, "application/pdf");
    } else if (IS_FILE_EXT(filename, ".html")) {
        return httpd_resp_set_type(req, "text/html");
    } else if (IS_FILE_EXT(filename, ".jpeg")) {
        return httpd_resp_set_type(req, "image/jpeg");
    } else if (IS_FILE_EXT(filename, ".ico")) {
        return httpd_resp_set_type(req, "image/x-icon");
    }
    /* This is a limited set only */
    /* For any other type always set as plain text */
    return httpd_resp_set_type(req, "text/plain");
}

// Just a copy from esp-idf (it is static in esp-idf)
static bool httpd_uri_match_simple(const char *uri1, const char *uri2, size_t len2)
{
    return strlen(uri1) == len2 &&          // First match lengths
        (strncmp(uri1, uri2, len2) == 0);   // Then match actual URIs
}

static bool server_uri_matcher(const char *uri1, const char *uri2, size_t len2)
{
	//NOTE: uri1 is always http_data (server data) => we can check it for 
	//default handler
	
//        ESP_LOGI(TAG, "%s: uri1: %s   uri2: %s\n", __func__, uri1, uri2);
	if (httpd_uri_match_simple(uri1, uri2, len2))
		return true;

	// 1. it is already registere2
	// 2. we haven't found any handlers and come up to NO_ONE_HTTP_NAME
	if (strcmp(uri1, NO_ONE_HTTP_NAME) == 0)
		return true;

	return false;
}

void connect_handler(void* arg, esp_event_base_t event_base,
                            int32_t event_id, void* event_data)
{
    httpd_handle_t* server = (httpd_handle_t*) arg;
    if (*server == NULL) {
        ESP_LOGI(TAG, "Starting webserver");
        *server = start_webserver();
    }
}

void disconnect_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    httpd_handle_t* server = (httpd_handle_t*) arg;
    if (*server) {
        ESP_LOGI(TAG, "Stopping webserver");
        if (stop_webserver(*server) == ESP_OK) {
            *server = NULL;
        } else {
            ESP_LOGE(TAG, "Failed to stop http server");
        }
    }
}

esp_err_t stop_webserver(httpd_handle_t server)
{
    // Stop the httpd server
    return httpd_stop(server);
}

httpd_handle_t start_webserver(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    // Rewrite uri_match_fn which is NULL and is set by HTTPD_DEFAULT_CONFIG()
    config.uri_match_fn = server_uri_matcher;

    // Start the httpd server
    ESP_LOGI(TAG, "Starting server on port: '%d'", config.server_port);
    if (httpd_start(&server, &config) == ESP_OK) {
        ESP_LOGI(TAG, "Registering URI handlers");
        httpd_register_uri_handler(server, &ws);
        //httpd_register_uri_handler(server, &http_index);
	
	// WARNINNG: keep no_one_http handler at the end because it will always be called
	// in case of any matches. This guaranteed by uri_match_fn which check code name
	// and return the hander
        httpd_register_uri_handler(server, &no_one_http);
        return server;
    }

    ESP_LOGI(TAG, "Error starting server!");
    return NULL;
}

