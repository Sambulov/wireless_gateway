#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_intr_alloc.h"
#include "esp_timer.h"
#include "app.h"
#include "uart.h"

#define GW_LOG_MODULE GW_LOG_UART
#include "gw_log.h"

typedef struct {
    QueueHandle_t uart_queue;
    int port; // UART_NUM_0, UART_NUM_1, UART_NUM_2
    uart_config_t cnf;
    event_t on_receive;
    uint8_t echo_enabled;
    uint8_t mode;                 /* gw_uart_mode_t: режим работы порта */
    volatile uint8_t rx_locked;
    /* 1 — драйвер потерял принятое (переполнение FIFO/кольца). Флаг снимает
     * потребитель приёма и отдаёт эту потерю клиенту меткой, чтобы поток не
     * выглядел ровным при дыре в нём. */
    volatile uint8_t rx_lost;
} gw_uart_private_t;

#define ASSERRT_STRUCTURE_CAST(private_type, public_type, prv_size_def, def_file)   _Static_assert(sizeof(private_type) == sizeof(public_type), "In "def_file" data structure size of "#public_type" doesn't match, check "#prv_size_def)

ASSERRT_STRUCTURE_CAST(gw_uart_private_t, gw_uart_t, GW_UART_PRIVATE_SIZE, "uart.h");

/* ── Эхо однопроводной линии ────────────────────────────────────────────────
 *
 * В однопроводном режиме всё переданное возвращается в свой же приёмник, и это
 * эхо надо отбросить — ровно столько байт, сколько передали. Считаем по числу
 * байт, а не по времени: событие приёма приходит НЕ вместе с байтом, драйвер
 * отдаёт UART_DATA по таймауту приёма, то есть позже последнего байта.
 *
 * Ожидание ограничено: если за GIVE_UP_US после расчётного конца передачи эхо не
 * пришло, линия не однопроводная — счётчик сбрасывается, чтобы не съесть чужой
 * ответ. Ответ собеседника приходит всегда ПОСЛЕ эха, поэтому отбрасывается
 * начало, а не середина.
 *
 * Счётчики — на порт и через атомарные операции: пишет задача API, читает
 * задача приёма. */
#define UART_ECHO_GIVE_UP_US   50000

static volatile uint32_t uart_echo_pending[3];
static volatile int64_t  uart_echo_deadline_us[3];

/* ── Простой линии ──────────────────────────────────────────────────────────
 *
 * Клиенту нужно знать, где поток принятого прерывается: по этому признаку
 * протокол страницы делит данные на кадры (как t3.5 в Modbus RTU). Признака два:
 * b_end от драйвера (линия молчала дольше порога) и измеренный gap_us (простой
 * перед куском). Порог задаётся настройкой порта (idle_ch, знаки) и переводится
 * в символьные периоды драйвера: uart_set_rx_timeout ждёт СИМВОЛЫ (~11 бит), а
 * знак у нас 10 бит — отсюда округление вверх. */
static uint8_t  uart_idle_chars[3];
static volatile int64_t uart_last_rx_us[3];

/* Простой перед этим куском и отметка времени прочтения. */
static uint32_t uart_gap_take(int port)
{
    int64_t now  = esp_timer_get_time();
    int64_t prev = uart_last_rx_us[port];
    uart_last_rx_us[port] = now;
    if (prev == 0) return 0;                    /* первое прочтение после старта */
    int64_t gap = now - prev;
    return (gap > 0 && gap < 3600000000LL) ? (uint32_t)gap : 0;
}

/* Знаки → символьные периоды драйвера (1..126). */
static uint8_t uart_idle_symbols(uint8_t chars)
{
    if (chars == 0) chars = GW_UART_IDLE_CH_DEFAULT;
    uint32_t sym = ((uint32_t)chars * 10u + 10u) / 11u;
    if (sym < 1) sym = 1;
    if (sym > 126) sym = 126;
    return (uint8_t)sym;
}

/* Сколько микросекунд занимает один знак (10 бит) на этой скорости. */
static uint32_t uart_char_us(uint32_t boud)
{
    return boud ? (uint32_t)((10ULL * 1000000ULL) / boud) : 87;
}

static uint8_t uart_single_wire(gw_uart_private_t *uart)
{
    return uart->mode == (uint8_t)GW_UART_MODE_UART_SINGLE_WIRE;
}

