#pragma once

#include "CodeLib.h"

#include "esp_littlefs.h"
#include "esp_http_server.h"
#include "driver/uart.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_eth.h"
#include "esp_wifi_types_generic.h"
#include "esp_wifi_types.h"

#include "nvs_flash.h"
#include "sys/param.h"
#include "sys/stat.h"


#include "web_api.h"

#include <stdio.h>

#include "sys_def.h"

#include "protocol_examples_common.h"
#include "esp_littlefs.h"
#include "tftp_server_wg.h"
#include "esp_http_server.h"

#include "uart.h"
#include "connection.h"








#define TAG_S(x)  #x
#define TAG_SX(x) TAG_S(x)
#define TAG  TAG_SX(__FILE__) " " TAG_SX(__LINE__)

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

httpd_handle_t start_webserver(void);
uint8_t webserver_register_handler(httpd_handle_t server, httpd_uri_t *uri_handler);

extern httpd_uri_t file_server;
extern httpd_uri_t dir_list;
extern httpd_uri_t file_upload;
extern httpd_uri_t file_delete;

/*========================*/

typedef enum {
  UART_PORT_MODE_RAW,
  UART_PORT_MODE_MODBUS
} uart_port_mode_t;

typedef struct {
    wifi_config_t ap_cnf;
    wifi_config_t sta_cnf;


    httpd_handle_t web_server;


    struct {
      uint32_t raw_sent_ts;
      struct {
        gw_uart_t desc;
        gw_uart_config_t cnf;
        uart_port_mode_t mode;
      } port[2];
    } uart;
} app_context_t;

httpd_uri_t *pxWsServerInit(char *uri);

/*==========================*/

bool load_config(app_context_t *app);

/*==========================*/

#define ESP_WS_API_UART1_CNF     0x1010
#define ESP_WS_API_UART1_RAW_RX  0x1011
#define ESP_WS_API_UART1_RAW_TX  0x1012
#define ESP_WS_API_UART2_CNF     0x1020
#define ESP_WS_API_UART2_RAW_RX  0x1021
#define ESP_WS_API_UART2_RAW_TX  0x1022

void api_handler_uart_work(app_context_t *app);

// #define ESP_WS_API_MODBUS_ID        2000
// void api_handler_modbus_work(app_context_t *app);

#define ESP_WS_API_ECHO_ID          1000
#define ESP_WS_API_CONT_ID          1001
#define ESP_WS_API_ASYNC_ID         1002

void api_handler_system_work(app_context_t *app);

#ifdef __cplusplus
}
#endif
