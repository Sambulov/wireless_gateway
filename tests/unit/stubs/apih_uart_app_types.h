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

typedef int esp_err_t;
#define ESP_OK   0
#define ESP_FAIL (-1)

#define TAG_S(x)  #x
#define TAG_SX(x) TAG_S(x)
#define TAG  TAG_SX(__FILE__) " " TAG_SX(__LINE__)

/* FID names (kept in sync with the real app.h). Нумерация аппаратная:
 * uart0 — консоль, uart1 и uart2 — порты платы. */
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
#define ESP_WS_API_UART0_MODBUS_REQ  "uart0.modbus"
#define ESP_WS_API_UART1_MODBUS_REQ  "uart1.modbus"
#define ESP_WS_API_UART2_MODBUS_REQ  "uart2.modbus"

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
