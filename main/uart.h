#pragma once

typedef enum {
  GW_UART_WORD_7BIT = 0,
  GW_UART_WORD_8BIT = 1,
  GW_UART_WORD_9BIT = 2 /* unsupported */
} gw_uart_word_t;

typedef enum {
  GW_UART_PARITY_NONE = 0,
  GW_UART_PARITY_ODD  = 1,
  GW_UART_PARITY_EVEN = 2
} gw_uart_parity_t;

typedef enum {
  GW_UART_STOP_BITS1      = 0,
  GW_UART_STOP_BITS2      = 1,
  GW_UART_STOP_BITS0_5    = 2, /* unsupported */
  GW_UART_STOP_BITS1_5    = 3
} gw_uart_stop_bits_t;

typedef enum {
  GW_UART_PORT_0 = 0,
  GW_UART_PORT_1 = 1, /* unsupported */
  GW_UART_PORT_2 = 2,
} gw_uart_port_t;

#define GW_UART_PRIVATE(size)    uint32_t __private[(size + sizeof(void *) - 1) >> 2]

#define GW_UART_PRIVATE_SIZE     8

typedef struct {
  GW_UART_PRIVATE(GW_UART_PRIVATE_SIZE);
} gw_uart_t;

typedef struct {
  gw_uart_word_t bits;
  uint32_t boud;
  gw_uart_parity_t parity;
  gw_uart_stop_bits_t stop;
} gw_uart_config_t;

extern const gw_uart_config_t gw_uart_config_default;

uint8_t gw_uart_init(void *desc, gw_uart_port_t port, uint32_t buffer_size);
int32_t gw_uart_read(void *desc, uint8_t *buf, uint16_t size);
int32_t gw_uart_write(void *desc, const uint8_t *buf, uint16_t size);
uint8_t gw_uart_set(void *desc, const gw_uart_config_t *cnf);