/* Заказ на отброс эха: столько байт вернётся от этой посылки. */
static void uart_arm_echo(int port, uint32_t boud, uint32_t size)
{
    int64_t deadline = esp_timer_get_time()
                     + (int64_t)size * uart_char_us(boud)
                     + UART_ECHO_GIVE_UP_US;
    __atomic_add_fetch(&uart_echo_pending[port], size, __ATOMIC_SEQ_CST);
    if (deadline > __atomic_load_n(&uart_echo_deadline_us[port], __ATOMIC_SEQ_CST))
        __atomic_store_n(&uart_echo_deadline_us[port], deadline, __ATOMIC_SEQ_CST);
}

/* Сколько байт из этого куска — эхо (их не отдаём в приём). */
static int uart_echo_skip(int port, int size)
{
    uint32_t pending = __atomic_load_n(&uart_echo_pending[port], __ATOMIC_SEQ_CST);

    if (pending == 0)
        return 0;
    if (esp_timer_get_time()
        > __atomic_load_n(&uart_echo_deadline_us[port], __ATOMIC_SEQ_CST)) {
        __atomic_store_n(&uart_echo_pending[port], 0u, __ATOMIC_SEQ_CST);
        return 0;
    }
    uint32_t drop = ((uint32_t)size < pending) ? (uint32_t)size : pending;
    __atomic_store_n(&uart_echo_pending[port], pending - drop, __ATOMIC_SEQ_CST);
    return (int)drop;
}

static void uart_event_task(void *pvParameters) {
    static uint8_t buf[128];
    uart_event_t event;
    gw_uart_private_t *uart = (gw_uart_private_t *)pvParameters;

    while (xQueueReceive(uart->uart_queue, (void *)&event, (TickType_t)portMAX_DELAY)) {
        switch (event.type) {
        /* данных приходит много больше прочих событий: обрабатывать быстро,
         * иначе очередь переполнится. */
        case UART_DATA: {
            if (uart->rx_locked) break;
            gw_uart_event_data_t ev_trig;
            ev_trig.buf = buf;
            ev_trig.b_end = event.timeout_flag ? 1 : 0;
            size_t size = event.size;
            while (size) {
                ev_trig.size = CL_MIN(size, 128);
                int sz = uart_read_bytes(uart->port, ev_trig.buf, ev_trig.size, 0);
                if(sz <= 0) break;
                ev_trig.size = sz;
                /* Простой перед куском — по нему клиент видит, где кончился
                 * предыдущий кадр. Считаем ДО отбрасывания эха. */
                ev_trig.gap_us = uart_gap_take(uart->port);
                /* Однопроводная линия: начало куска — своё же переданное.
                 * Отбрасываем ровно столько байт, сколько передали. */
                if (uart_single_wire(uart)) {
                    int skip = uart_echo_skip(uart->port, sz);
                    if (skip >= sz) {
                        size -= sz;
                        continue;
                    }
                    ev_trig.buf += skip;
                    ev_trig.size = sz - skip;
                }
                if(uart->echo_enabled) {
                    uart_write_bytes(uart->port, ev_trig.buf, ev_trig.size);
                }
                event_raise(&uart->on_receive, uart, &ev_trig);
                size -= sz;
            }
            break;
        }
        /* Переполнение приёма: кольцо драйвера уже потеряло байты, но что в нём
         * лежит — отдавать надо (uart_flush_input выбросил бы и это). Пишем не
         * на каждое событие: консоль синхронная (115200), печать тормозит задачу
         * приёма и усиливает переполнение. Считаем и пишем раз в секунду. */
        case UART_FIFO_OVF:
        case UART_BUFFER_FULL: {
            static uint32_t ovf_count[3];
            static uint32_t ovf_ms[3];
            uint8_t  p = (uint8_t)uart->port;
            uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
            uart->rx_lost = 1;          /* потерю отдадим клиенту меткой */
            ovf_count[p]++;
            if ((now - ovf_ms[p]) >= 1000) {
                ESP_LOGW(TAG, "UART%d: переполнение приёма (событий %lu за %lu мс): часть принятого потеряна",
                         (int)uart->port, (unsigned long)ovf_count[p],
                         (unsigned long)(now - ovf_ms[p]));
                ovf_count[p] = 0;
                ovf_ms[p] = now;
            }
            break;
        }
        case UART_BREAK:
            break;
        case UART_PARITY_ERR:
            break;
        case UART_FRAME_ERR:
            break;
        default:
            LOGI(TAG, "uart event type: %d", event.type);
            break;
        }
    }
    vTaskDelete(NULL);
}

