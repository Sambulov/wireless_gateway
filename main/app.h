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


#include "api.h"

#include <stdio.h>


#include "esp_littlefs.h"
#include "esp_http_server.h"

#include "uart.h"
#include "connection.h"
#include "web_server.h"
#include "ws_transport.h"

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

/* IPv4 configuration of one interface. Addresses are in network byte order;
 * 0 means "not set" — falls back to the lwIP/esp_netif default. */
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

/* Политика «какой сервис каким интерфейсам оказан» (web_iface_policy.c)
 * применяется при регистрации URI: сервис, оказанный не всем, отвечает 404. */

/* Файлы читаются по HTTP GET общим обработчиком-ловушкой, а меняются только
 * через WebSocket-сервисы sys.file.*; прошивку принимает sys.fw.* (apih_fw.c). */
extern httpd_uri_t file_server;
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
    /* Точка доступа, которую выбрал пользователь. Все нули — не выбирал:
     * подключаемся к любой точке с этим именем, выбирая сильнейшую. Привязка
     * действует только для того же имени сети. */
    uint8_t sta_bssid[6];
    /* mac_sta/mac_ap — то, что ЗАКАЗАЛ пользователь (нули — «как с завода»);
     * *_factory — заводские из eFuse, снятые до первого esp_wifi_set_mac.
     * Драйвер принимает новый MAC только на остановленном интерфейсе, поэтому
     * смена действует со следующей перезагрузки (см. net_wifi_init). */
    uint8_t mac_sta[6];
    uint8_t mac_ap[6];
    uint8_t mac_sta_factory[6];
    uint8_t mac_ap_factory[6];
    /* Веб-серверы интерфейсов: свой на точку доступа и на станцию. Сокетный
     * слой (web_httpd.c) у них общий — httpd на порт. */
    web_server_t *web_ap;
    web_server_t *web_sta;
#ifdef CONFIG_QEMU_BUILD
    /* В сборке для QEMU единственный сетевой интерфейс — проводной. */
    web_server_t *web_eth;
#endif
    /* Ядра API — по одному на транспорт. Сервисы регистрируют свои методы на
     * КОНКРЕТНОМ ядре: запрос приходит вызовом обработчика, ответ уходит через
     * дескриптор вызова (api_call_send_arg и api_call_group_fid_data) — очередей
     * у ядра нет. */
    api_t *api_ap;
    api_t *api_sta;
    struct {
      uint32_t raw_sent_ts;
      struct app_uart_t {
        gw_uart_t desc;
        gw_uart_config_t cnf;
        void *proto_context; /* holds upstanding proto descriptor */
      } port[3];
    } uart;
} app_context_t;

/*==========================*/

bool load_config(app_context_t *app);
void save_wifi_sta_config(const char *ssid, const char *password);
/* BSSID выбранной точки (6 байт); NULL или нули — «любая точка с этим именем» */
void save_wifi_sta_bssid(const uint8_t *bssid);
void save_wifi_ap_config(const char *ssid, const char *password);
/* Персональный MAC интерфейса: mac == NULL или нули — вернуть заводской, ключ в
 * NVS стирается. Проверка как в драйвере: первый байт без бита групповой
 * адресации, не все нули. */
void save_wifi_mac_config(uint8_t ap, const uint8_t *mac);
uint8_t wifi_mac_valid(const uint8_t *mac);
/* Per-device softAP credentials: "WG-<3 MAC bytes>" + password derived from the
 * MAC. CONFIG_WIFI_AP_DEFAULT_SSID/_PASS override the generated values when set. */
void wifi_ap_credentials_default(char *ssid, size_t ssid_size, char *pass, size_t pass_size);
/* Empty password means an open network; WPA2 needs 8..63 characters. */
uint8_t wifi_password_valid(const char *pass);
/* Поднимает сеть прибора в порядке, заданном сборкой (connection.c): подписки
 * на события драйвера, затем Wi-Fi (в QEMU — проводной), затем адреса. */
