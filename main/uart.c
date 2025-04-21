#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_intr_alloc.h"
#include "app.h"
#include "uart.h"

static const char *TAG = "uart";

typedef struct {
    QueueHandle_t uart_queue;
    int port; // UART_NUM_0, UART_NUM_1, UART_NUM_2
} gw_uart_private_t;

#define ASSERRT_STRUCTURE_CAST(private_type, public_type, prv_size_def, def_file)   _Static_assert(sizeof(private_type) == sizeof(public_type), "In "def_file" data structure size of "#public_type" doesn't match, check "#prv_size_def)

ASSERRT_STRUCTURE_CAST(gw_uart_private_t, gw_uart_t, GW_UART_PRIVATE_SIZE, "uart.h");

static void uart_event_task(void *pvParameters)
{
    uart_event_t event;
    gw_uart_private_t *uart = (gw_uart_private_t *)pvParameters;

    //Waiting for UART event.
    while (xQueueReceive(uart->uart_queue, (void *)&event, (TickType_t)portMAX_DELAY)) {
        switch (event.type) {
        //Event of UART receiving data
        /*We'd better handler data event fast, there would be much more data events than
        other types of events. If we take too much time on data event, the queue might
        be full.*/
        case UART_DATA:
            //uart_read_bytes(UART_PORT, dtmp, event.size, portMAX_DELAY);
            //uart_write_bytes(UART_PORT, (const char*) dtmp, event.size);
            break;
        //Event of HW FIFO overflow detected
        case UART_FIFO_OVF:
            ESP_LOGI(TAG, "hw fifo overflow");
            // If fifo overflow happened, you should consider adding flow control for your application.
            // The ISR has already reset the rx FIFO,
            // As an example, we directly flush the rx buffer here in order to read more data.
            uart_flush_input(uart->port);
            xQueueReset(uart->uart_queue);
            break;
        //Event of UART ring buffer full
        case UART_BUFFER_FULL:
            ESP_LOGI(TAG, "ring buffer full");
            // If buffer full happened, you should consider increasing your buffer size
            // As an example, we directly flush the rx buffer here in order to read more data.
            uart_flush_input(uart->port);
            xQueueReset(uart->uart_queue);
            break;
        //Event of UART RX break detected
        case UART_BREAK:
            //ESP_LOGI(TAG, "uart rx break");
            break;
        //Event of UART parity check error
        case UART_PARITY_ERR:
            //ESP_LOGI(TAG, "uart parity error");
            break;
        //Event of UART frame error
        case UART_FRAME_ERR:
            //ESP_LOGI(TAG, "uart frame error");
            break;
        //UART_PATTERN_DET
        case UART_PATTERN_DET:
            //uart_get_buffered_data_len(UART_PORT, &buffered_size);
            //int pos = uart_pattern_pop_pos(UART_PORT);
            //ESP_LOGI(TAG, "[UART PATTERN DETECTED] pos: %d, buffered size: %d", pos, buffered_size);
            //if (pos == -1) {
                // There used to be a UART_PATTERN_DET event, but the pattern position queue is full so that it can not
                // record the position. We should set a larger queue size.
                // As an example, we directly flush the rx buffer here.
            //    uart_flush_input(UART_PORT);
            //} else {
            //    uart_read_bytes(UART_PORT, dtmp, pos, 100 / portTICK_PERIOD_MS);
            //}
            break;
        //Others
        default:
            ESP_LOGI(TAG, "uart event type: %d", event.type);
            break;
        }
    }
    vTaskDelete(NULL);
}

uint8_t gw_uart_set(void *desc, gw_uart_word_t bits, uint32_t boud, gw_uart_parity_t parity, gw_uart_stop_bits_t stop) {
    gw_uart_private_t *uart = (gw_uart_private_t *) desc;
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
    const uart_config_t new_config = {
        .baud_rate = boud,
        .data_bits = bits,
        .parity = parity,
        .stop_bits = stop,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 122,
    };
    int rxp, txp, rts, cts = UART_PIN_NO_CHANGE;
    /*  
    UART0      RX TX RTS CTS
    UART_NUM_0  3  1  22  19
    UART_NUM_1  9 10  11   6
    UART_NUM_2 16 17   7   8
    */
    switch(uart->port) {
        case UART_NUM_0: rxp = 3; txp = 1; rts = 22; break;
        case UART_NUM_2: rxp = 16; txp = 17; rts = 18; break;
        case UART_NUM_1: rxp = 9; txp = 10; rts = 11; /* GPIO6 to GPIO11 connected to flash */
        /* fall through */
        default: return 0;
    }
    uint8_t result = 1;
    result = result && (uart_set_pin(uart->port, txp, rxp, rts, cts) == ESP_OK);
    result = result && (uart_param_config(uart->port, &new_config) == ESP_OK);
    ESP_LOGI(TAG, "UART configured: port %d, tx_pin: %d, rx_pin: %d", uart->port, txp, rxp);
    return result;
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
    uint8_t result = gw_uart_set(desc, GW_UART_WORD_8BIT, 115200, GW_UART_PARITY_NONE, GW_UART_STOP_BITS1);
    result = result && (uart_driver_install(port, buffer_size, buffer_size, 10, &uart->uart_queue, 0) == ESP_OK);
    result = result && (uart_set_mode(uart->port, UART_MODE_RS485_HALF_DUPLEX) == ESP_OK);
    //ESP_ERROR_CHECK(uart_enable_rx_intr(uart->port));
    result = result && (xTaskCreate(uart_event_task, "uart_event", 4096, uart, tskIDLE_PRIORITY, NULL) == pdPASS);
    return result;
}

int32_t gw_uart_read(void *desc, uint8_t *buf, uint16_t size) {
    gw_uart_private_t *uart = (gw_uart_private_t *) desc;
	return uart_read_bytes(uart->port, buf, size, 0);
}

int32_t gw_uart_write(void *desc, const uint8_t *buf, uint16_t size) {
    gw_uart_private_t *uart = (gw_uart_private_t *) desc;
	return uart_write_bytes(uart->port, buf, size);
}
