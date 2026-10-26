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
/* Объект веб-сервера интерфейса: приложение поднимает его на каждый интерфейс и
 * регистрирует в нём то, что этому интерфейсу оказано. */
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

/* Политика «какой сервис каким интерфейсам оказан» применяется при регистрации
 * URI в объекте веб-сервера (web_server.c, таблица — web_iface_policy.c):
 * обработчик сервиса, оказанного не всем, отвечает 404 соединению с
 * неподходящего интерфейса. */

/* Файлы читаются по HTTP GET — общим обработчиком-ловушкой, а меняются только
 * через WebSocket-сервисы sys.file.*. Прошивка по HTTP больше не принимается:
 * её принимает служба sys.fw.* (apih_fw.c). */
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
    /* Точка доступа, которую выбрал пользователь (BSSID конкретного радио).
     * Все нули — не выбирал: подключаемся к любой точке с этим именем, выбирая
     * сильнейшую. Адрес привязан к имени сети: сменилось имя — привязка не
     * действует. */
    uint8_t sta_bssid[6];
    /* MAC адреса интерфейсов. mac_sta/mac_ap — то, что ЗАКАЗАЛ пользователь
     * (все нули — «как с завода»); mac_sta_factory/mac_ap_factory — заводские
     * значения из eFuse, прочитанные до первого esp_wifi_set_mac. Драйвер
     * принимает новый MAC только на остановленном интерфейсе, поэтому смена
     * действует со следующей перезагрузки (см. net_wifi_init). */
    uint8_t mac_sta[6];
    uint8_t mac_ap[6];
    uint8_t mac_sta_factory[6];
    uint8_t mac_ap_factory[6];
    /* Веб-серверы интерфейсов: объект на интерфейс, по одному на точку доступа и
     * станцию. Сокетный слой (web_httpd.c) у них общий — httpd на порт. */
    web_server_t *web_ap;
    web_server_t *web_sta;
#ifdef CONFIG_QEMU_BUILD
    /* В сборке для QEMU единственный сетевой интерфейс — проводной. */
    web_server_t *web_eth;
#endif
    /* Ядра API этого приложения — по одному на транспорт (точка доступа и
     * станция). Дескрипторы заводит app_main, а сервисы регистрируют на
     * КОНКРЕТНОМ ядре свои обработчики FID: запрос приходит вызовом обработчика,
     * ответ уходит из обработчика через дескриптор вызова (api_reply и
     * api_broadcast) — очередей у ядра нет. */
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
/* Персональный MAC интерфейса: mac == NULL (или все нули) — вернуть заводской,
 * ключ в NVS стирается. Значение проверяется по тем же правилам, что и в
 * драйвере: первый байт без бита групповой адресации, не все нули. */
void save_wifi_mac_config(uint8_t ap, const uint8_t *mac);
uint8_t wifi_mac_valid(const uint8_t *mac);
/* Per-device softAP credentials: "WG-<3 MAC bytes>" + password derived from the
 * MAC. CONFIG_WIFI_AP_DEFAULT_SSID/_PASS override the generated values when set. */
void wifi_ap_credentials_default(char *ssid, size_t ssid_size, char *pass, size_t pass_size);
/* Empty password means an open network; WPA2 needs 8..63 characters. */
uint8_t wifi_password_valid(const char *pass);
/* Поднимает сеть прибора в порядке, заданном сборкой (connection.c): учёт
 * соединений и скан подписываются на события драйвера, затем Wi-Fi (в сборке для
 * QEMU — проводной интерфейс), затем адреса интерфейсов. */
void net_init(app_context_t *app);
/* Applies the IPv4 settings of both interfaces (station and access point).
 * Called once after Wi-Fi init and whenever the configuration page changes
 * them; falls back to DHCP/esp_netif defaults where nothing is configured. */
void net_apply_config(const app_context_t *app);
/* Persists the IPv4 settings in NVS */
void net_save_config(const app_context_t *app);

/*==========================*/

/* FID — текстовое имя "<подсистема>.<действие>", а не код: имя видно в кадре
 * клиента и в ответе, и по нему же вызов маршрутизируется. Один и тот же вызов
 * используется и как одноразовый запрос, и как подписка (FLAGS:4). */
#define ESP_WS_API_ECHO_ID           "echo"
/* Device configuration API (see apih_sys.c and config_page.html):
 *   wifi.ap    GET/SET AP credentials — a write is accepted only while exactly
 *              one station is associated with the softAP (see below)
 *   sys.info   GET device status: MACs, IPs, AP client count, heap, filesystem
 *   sys.reboot reboot the gateway after the response has been delivered; the
 *              HTTP policy allows it from any interface, so the station core
 *              registers it with a writable context too
 *   sys.files  GET LittleFS file listing
 *   sys.file.write  PUT a file in chunks: PATH, OFFSET, DATA (base64), LAST —
 *              written to a temporary file and renamed over the target on LAST
 *   sys.file.delete DELETE a file: PATH
 *   sys.file.rename RENAME a file: FROM, TO (refused when TO already exists)
 *   sys.net    GET/SET IPv4 settings of the station and access point interfaces
 *              (DHCP or static address, mask, gateway, DNS)
 *   wifi.mac   GET/SET the MAC of one interface (IF = STA|AP, MAC = address or
 *              "factory"): the driver takes a new MAC only while the interface is
 *              stopped, so the change is stored in NVS and applied on reboot
 *   wifi.scan  GET the list of networks around (SSID, RSSI, channel, security);
 *              a helper for filling the SSID field — credentials are still typed
 *              by hand
 *   sys.fw.begin  PUT start a firmware upload: SIZE (bytes) — the next OTA
 *              partition is erased and an OTA session is opened; the answer
 *              carries SESSION:1
 *   sys.fw.write  PUT one chunk: SEQ (strictly the next chunk number) and DATA
 *              (base64); the answer carries the number of bytes accepted so far
 *              (WRITTEN). The call stays open until the next chunk is asked for,
 *              which is how the service detects a stalled upload
 *   sys.fw.end    PUT verify and boot: MD5 (optional, 32 hex digits of the image
 *              itself), then esp_ota_end + set_boot_partition + reboot
 *   sys.fw.abort  PUT drop the upload opened by sys.fw.begin
 *   Прошивка (sys.fw.*) зарегистрирована ТОЛЬКО на AP-ядре (apih_fw.c), и
 *   порция принимается, только пока на точке доступа ровно один клиент.
 *
 * Сервисы регистрируются на КОНКРЕТНОМ ядре (см. api.h): «только точка доступа»
 * — это регистрация на AP-ядре, а не маска интерфейсов. Файловый менеджер
 * (sys.files и sys.file.*) и echo есть на обоих ядрах одинаково — они доступны
 * с любого интерфейса. */
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
/* Регистрирует обработчики системных FID на обоих ядрах (задачи у службы нет:
 * их зовёт такт ядра, который крутит задача приложения) */
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