void net_init(app_context_t *app);
/* Applies the IPv4 settings of both interfaces: called after Wi-Fi init and on
 * every change; falls back to DHCP/esp_netif defaults where nothing is set. */
void net_apply_config(const app_context_t *app);
void net_save_config(const app_context_t *app);

/*==========================*/

/* FID — текстовое имя "<подсистема>.<действие>", а не код: имя видно в кадре
 * клиента и в ответе, и по нему же вызов маршрутизируется. Один и тот же метод
 * обслуживает и разовый вызов, и подписку — разницу задаёт обработчик, вернув
 * HDL_TRUE. */
#define ESP_WS_API_ECHO_ID           "echo"
/* Device configuration API (see apih_sys.c and config_page.html):
 *   wifi.ap    GET/SET AP credentials — a write is accepted only while exactly
 *              one station is associated with the softAP
 *   sys.info   GET device status: MACs, IPs, AP client count, heap, filesystem
 *   sys.reboot reboot after the response has been delivered; allowed from any
 *              interface, so the station core registers it writable too
 *   sys.files  GET LittleFS file listing
 *   sys.file.write  PATH, OFFSET, DATA (base64), LAST — written to a temporary
 *              file and renamed over the target on LAST
 *   sys.file.delete PATH
 *   sys.file.rename FROM, TO (refused when TO already exists)
 *   sys.net    GET/SET IPv4 settings of the station and access point
 *   wifi.mac   IF = STA|AP, MAC = address or "factory": the driver takes a new
 *              MAC only while the interface is stopped, so it applies on reboot
 *   wifi.scan  GET the list of networks around (SSID, RSSI, channel, security)
 *   sys.fw.begin  SIZE (bytes) — the next OTA partition is erased, the answer
 *              carries SESSION:1
 *   sys.fw.write  SEQ (strictly the next chunk number) and DATA (base64); the
 *              answer carries WRITTEN. The call stays open until the next chunk
 *              is asked for, which is how a stalled upload is detected
 *   sys.fw.end    MD5 (optional, 32 hex digits of the image), then esp_ota_end +
 *              set_boot_partition + reboot
 *   sys.fw.abort  drop the upload opened by sys.fw.begin
 *
 * Прошивка (sys.fw.*) зарегистрирована ТОЛЬКО на AP-ядре (apih_fw.c): «только
 * точка доступа» — это регистрация на AP-ядре, а не маска интерфейсов. */
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
#define ESP_WS_API_SYS_FW_BEGIN      "sys.fw.begin"
#define ESP_WS_API_SYS_FW_WRITE      "sys.fw.write"
#define ESP_WS_API_SYS_FW_END        "sys.fw.end"
#define ESP_WS_API_SYS_FW_ABORT      "sys.fw.abort"
#define ESP_WS_API_SYS_NET           "sys.net"
#define ESP_WS_API_SYS_WIFI_MAC      "wifi.mac"
#define ESP_WS_API_SYS_WIFI_SCAN     "wifi.scan"
/* Регистрирует обработчики системных FID на обоих ядрах: задачи у службы нет,
 * их зовёт такт ядра. */
esp_err_t api_sys_run(app_context_t *app);

#define ESP_WS_API_UART0_CNF         "uart0.config"
#define ESP_WS_API_UART0_RAW_RX      "uart0.rx"
#define ESP_WS_API_UART0_RAW_TX      "uart0.tx"
#define ESP_WS_API_UART0_ECHO        "uart0.echo"
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

#define ESP_WS_API_UART0_MODBUS_REQ  "uart0.modbus"
#define ESP_WS_API_UART1_MODBUS_REQ  "uart1.modbus"
#define ESP_WS_API_UART2_MODBUS_REQ  "uart2.modbus"
esp_err_t ws_modbus_run(app_context_t *app);

#ifdef __cplusplus
}
#endif