uint8_t gw_uart_get(void *desc, gw_uart_config_t *out_cnf) {
    gw_uart_private_t *uart = (gw_uart_private_t *) desc;
    gw_uart_word_t bits;
    uint32_t boud = uart->cnf.baud_rate;
    gw_uart_parity_t parity;
    gw_uart_stop_bits_t stop;
    switch (uart->cnf.data_bits) {
        case UART_DATA_7_BITS: bits = GW_UART_WORD_7BIT; break;
        case UART_DATA_8_BITS: bits = GW_UART_WORD_8BIT; break;
        default: return 0;
    }
    switch (uart->cnf.parity) {
        case UART_PARITY_DISABLE: parity = GW_UART_PARITY_NONE; break;
        case UART_PARITY_ODD: parity = GW_UART_PARITY_ODD; break;
        case UART_PARITY_EVEN: parity = GW_UART_PARITY_EVEN; break;
        default: return 0;
    }
    switch (uart->cnf.stop_bits) {
        case UART_STOP_BITS_1: stop = GW_UART_STOP_BITS1; break;
        case UART_STOP_BITS_1_5: stop = GW_UART_STOP_BITS1_5; break;
        case UART_STOP_BITS_2: stop = GW_UART_STOP_BITS2; break;
        default: return 0;
    }
    if(out_cnf != NULL) {
        out_cnf->boud = boud;
        out_cnf->bits = bits;
        out_cnf->parity = parity;
        out_cnf->stop = stop;
        out_cnf->mode = (gw_uart_mode_t)uart->mode;
        out_cnf->idle_ch = uart_idle_chars[uart->port];
        return 1;
    }
    return 0;
}

/* Наш режим → режим драйвера IDF. Однопроводный режим драйверу не виден: он
 * остаётся обычным UART (направление не переключается), а эхо глушит наш приём. */
static uart_mode_t uart_mode_idf(gw_uart_mode_t mode) {
    return (mode == GW_UART_MODE_RS485_HALF_DUPLEX) ? UART_MODE_RS485_HALF_DUPLEX
                                                    : UART_MODE_UART;
}

#if GW_LOG_MODULE
static const char *uart_mode_name(gw_uart_mode_t mode) {
    switch (mode) {
        case GW_UART_MODE_RS485_HALF_DUPLEX: return "полудуплекс";
        case GW_UART_MODE_UART_SINGLE_WIRE:  return "однопроводный";
        case GW_UART_MODE_UART:
        default:                             return "UART";
    }
}
#endif

