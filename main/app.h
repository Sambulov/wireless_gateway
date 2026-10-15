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

#include "esp_littlefs.h"
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

/* IPv4 configuration of one interface. Addresses are kept in network byte
 * order; 0 means "not set" and falls back to the value lwIP/esp_netif provides
 * by default (DHCP for the station, 192.168.4.1/24 for the access point). */
typedef enum {
    NET_MODE_DHCP   = 0,
    NET_MODE_STATIC = 1,
} net_mode_t;

typedef struct {
    uint8_t  mode;   /* net_mode_t; the access point is always static */
    uint32_t ip;
    uint32_t mask;
    uint32_t gw;
    uint32_t dns1;
    uint32_t dns2;
} net_iface_cnf_t;

#define WEB_FILE_HANDLER_NAME "/*"

httpd_handle_t start_webserver(void);
httpd_handle_t start_ws_server(void);
/* Единственная точка регистрации URI-обработчика: здесь же применяется политика
 * «какой сервис каким интерфейсам оказан» — см. web_iface_policy.c. Обработчик
 * сервиса, оказанного не всем, регистрируется в обёртке, которая отвечает 404
 * соединению с неподходящего интерфейса. */
uint8_t webserver_register_handler(httpd_handle_t server, httpd_uri_t *uri_handler);

/* Файлы читаются по HTTP GET — общим обработчиком-ловушкой, а меняются только через
 * WebSocket-сервисы sys.file.* — HTTP-методов /upload/…, /delete/… и /rename/…
 * больше нет. */
extern httpd_uri_t file_server;
extern httpd_uri_t ota_update;
extern httpd_uri_t modbus_alias;
extern httpd_uri_t config_page;
extern httpd_uri_t config_page_html;

/*========================*/

typedef struct {
    wifi_config_t ap_cnf;
    wifi_config_t sta_cnf;
    /* IPv4 settings of the two interfaces, see net_iface_cnf_t */
    net_iface_cnf_t net_sta;
    net_iface_cnf_t net_ap;
    httpd_handle_t web_server;
    httpd_handle_t ws_server;
    struct {
      uint32_t raw_sent_ts;
      struct app_uart_t {
        gw_uart_t desc;
        gw_uart_config_t cnf;
        void *proto_context; /* holds upstanding proto descriptor */
      } port[2];
    } uart;
} app_context_t;

void ws_server_init(void);
httpd_uri_t *ws_transport_httpd_init(char *uri);

/*==========================*/

bool load_config(app_context_t *app);
void save_wifi_sta_config(const char *ssid, const char *password);
void save_wifi_ap_config(const char *ssid, const char *password);
/* Per-device softAP credentials: "WG-<3 MAC bytes>" + password derived from the
 * MAC. CONFIG_WIFI_AP_DEFAULT_SSID/_PASS override the generated values when set. */
void wifi_ap_credentials_default(char *ssid, size_t ssid_size, char *pass, size_t pass_size);
/* Empty password means an open network; WPA2 needs 8..63 characters. */
uint8_t wifi_password_valid(const char *pass);
/* Number of stations currently associated with the softAP (0 when AP is off). */
uint8_t wifi_ap_clients_count(void);
/* Applies the IPv4 settings of both interfaces (station and access point).
 * Called once after Wi-Fi init and whenever the configuration page changes
 * them; falls back to DHCP/esp_netif defaults where nothing is configured. */
void net_apply_config(const app_context_t *app);
/* Persists the IPv4 settings in NVS */
void net_save_config(const app_context_t *app);
/* Parses "a.b.c.d" into network byte order; 0 on success */
uint8_t net_parse_ip4(const char *str, uint32_t *out);
/* Formats an address into out, "—" when unset */
void net_format_ip4(uint32_t ip, char *out, size_t out_size);

/*==========================*/

/* FID — текстовое имя "<подсистема>.<действие>", а не код: имя видно в кадре
 * клиента и в ответе, и по нему же вызов маршрутизируется. Один и тот же вызов
 * используется и как одноразовый запрос, и как подписка (FLAGS:4). */
#define ESP_WS_API_ECHO_ID           "echo"
/* Device configuration API (see apih_sys.c and config_page.html):
 *   wifi.ap    GET/SET AP credentials — a write is accepted only while exactly
 *              one station is associated with the softAP (see below)
 *   sys.info   GET device status: MACs, IPs, AP client count, heap, filesystem
 *   sys.reboot reboot the gateway after the response has been delivered
 *   sys.files  GET LittleFS file listing
 *   sys.file.write  PUT a file in chunks: PATH, OFFSET, DATA (base64), LAST —
 *              written to a temporary file and renamed over the target on LAST
 *   sys.file.delete DELETE a file: PATH
 *   sys.file.rename RENAME a file: FROM, TO (refused when TO already exists)
 *   sys.net    GET/SET IPv4 settings of the station and access point interfaces
 *              (DHCP or static address, mask, gateway, DNS)
 *
 * Файловый менеджер (sys.files и sys.file.*) оказывается только точке доступа:
 * раньше это была та же граница у HTTP-методов /upload/…, /delete/…, /rename/…,
 * которые эти сервисы заменили. */
#define ESP_WS_API_SYS_WIFI_STA      "wifi.sta"
#define ESP_WS_API_SYS_WIFI_AP       "wifi.ap"
#define ESP_WS_API_SYS_INFO          "sys.info"
#define ESP_WS_API_SYS_REBOOT        "sys.reboot"
#define ESP_WS_API_SYS_FILES         "sys.files"
#define ESP_WS_API_SYS_FILE_WRITE    "sys.file.write"
#define ESP_WS_API_SYS_FILE_DELETE   "sys.file.delete"
#define ESP_WS_API_SYS_FILE_RENAME   "sys.file.rename"
#define ESP_WS_API_SYS_FILE_MKDIR    "sys.file.mkdir"
#define ESP_WS_API_SYS_FILE_RMDIR    "sys.file.rmdir"
#define ESP_WS_API_SYS_NET           "sys.net"
/* Registers the system FID queue and starts the task serving it */
esp_err_t api_sys_run(app_context_t *app);

#define ESP_WS_API_UART1_CNF         "uart1.config"
#define ESP_WS_API_UART1_RAW_RX      "uart1.rx"
#define ESP_WS_API_UART1_RAW_TX      "uart1.tx"
#define ESP_WS_API_UART1_ECHO        "uart1.echo"
#define ESP_WS_API_UART2_CNF         "uart2.config"
#define ESP_WS_API_UART2_RAW_RX      "uart2.rx"
#define ESP_WS_API_UART2_RAW_TX      "uart2.tx"
#define ESP_WS_API_UART2_ECHO        "uart2.echo"
//TODO: move to apih_uart.h
esp_err_t ws_uart_run(app_context_t *app);

#define ESP_WS_API_UART1_MODBUS_REQ  "uart1.modbus"
#define ESP_WS_API_UART2_MODBUS_REQ  "uart2.modbus"
esp_err_t ws_modbus_run(app_context_t *app);

void *get_ws_worker_queue(void);

#ifdef __cplusplus
}
#endif
