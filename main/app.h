#pragma once

#include "esp_littlefs.h"
#include "esp_http_server.h"
#include "driver/uart.h"
#include "connection.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_wifi_types_generic.h"
#include "esp_wifi_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define max(a,b) \
({ __typeof__ (a) _a = (a); \
    __typeof__ (b) _b = (b); \
  _a > _b ? _a : _b; })

  #define min(a,b) \
({ __typeof__ (a) _a = (a); \
    __typeof__ (b) _b = (b); \
  _a < _b ? _a : _b; })

extern esp_vfs_littlefs_conf_t conf;
bool directory_exists(const char *path);
esp_err_t setup_littlefs(void);


/*=======================*/

#define WEB_FILE_HANDLER_NAME "/*"

esp_err_t stop_webserver(httpd_handle_t server);

httpd_handle_t start_webserver(void);
uint8_t webserver_register_handler(httpd_handle_t server, httpd_uri_t *uri_handler);

extern httpd_uri_t file_server;
extern httpd_uri_t dir_list;
extern httpd_uri_t file_upload;
extern httpd_uri_t file_delete;

/*========================*/

typedef struct {
    wifi_config_t ap_cnf;
    wifi_config_t sta_cnf;

    uart_config_t uart_cnf;

    httpd_handle_t web_server;
} app_context_t;

httpd_uri_t *pxWsServerInit(char *uri);

/*==========================*/

void uart_reconfigure(uart_config_t *uart_cnf);
void uart_send_data(const char *data, size_t len);


/*==========================*/

bool load_config(app_context_t *app);

#ifdef __cplusplus
}
#endif
