#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "app.h"


static const char *TAG = "uart";

/*  
UART0      RX TX RTS CTS
UART_NUM_0  3  1  22  19
UART_NUM_1  9 10  11   6
UART_NUM_2 16 17   7   8
*/

#define UART_PORT         UART_NUM_1
#define UART_RX_PIN                9
#define UART_TX_PIN               10
#define UART_RTS_PIN              11
#define UART_CTS_PIN               6
#define UART_BUF_SIZE           1024

// Задача для чтения данных с UART и отправки через WebSocket
void uart_to_websocket_task(void *pvParameters) {
    uint8_t *data = (uint8_t *)malloc(UART_BUF_SIZE);
    while (1) {
        int len = uart_read_bytes(UART_PORT, data, UART_BUF_SIZE, pdMS_TO_TICKS(100));
        if (len > 0) {
            ESP_LOGI(TAG, "UART Data Received: %.*s", len, data);
            // Отправляем данные всем подключенным WebSocket-клиентам
            // if (server != NULL) {
            //     httpd_ws_frame_t ws_pkt = {
            //         .final = true,
            //         .fragmented = false,
            //         .type = HTTPD_WS_TYPE_TEXT,
            //         .payload = data,
            //         .len = len,
            //     };
            //     httpd_ws_send_frame_async(server, httpd_req_to_sockfd(server->hd_req), &ws_pkt);
            // }
        }
    }
    free(data);
}

static void uart_event_task(void *pvParameters)
{
    uart_event_t event;
    QueueHandle_t uart_queue = (QueueHandle_t)pvParameters;
    size_t buffered_size;
    uint8_t* dtmp = (uint8_t*) malloc(UART_BUF_SIZE);
    for (;;) {
        //Waiting for UART event.
        if (xQueueReceive(uart_queue, (void *)&event, (TickType_t)portMAX_DELAY)) {
            bzero(dtmp, UART_BUF_SIZE);
            ESP_LOGI(TAG, "uart[%d] event:", UART_PORT);
            switch (event.type) {
            //Event of UART receiving data
            /*We'd better handler data event fast, there would be much more data events than
            other types of events. If we take too much time on data event, the queue might
            be full.*/
            case UART_DATA:
                ESP_LOGI(TAG, "[UART DATA]: %d", event.size);
                uart_read_bytes(UART_PORT, dtmp, event.size, portMAX_DELAY);
                ESP_LOGI(TAG, "[DATA EVT]:");
                uart_write_bytes(UART_PORT, (const char*) dtmp, event.size);
                break;
            //Event of HW FIFO overflow detected
            case UART_FIFO_OVF:
                ESP_LOGI(TAG, "hw fifo overflow");
                // If fifo overflow happened, you should consider adding flow control for your application.
                // The ISR has already reset the rx FIFO,
                // As an example, we directly flush the rx buffer here in order to read more data.
                uart_flush_input(UART_PORT);
                xQueueReset(uart_queue);
                break;
            //Event of UART ring buffer full
            case UART_BUFFER_FULL:
                ESP_LOGI(TAG, "ring buffer full");
                // If buffer full happened, you should consider increasing your buffer size
                // As an example, we directly flush the rx buffer here in order to read more data.
                uart_flush_input(UART_PORT);
                xQueueReset(uart_queue);
                break;
            //Event of UART RX break detected
            case UART_BREAK:
                ESP_LOGI(TAG, "uart rx break");
                break;
            //Event of UART parity check error
            case UART_PARITY_ERR:
                ESP_LOGI(TAG, "uart parity error");
                break;
            //Event of UART frame error
            case UART_FRAME_ERR:
                ESP_LOGI(TAG, "uart frame error");
                break;
            //UART_PATTERN_DET
            case UART_PATTERN_DET:
                uart_get_buffered_data_len(UART_PORT, &buffered_size);
                int pos = uart_pattern_pop_pos(UART_PORT);
                ESP_LOGI(TAG, "[UART PATTERN DETECTED] pos: %d, buffered size: %d", pos, buffered_size);
                if (pos == -1) {
                    // There used to be a UART_PATTERN_DET event, but the pattern position queue is full so that it can not
                    // record the position. We should set a larger queue size.
                    // As an example, we directly flush the rx buffer here.
                    uart_flush_input(UART_PORT);
                } else {
                    uart_read_bytes(UART_PORT, dtmp, pos, 100 / portTICK_PERIOD_MS);
                }
                break;
            //Others
            default:
                ESP_LOGI(TAG, "uart event type: %d", event.type);
                break;
            }
        }
    }
    free(dtmp);
    dtmp = NULL;
    vTaskDelete(NULL);
}

void uart_init() {
    static QueueHandle_t uart_queue;
    static const uart_config_t uart_defaut_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_RTS,
        .rx_flow_ctrl_thresh = 122,
    };
    uart_param_config(UART_PORT, &uart_defaut_config);
    uart_set_pin(UART_PORT, UART_TX_PIN, UART_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uart_driver_install(UART_PORT, UART_BUF_SIZE, UART_BUF_SIZE, 10, &uart_queue, 0);
    xTaskCreate(uart_event_task, "uart_event", 4096, &uart_queue, 5, NULL);
}

void uart_reconfigure(uart_config_t *uart_cnf) {
    ESP_ERROR_CHECK(uart_param_config(UART_PORT, uart_cnf));
    ESP_LOGI(TAG, "UART reconfigured: br=%d, db=%d, p=%d, sb=%d", 
        uart_cnf->baud_rate, uart_cnf->data_bits, uart_cnf->parity, uart_cnf->stop_bits);
}

void uart_send_data(const char *data, size_t len) {
    uart_write_bytes(UART_PORT, data, len);
    ESP_LOGI(TAG, "Data sent to UART: %.*s", len, data);
}

