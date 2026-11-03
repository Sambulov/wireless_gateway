#pragma once
#include "CodeLib.h"
#include "uart.h"

#ifdef __cplusplus
extern "C" {
#endif

void             gw_uart_stub_reset(void);
gw_uart_config_t gw_uart_stub_last_config(void *desc);
int              gw_uart_stub_write_calls(void *desc);
int              gw_uart_stub_last_write_len(void *desc);
const uint8_t   *gw_uart_stub_last_write_buf(void *desc);
/* Драйвер «потерял» принятое: следующий take_rx_loss этого порта вернёт 1. */
void             gw_uart_stub_set_rx_loss(void *desc);

#ifdef __cplusplus
}
#endif
