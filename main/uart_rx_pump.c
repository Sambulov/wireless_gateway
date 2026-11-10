#include "uart_rx_pump.h"

void uart_rx_pump_init(uart_rx_pump_t *p, const uart_rx_pump_ops_t *ops, void *ctx)
{
    p->ops = ops;
    p->ctx = ctx;
}

void uart_rx_pump_run(uart_rx_pump_t *p, size_t size, uint8_t b_end)
{
    const uart_rx_pump_ops_t *ops = p->ops;

    while (size) {
        uint32_t want = (size < UART_RX_PUMP_CHUNK) ? (uint32_t)size : UART_RX_PUMP_CHUNK;
        int32_t sz = ops->read(p->ctx, p->buf, want);
        /* 0 — FIFO на миг пуст, хотя событие обещало данные: без выхода цикл
         * крутился бы вечно (см. docs/uart_sz_trouble.txt). */
        if (sz <= 0)
            break;
        /* Простой перед куском — по нему клиент видит, где кончился
         * предыдущий кадр. Считаем ДО отбрасывания эха. */
        uint32_t gap = ops->gap_take ? ops->gap_take(p->ctx) : 0;
        /* Однопроводная линия: начало куска — своё же переданное. */
        uint32_t skip = ops->echo_skip ? ops->echo_skip(p->ctx, (uint32_t)sz) : 0;
        size -= (size_t)sz;
        if (skip >= (uint32_t)sz)
            continue;
        const uint8_t *data = p->buf + skip;
        uint32_t len = (uint32_t)sz - skip;
        if (ops->echo_write)
            ops->echo_write(p->ctx, data, len);
        ops->deliver(p->ctx, data, len, b_end, gap);
    }
}
