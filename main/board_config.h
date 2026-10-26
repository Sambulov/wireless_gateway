#pragma once

/* ── Конфигурация платы ─────────────────────────────────────────────────────
 *
 * Что на этой плате к каким выводам припаяно. Меняется только здесь: код
 * драйверов и служб о распиновке ничего не знает.
 *
 * Нумерация портов — АППАРАТНАЯ: uart0 (консоль), uart1, uart2 (RS-485).
 * Она же видна снаружи в именах FID (uart0.config, uart2.rx и т.д.).
 *
 * Годятся не любые выводы ESP32: 6..11 заняты SPI-flash, 34..39 работают
 * только на вход (годится под RX, но не под TX/RTS), выводы 0/2/5/12/15 —
 * strapping (уровень на них при старте влияет на режим загрузки).
 *
 * RS-485: направление передачи задаёт RTS (он же DE модуля). Если у модуля
 * автонаправление по линии TX, пин RTS можно не назначать (GW_UART_PIN_NONE).
 */

/* Пин не назначен. */
#define GW_UART_PIN_NONE   (-1)

/* UART0 — консоль (лог и загрузчик). */
#define BOARD_UART0_TX    1
#define BOARD_UART0_RX    3
#define BOARD_UART0_RTS   22

/* UART1 — свободный порт платы. */
#define BOARD_UART1_TX    19
#define BOARD_UART1_RX    18
#define BOARD_UART1_RTS   21

/* UART2 — наш RS-485 (INNOVERT ISD222M21E). */
#define BOARD_UART2_TX    17
#define BOARD_UART2_RX    16
#define BOARD_UART2_RTS   23

/* Порядок в таблицах — по номеру порта, поэтому менять его нельзя. */
#define BOARD_UART_TX(port)  ((port) == 0 ? BOARD_UART0_TX  \
                            : (port) == 1 ? BOARD_UART1_TX  : BOARD_UART2_TX)
#define BOARD_UART_RX(port)  ((port) == 0 ? BOARD_UART0_RX  \
                            : (port) == 1 ? BOARD_UART1_RX  : BOARD_UART2_RX)
#define BOARD_UART_RTS(port) ((port) == 0 ? BOARD_UART0_RTS \
                            : (port) == 1 ? BOARD_UART1_RTS : BOARD_UART2_RTS)
