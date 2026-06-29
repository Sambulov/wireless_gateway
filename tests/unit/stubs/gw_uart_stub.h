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

#ifdef __cplusplus
}
#endif
