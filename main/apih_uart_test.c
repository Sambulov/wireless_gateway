#include "app.h"

static void log_uart_config(app_context_t *app)
{
    static const char *parity_str[] = { "none", "odd", "even" };
    static const char *stop_str[]   = { "0.5", "1", "2", "1.5" };

    for (int p = 0; p < 2; p++) {
        gw_uart_config_t cnf;
        if (gw_uart_get(&app->uart.port[p].desc, &cnf)) {
            ESP_LOGI("uart_test", "port[%d] baud=%lu bits=%s parity=%s stop=%s",
                     p, cnf.boud,
                     cnf.bits == GW_UART_WORD_7BIT ? "7" : "8",
                     parity_str[cnf.parity],
                     stop_str[cnf.stop]);
        } else {
            ESP_LOGW("uart_test", "port[%d] gw_uart_get failed", p);
        }
    }
}

static void vUartRawTxTest(void *pvParameters)
{
    app_context_t *app = (app_context_t *)pvParameters;
    vTaskDelay(pdMS_TO_TICKS(200));

    log_uart_config(app);

    static const struct {
        uint32_t   fid;
        int        id;
        const char *label;
        const char *data; /* JSON string value: "base64payload" */
    } ports[] = {
        { ESP_WS_API_UART1_RAW_TX, 1, "UART1", "\"SGVsbG8=\"" },
        { ESP_WS_API_UART2_RAW_TX, 2, "UART2", "\"SGVsbG8=\"" },
    };

    uint32_t sent = 0, ok = 0, tout = 0, drop = 0;

    for (int i = 0; ; i++) {
        int p = i & 1;

        if (i % 100 == 0) {
            ESP_LOGI("uart_test", "--- heap free=%lu min=%lu stack_hwm=%u sent=%lu ok=%lu drop=%lu tout=%lu",
                     (uint32_t)esp_get_free_heap_size(),
                     (uint32_t)esp_get_minimum_free_heap_size(),
                     uxTaskGetStackHighWaterMark(NULL),
                     sent, ok, drop, tout);
        }

        size_t data_len = lStrLen(ports[p].data);
        webapi_msg_t *msg = malloc(sizeof(webapi_msg_t));
        if (!msg) {
            drop++;
            ESP_LOGE("uart_test", "[%s] malloc msg failed heap=%lu",
                     ports[p].label, (uint32_t)esp_get_free_heap_size());
            continue;
        }
        msg->data = malloc(data_len);
        if (!msg->data) {
            free(msg);
            drop++;
            ESP_LOGE("uart_test", "[%s] malloc data failed heap=%lu",
                     ports[p].label, (uint32_t)esp_get_free_heap_size());
            continue;
        }
        mem_cpy(msg->data, ports[p].data, data_len);
        msg->fid = ports[p].fid;
        msg->id  = ports[p].id;
        msg->len = data_len;

        if (queue_send(get_uart_worker_queue(), &msg, pdMS_TO_TICKS(0)) != pdPASS) {
            free(msg->data);
            free(msg);
            drop++;
            ESP_LOGW("uart_test", "[%s] cmd_queue full drop=%lu heap=%lu",
                     ports[p].label, drop, (uint32_t)esp_get_free_heap_size());
        } else {
            sent++;
            webapi_msg_t *resp = NULL;
            if (queue_receive(get_ws_worker_queue(), &resp, pdMS_TO_TICKS(100)) == pdPASS) {
                ok++;
                ESP_LOGI("uart_test", "[%s] resp=%.*s sent=%lu ok=%lu drop=%lu tout=%lu heap=%lu",
                         ports[p].label, (int)resp->len, resp->data,
                         sent, ok, drop, tout,
                         (uint32_t)esp_get_free_heap_size());
                free(resp->data);
                free(resp);
            } else {
                tout++;
                ESP_LOGW("uart_test", "[%s] timeout sent=%lu ok=%lu drop=%lu tout=%lu heap=%lu",
                         ports[p].label, sent, ok, drop, tout,
                         (uint32_t)esp_get_free_heap_size());
            }
        }
    }
}

void ws_uart_integrational_test_run(app_context_t *app)
{
    xTaskCreate(vUartRawTxTest, "uart_test", 4096, app, 4, NULL);
}
