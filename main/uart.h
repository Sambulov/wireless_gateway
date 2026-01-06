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
  GW_UART_STOP_BITS0_5    = 0, /* unsupported */
  GW_UART_STOP_BITS1      = 1,
  GW_UART_STOP_BITS2      = 2,
  GW_UART_STOP_BITS1_5    = 3
} gw_uart_stop_bits_t;

typedef enum {
  GW_UART_PORT_0 = 0,
  GW_UART_PORT_1 = 1, /* unsupported */
  GW_UART_PORT_2 = 2,
} gw_uart_port_t;

#define GW_UART_PRIVATE(size)    uint32_t __private[(size + sizeof(void *) - 1) >> 2]

#define GW_UART_PRIVATE_SIZE     44

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

typedef struct {
  uint8_t *buf;
  uint32_t size;
} gw_uart_event_data_t;

uint8_t gw_uart_init(void *desc, gw_uart_port_t port, uint32_t buffer_size);
int32_t gw_uart_write(void *desc, const uint8_t *buf, uint16_t size);
uint8_t gw_uart_set(void *desc, const gw_uart_config_t *cnf);
uint8_t gw_uart_get(void *desc, gw_uart_config_t *out_cnf);
int32_t gw_uart_available_write(void *desc);

void gw_uart_on_receive_subscribe(void *desc, delegate_t *delegate);