uint8_t gw_uart_set(void *desc, const gw_uart_config_t *cnf) {
    gw_uart_private_t *uart = (gw_uart_private_t *) desc;
    gw_uart_word_t bits = cnf->bits;
    uint32_t boud = cnf->boud;
    gw_uart_parity_t parity = cnf->parity;
    gw_uart_stop_bits_t stop = cnf->stop;
    switch (bits) {
        case GW_UART_WORD_7BIT: bits = UART_DATA_7_BITS; break;
        case GW_UART_WORD_8BIT: bits = UART_DATA_8_BITS; break;
        case GW_UART_WORD_9BIT:
        default: return 0;
    }
    switch (parity) {
        case GW_UART_PARITY_NONE: parity = UART_PARITY_DISABLE; break;
        case GW_UART_PARITY_ODD: parity = UART_PARITY_ODD; break;
        case GW_UART_PARITY_EVEN: parity = UART_PARITY_EVEN; break;
        default: return 0;
    }
    switch (stop) {
        case GW_UART_STOP_BITS1: stop = UART_STOP_BITS_1; break;
        case GW_UART_STOP_BITS1_5: stop = UART_STOP_BITS_1_5; break;
        case GW_UART_STOP_BITS2: stop = UART_STOP_BITS_2; break;
        case GW_UART_STOP_BITS0_5:
        default: return 0;
    }
    if (cnf->mode != GW_UART_MODE_UART
        && cnf->mode != GW_UART_MODE_RS485_HALF_DUPLEX
        && cnf->mode != GW_UART_MODE_UART_SINGLE_WIRE)
        return 0;
    uart->cnf.baud_rate = boud;
    uart->cnf.data_bits = bits;
    uart->cnf.parity = parity;
    uart->cnf.stop_bits = stop;
    uart->cnf.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uart->cnf.rx_flow_ctrl_thresh = 122;
    /* Настройки переносим в uart->cnf ТОЛЬКО после успеха: иначе при отказе
     * (например, недопустимая скорость) gw_uart_get врал бы и нам, и странице. */
    uart_config_t next = uart->cnf;
    next.baud_rate = boud;
    next.data_bits = bits;
    next.parity = parity;
    next.stop_bits = stop;
    next.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    next.rx_flow_ctrl_thresh = 122;

    if (uart_param_config(uart->port, &next) != ESP_OK) {
        ESP_LOGW(TAG, "UART config failed (boud %lu)", boud);
        return 0;
    }
    /* uart_set_mode требует установленного драйвера: при инициализации режим
     * применяется отдельно, после установки (см. gw_uart_init). */
    if (uart_is_driver_installed(uart->port)
        && uart_set_mode(uart->port, uart_mode_idf(cnf->mode)) != ESP_OK) {
        ESP_LOGW(TAG, "UART mode failed");
        return 0;
    }

    uart->cnf  = next;
    uart->mode = (uint8_t)cnf->mode;
    if (cnf->idle_ch != 0) {
        uart_idle_chars[uart->port] = cnf->idle_ch;
        /* Таймаут приёма — тоже только на установленном драйвере (gw_uart_init). */
        if (uart_is_driver_installed(uart->port))
            uart_set_rx_timeout(uart->port, uart_idle_symbols(cnf->idle_ch));
    } else if (uart_idle_chars[uart->port] == 0) {
        uart_idle_chars[uart->port] = GW_UART_IDLE_CH_DEFAULT;
    }
    LOGI(TAG, "UART configured: port %d, boud: %lu, mode: %s", uart->port, boud,
             uart_mode_name(cnf->mode));
    return 1;
}

/* Вывод годится под UART? 6..11 заняты flash, 34..39 только вход. */
static uint8_t uart_pin_ok(int8_t pin, uint8_t output)
{
    if (pin == GW_UART_PIN_NONE)
        return 1;
    if (pin < 0 || pin > 39)
        return 0;
    if (pin >= 6 && pin <= 11)
        return 0;                        /* SPI-flash */
    if (output && pin >= 34 && pin <= 39)
        return 0;                        /* только вход */
    return 1;
}

uint8_t gw_uart_init(void *desc, gw_uart_port_t port, uint32_t buffer_size) {
    gw_uart_private_t *uart = (gw_uart_private_t *) desc;
    switch (port) {
        case GW_UART_PORT_0: port = UART_NUM_0; break;
        case GW_UART_PORT_1: port = UART_NUM_1; break;
        case GW_UART_PORT_2: port = UART_NUM_2; break;
        default: return 0;
    }
    uart->port = port;
    /* Умолчание по порту — обычный UART; режим задаётся только настройкой порта
     * (uartN.config, MODE). */
    gw_uart_config_t cnf = gw_uart_config_default;
    uint8_t result = gw_uart_set(desc, &cnf);
    int rxp, txp, rtsp;
    int cts = UART_PIN_NO_CHANGE;

    if (uart->port < 0 || uart->port > 2)
        return 0;
    /* Пины — из конфигурации платы (Kconfig, меню "Board configuration"). */
    switch (uart->port) {
        case 0:
            txp  = CONFIG_GW_UART0_TX;
            rxp  = CONFIG_GW_UART0_RX;
            rtsp = CONFIG_GW_UART0_RTS;
            break;
        case 1:
            txp  = CONFIG_GW_UART1_TX;
            rxp  = CONFIG_GW_UART1_RX;
            rtsp = CONFIG_GW_UART1_RTS;
            break;
        default:
            txp  = CONFIG_GW_UART2_TX;
            rxp  = CONFIG_GW_UART2_RX;
            rtsp = CONFIG_GW_UART2_RTS;
            break;
    }
    if (!uart_pin_ok((int8_t)txp, 1) || !uart_pin_ok((int8_t)rxp, 0)
        || !uart_pin_ok((int8_t)rtsp, 1)) {
        ESP_LOGE(TAG, "UART %d: недопустимые пины TX=%d RX=%d RTS=%d", uart->port, txp, rxp, rtsp);
        return 0;
    }
    result = result && (uart_set_pin(uart->port, txp, rxp, rtsp, cts) == ESP_OK);
    /* Приём и передача — РАЗНЫЕ кольца: размер приёма задаёт вызывающий (наш
     * накопитель кадра), размер передачи фиксирован (см. uart.h). */
    result = result && (uart_driver_install(port, buffer_size, GW_UART_TX_RING, 10,
                                            &uart->uart_queue, 0) == ESP_OK);
    result = result && (uart_set_mode(uart->port, uart_mode_idf((gw_uart_mode_t)uart->mode)) == ESP_OK);
    /* Порог «линия молчит» — таймаут приёма драйвера: по нему приходит признак
     * конца кадра (см. gw_uart_event_data_t::b_end). */
    result = result && (uart_set_rx_timeout(uart->port,
                        uart_idle_symbols(uart_idle_chars[uart->port])) == ESP_OK);
    /* Стек задачи приёма: она только копирует принятое в кольцо сервиса и
     * поднимает событие — 3072 хватает с запасом.
     *
     * Приоритет — ВЫШЕ задачи такта API (приоритет 5, см. app.c): у линии нет
     * пауз, а аппаратный FIFO всего 128 байт. При равном или меньшем приоритете
     * FIFO переполняется и приём теряет данные ещё до кольца. Задача очень
     * короткая (скопировать кусок и выйти), поэтому вытеснять такт ей можно. */
    result = result && (xTaskCreate(uart_event_task, "uart_event", 3072, uart, 6, NULL) == pdPASS);
    if(result)
        LOGI(TAG, "UART port %d init OK", uart->port);
    return result;
}

