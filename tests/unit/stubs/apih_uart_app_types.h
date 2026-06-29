#pragma once
/* Shadow of main/app.h — host unit tests only. Provides only what
 * apih_uart.c and other testable units actually need, without any
 * ESP-IDF or FreeRTOS headers. */

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include "CodeLib.h"
#include "uart.h"
#include "web_api.h"
#include "ws_hal.h"

typedef int esp_err_t;
#define ESP_OK   0
#define ESP_FAIL (-1)

#define TAG_S(x)  #x
#define TAG_SX(x) TAG_S(x)
#define TAG  TAG_SX(__FILE__) " " TAG_SX(__LINE__)

/* FID constants (kept in sync with the real app.h) */
#define ESP_WS_API_UART1_CNF         0x1010
#define ESP_WS_API_UART1_RAW_RX      0x1011
#define ESP_WS_API_UART1_RAW_TX      0x1012
#define ESP_WS_API_UART1_ECHO        0x1013
#define ESP_WS_API_UART2_CNF         0x1020
#define ESP_WS_API_UART2_RAW_RX      0x1021
#define ESP_WS_API_UART2_RAW_TX      0x1022
#define ESP_WS_API_UART2_ECHO        0x1023

typedef struct {
    struct {
        uint32_t raw_sent_ts;
        struct app_uart_t {
            gw_uart_t        desc;
            gw_uart_config_t cnf;
            void            *proto_context;
        } port[2];
    } uart;
} app_context_t;

void *get_ws_worker_queue(void);
