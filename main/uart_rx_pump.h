#pragma once

#include <stdint.h>
#include <stddef.h>

/* Выборка принятого из драйвера порта кусками и раздача их подписчику.
 *
 * Логика без драйвера (правило 12, Humble Object): задача событий UART только
 * зовёт uart_rx_pump_run с размером из события, а чтение, отброс эха
 * однопроводной линии, эхо порта и раздача приходят через ops.
 *
 * Буфер куска — В ОБЪЕКТЕ, по одному объекту на порт. Задачи приёма портов
 * работают параллельно (на двух ядрах), и общий буфер давал кросс-ток: кусок
 * одного порта доставлялся подписчику другого. */

#define UART_RX_PUMP_CHUNK 128

typedef struct {
    /* Прочитать до size байт без ожидания. <= 0 — читать нечего. */
    int32_t  (*read)(void *ctx, uint8_t *buf, uint32_t size);
    /* Простой линии перед этим куском, мкс (см. gw_uart_event_data_t::gap_us).
     * NULL — 0. */
    uint32_t (*gap_take)(void *ctx);
    /* Сколько байт с начала куска — своё же эхо (однопроводная линия). NULL — 0. */
    uint32_t (*echo_skip)(void *ctx, uint32_t size);
    /* Эхо порта: вернуть принятое в линию. NULL — эха нет. */
    void     (*echo_write)(void *ctx, const uint8_t *buf, uint32_t size);
    /* Кусок подписчику. Буфер живёт только до возврата. Обязателен, как и read. */
    void     (*deliver)(void *ctx, const uint8_t *buf, uint32_t size,
                        uint8_t b_end, uint32_t gap_us);
} uart_rx_pump_ops_t;

/* Исключение из правила 7: поля открыты только ради статического размещения
 * по порту (без кучи); снаружи их не трогают. */
typedef struct {
    const uart_rx_pump_ops_t *ops;
    void *ctx;
    uint8_t buf[UART_RX_PUMP_CHUNK];
} uart_rx_pump_t;

void uart_rx_pump_init(uart_rx_pump_t *p, const uart_rx_pump_ops_t *ops, void *ctx);

/* Забрать size байт, о которых сообщил драйвер, и раздать кусками. */
void uart_rx_pump_run(uart_rx_pump_t *p, size_t size, uint8_t b_end);
