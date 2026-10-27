#pragma once

#define UART_MAX_SPEED        500000

/* Режим работы порта. Различает их только направление передачи: в RS-485 приёмник
 * и передатчик сидят на одной паре, поэтому направлением управляет драйвер (RTS). */
typedef enum {
  GW_UART_MODE_UART              = 0, /* обычный UART: RX и TX независимы */
  GW_UART_MODE_RS485_HALF_DUPLEX = 1, /* RS-485 с автонаправлением (глушит драйвер) */
  /* Однопроводный UART: RX и TX на одном проводе, направление не переключается,
   * поэтому своё переданное возвращается эхом. Глушим сами: пока идёт передача и
   * ещё короткое время после неё, принятое в приём не попадает. */
  GW_UART_MODE_UART_SINGLE_WIRE  = 2
} gw_uart_mode_t;

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
  GW_UART_PORT_1 = 1,
  GW_UART_PORT_2 = 2,
} gw_uart_port_t;

#include "board_config.h"

#define GW_UART_PRIVATE(size)    uint32_t __private[(size + sizeof(void *) - 1) >> 2]

#define GW_UART_PRIVATE_SIZE     48

typedef struct {
  GW_UART_PRIVATE(GW_UART_PRIVATE_SIZE);
} gw_uart_t;

typedef struct {
  gw_uart_word_t bits;
  uint32_t boud;
  gw_uart_parity_t parity;
  gw_uart_stop_bits_t stop;
  gw_uart_mode_t mode;
  /* Сколько знаков (10 бит) линия должна молчать, чтобы принятое считалось
   * законченным кадром: это порог таймаута приёма драйвера, по нему приходит
   * b_end, и по нему же клиент делит поток на кадры (как t3.5 в Modbus RTU). */
  uint8_t idle_ch;
} gw_uart_config_t;

/* Умолчание порога простоя: 4 знака — чуть больше modbus'овских 3.5. */
#define GW_UART_IDLE_CH_DEFAULT   4

/* Кольца драйвера порта — КУЧА, выделяются на каждый порт сразу и навсегда,
 * поэтому размер выбран по скорости линии, а не «побольше»: приём 2048 — почти
 * два кадра по 115200 бод (864 Б за кадр), передача 2048 — любая посылка
 * сервиса влезает целиком, запись не ждёт линию. */
#define GW_UART_RX_RING   2048
#define GW_UART_TX_RING   2048

extern const gw_uart_config_t gw_uart_config_default;

typedef struct {
  uint8_t *buf;
  uint32_t size;
  /* Признак конца кадра: драйвер выставляет его, когда после принятых байт
   * наступила пауза (таймаут приёма). */
  uint8_t b_end;
  /* Простой линии ПЕРЕД этими байтами, мкс; ноль — кусок продолжает предыдущий.
   * Точность ограничена таймаутом приёма драйвера («простой + таймаут»), а
   * достоверный признак границы кадра — b_end. */
  uint32_t gap_us;
} gw_uart_event_data_t;

uint8_t gw_uart_init(void *desc, gw_uart_port_t port, uint32_t buffer_size);

int32_t gw_uart_read(void *desc, uint8_t *buf, uint16_t size);
int32_t gw_uart_write(void *desc, const uint8_t *buf, uint16_t size);
uint8_t gw_uart_set(void *desc, const gw_uart_config_t *cnf);
uint8_t gw_uart_get(void *desc, gw_uart_config_t *out_cnf);
int32_t gw_uart_available_write(void *desc);

void gw_uart_on_receive_subscribe(void *desc, delegate_t *delegate);

void gw_uart_set_echo(void *desc, uint8_t enabled);
uint8_t gw_uart_get_echo(void *desc);

void gw_uart_lock_rx(void *desc);
void gw_uart_unlock_rx(void *desc);