const gw_uart_config_t gw_uart_config_default = {
    .bits = GW_UART_WORD_8BIT,
    .parity = GW_UART_PARITY_NONE,
    .stop = GW_UART_STOP_BITS1,
    .boud = 115200,
    .mode = GW_UART_MODE_UART,
    .idle_ch = GW_UART_IDLE_CH_DEFAULT
};

int32_t gw_uart_read(void *desc, uint8_t *buf, uint16_t size) {
    gw_uart_private_t *uart = (gw_uart_private_t *) desc;
	return uart_read_bytes(uart->port, buf, size, 0);
}

int32_t gw_uart_write(void *desc, const uint8_t *buf, uint16_t size) {
    gw_uart_private_t *uart = (gw_uart_private_t *) desc;
    /* Однопроводная линия: окно глушения открываем ДО первой посылки — иначе
     * первый же байт успеет вернуться эхом и попасть в приём. */
    if (uart_single_wire(uart)) {
        uart_arm_echo(uart->port, uart->cnf.baud_rate, size);
    }
    return uart_write_bytes(uart->port, buf, size);
}

int32_t gw_uart_available_write(void *desc) {
    gw_uart_private_t *uart = (gw_uart_private_t *) desc;
    size_t size;
    if(uart_get_tx_buffer_free_size(uart->port, &size) == ESP_OK)
        return (int32_t)size;
    return -1;
}

void gw_uart_on_receive_subscribe(void *desc, delegate_t *delegate) {
    gw_uart_private_t *uart = (gw_uart_private_t *) desc;
    event_subscribe(&uart->on_receive, delegate);
}

void gw_uart_set_echo(void *desc, uint8_t enabled) {
    gw_uart_private_t *uart = (gw_uart_private_t *) desc;
    uart->echo_enabled = enabled ? 1 : 0;
}

uint8_t gw_uart_get_echo(void *desc) {
    gw_uart_private_t *uart = (gw_uart_private_t *) desc;
    return uart->echo_enabled;
}

/* 1 — с прошлого вызова драйвер терял принятое. Флаг снимается при чтении: одна
 * потеря сообщается клиенту один раз. */
uint8_t gw_uart_take_rx_loss(void *desc) {
    gw_uart_private_t *uart = (gw_uart_private_t *) desc;
    if (uart == NULL || !uart->rx_lost) return 0;
    uart->rx_lost = 0;
    return 1;
}

void gw_uart_lock_rx(void *desc) {
    gw_uart_private_t *uart = (gw_uart_private_t *) desc;
    uart->rx_locked = 1;
}

void gw_uart_unlock_rx(void *desc) {
    gw_uart_private_t *uart = (gw_uart_private_t *) desc;
    uart->rx_locked = 0;
}
