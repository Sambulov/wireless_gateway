#pragma once
/* Shadow of main/app.h — host unit tests only. Provides only what
 * apih_uart.c and other testable units actually need, without any
 * ESP-IDF or FreeRTOS headers. */

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include "CodeLib.h"
#include "uart.h"
#include "api.h"
#include "platform.h"
/* esp_mac.h несёт только имена MAC и тип wifi_interface_t: их тянут службы
 * (apih_sys_common.c) и объявления apih_sys_internal.h. */
#include "esp_mac.h"

typedef int esp_err_t;
#define ESP_OK   0
#define ESP_FAIL (-1)

#define TAG_S(x)  #x
#define TAG_SX(x) TAG_S(x)
#define TAG  TAG_SX(__FILE__) " " TAG_SX(__LINE__)

/* Журнал ESP-IDF (esp_log.h) на хосте: имена те же, а пишет он в платформенный
 * стаб — тесты считают сообщения, а не читают их (system_stub.h). */
#define ESP_LOGE(tag, fmt, ...) platform_log_w(tag, fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) platform_log_w(tag, fmt, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) platform_log_i(tag, fmt, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) platform_log_d(tag, fmt, ##__VA_ARGS__)

/* FID names (kept in sync with the real app.h) */
#define ESP_WS_API_UART1_CNF         "uart1.config"
#define ESP_WS_API_UART1_RAW_RX      "uart1.rx"
#define ESP_WS_API_UART1_RAW_TX      "uart1.tx"
#define ESP_WS_API_UART1_ECHO        "uart1.echo"
#define ESP_WS_API_UART2_CNF         "uart2.config"
#define ESP_WS_API_UART2_RAW_RX      "uart2.rx"
#define ESP_WS_API_UART2_RAW_TX      "uart2.tx"
#define ESP_WS_API_UART2_ECHO        "uart2.echo"
#define ESP_WS_API_SYS_FW_BEGIN      "sys.fw.begin"
#define ESP_WS_API_SYS_FW_WRITE      "sys.fw.write"
#define ESP_WS_API_SYS_FW_END        "sys.fw.end"
#define ESP_WS_API_SYS_FW_ABORT      "sys.fw.abort"

/* В прошивке объявление приходит из connection.h; в тестах — отсюда: служба
 * прошивки спрашивает, сколько станций на точке доступа (stubs/net_stub.c). */
uint8_t net_wifi_clients_count(void);

typedef struct {
    /* Ядра API теста: сервис берёт их отсюда, как и в прошивке (app.c).
     * Второе (станция) может быть NULL — тестам UART достаточно одного ядра. */
    api_t *api_ap;
    api_t *api_sta;
    struct {
        uint32_t raw_sent_ts;
        struct app_uart_t {
            gw_uart_t        desc;
            gw_uart_config_t cnf;
            void            *proto_context;
        } port[2];
    } uart;
} app_context_t;

/* Очередей у ядра нет: запрос приходит вызовом обработчика, ответ уходит по
 * дескриптору вызова, а рассылка — публичным вызовом api_broadcast (ядро
 * берётся из app.api_ap/app.api_sta). */
