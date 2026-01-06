#include "app.h"
#include "uart.h"
#include "cJSON_helpers.h"
#include "web_api.h"

typedef struct {
    __linked_list_object__
    void *call;
    uint8_t *data;
    uint32_t size;
    uint8_t wl_set   : 1,
            boud_set : 1,
            par_set  : 1,
            sb_set   : 1;
    uint8_t port_no  : 2,
            wl       : 2,
            par      : 2,
            sb       : 2;
    uint32_t boud;
} api_cmd_uart_t;

static uint8_t _api_handler_uart_cnf(void *call, void **context, uint32_t pending, uint8_t *arg, uint32_t arg_len, uint8_t port_no) {
    if(!pending) 
        return 1;
    if(arg == NULL) { /* If call with no args, do public subscription on uarts config change */
        if(pending == 1) { /* If first time call, subscribe. We will notyfy all clients by FID */
            api_call_send_status(call, API_CALL_STATUS_EXECUTING);
            return 0;
        }
        else { /* unsubscribe */
            api_call_send_status(call, API_CALL_STATUS_CANCELED);
            api_call_complete(call);
            return 1;
        }
    }
    ESP_LOGI(TAG, "Uart call, with arg:%s", arg);
    uint32_t status = API_CALL_ERROR_STATUS_NO_MEM;
    api_cmd_uart_t *cmd = NULL;
    cJSON *json = json_parse_with_length_opts((char *)arg, arg_len, 0, 0);
    if(json) {
        cmd = malloc(sizeof(api_cmd_uart_t));
        do {
            if(cmd == NULL) { break; }
            memset(cmd, 0, sizeof(api_cmd_uart_t));
            cmd->call = call;
            status = API_CALL_ERROR_STATUS_BAD_ARG;
            cmd->port_no = port_no;
            uint32_t val;
            cmd->wl_set = json_parse_int(json, "WL", &val);
            if(cmd->wl_set) {
                if((val < 7) || (val >= 9)) { break; }
                cmd->wl = val;
            }
            cmd->boud_set = json_parse_int(json, "BR", &val);
            if(cmd->boud_set) {
                if(val > 1000000) { break; }
                cmd->boud = val;
            }
            cmd->par_set = json_parse_int(json, "PAR", &val);
            if(cmd->par_set) {
                if(val >= 3) { break; }
                cmd->par = val;
            }
            cmd->sb_set = json_parse_int(json, "SB", &val);
            if(cmd->sb_set) {
                if((val >= 4) || (val == GW_UART_STOP_BITS0_5)) { break; }
                cmd->sb = val;
            }
            status = API_CALL_STATUS_BUSY;
            queue_handle_t queue = (queue_handle_t)*context;
            if(queue_send(queue, (void *)&cmd, pdMS_TO_TICKS(0)) != pdPASS) {
                ESP_LOGI(TAG, "Uart cmd dropped");
                break;
            }
            ESP_LOGI(TAG, "Uart cmd enqueued");
            status = API_CALL_STATUS_EXECUTING;
            break;
        } while (1);
    }
    if(status != API_CALL_STATUS_EXECUTING) {
        api_call_send_status(call, status);
        free(cmd);
    }
    json_delete(json);
    return (status != API_CALL_STATUS_EXECUTING);
}

static uint8_t _api_handler_uart1_cnf(void *call, void **context, uint32_t pending, uint8_t *arg, uint32_t arg_len) {
    return _api_handler_uart_cnf(call, context, pending, arg, arg_len, 0);
}

static uint8_t _api_handler_uart2_cnf(void *call, void **context, uint32_t pending, uint8_t *arg, uint32_t arg_len) {
    return _api_handler_uart_cnf(call, context, pending, arg, arg_len, 1);
}

static uint8_t _api_handler_uart_raw_tx(void *call, void **context, uint32_t pending, uint8_t *arg, uint32_t arg_len, uint8_t port_no) {
    uint32_t status = API_CALL_ERROR_STATUS_BAD_ARG;
    uint8_t *data = NULL;
    uint8_t valid_arg = arg && (arg_len > 5) && (arg[0] == '"') && ((arg[arg_len - 1] == '"'));
    arg_len -= 2;
    arg++;
    do {
        if(!valid_arg) break;
        int32_t buf_size = base64_decode_buffer_required(arg, arg_len);
        if(buf_size <= 0) break;
        status = API_CALL_ERROR_STATUS_NO_MEM;
        data = malloc(sizeof(api_cmd_uart_t) + buf_size);
        if(!data) break;
        api_cmd_uart_t *cmd = (api_cmd_uart_t *)data;
        status = API_CALL_ERROR_STATUS_BAD_ARG;
        uint8_t *buf = &data[sizeof(api_cmd_uart_t)];
        if(base64_decode(buf, buf_size, arg, arg_len) < 0) break;
        cmd->call = call;
        cmd->data = buf;
        cmd->size = buf_size;
        cmd->port_no = port_no;
        status = API_CALL_STATUS_BUSY;
        queue_handle_t queue = (queue_handle_t)*context;
        if(queue_send(queue, (void *)&cmd, pdMS_TO_TICKS(0)) != pdPASS) {
            ESP_LOGI(TAG, "Uart data dropped");
            break;
        }
        ESP_LOGI(TAG, "Uart data enqueued");
        status = API_CALL_STATUS_EXECUTING;
        break;
    } while (1);
    if(status != API_CALL_STATUS_EXECUTING) {
        api_call_send_status(call, status);
        free(data);
        return 1;
    }
    return 0;
}

static uint8_t _api_handler_uart_raw_rx(void *call, void **context, uint32_t pending, uint8_t *arg, uint32_t arg_len) {
    if(pending == 1) { /* If first time call, subscribe. We will feed data to all clients by FID */
        api_call_send_status(call, API_CALL_STATUS_EXECUTING);
        return 0;
    }
    else if(pending) { /* unsubscribe */
        api_call_send_status(call, API_CALL_STATUS_CANCELED);
        api_call_complete(call);
    }
    return 1;
}

static uint8_t _api_handler_uart1_raw_tx(void *call, void **context, uint32_t pending, uint8_t *arg, uint32_t arg_len) {
    return _api_handler_uart_raw_tx(call, context, pending, arg, arg_len, 0);
}

static uint8_t _api_handler_uart2_raw_tx(void *call, void **context, uint32_t pending, uint8_t *arg, uint32_t arg_len) {
    return _api_handler_uart_raw_tx(call, context, pending, arg, arg_len, 1);
}

typedef struct {
    uint32_t amount;
    uint32_t size;
    uint8_t *buf;
} uart_subscription_context_t;


#define UART_MAX_SPEED        500000
#define UART_API_SEND_DELAY   20
#define UART_TMP_BUF_SIZE     CL_SIZE_ALIGN4((UART_MAX_SPEED / 10) / (1000 / UART_API_SEND_DELAY))

static void uart_event_on_rx(void *event_trigger, void *sender, void *context) {
    (void)sender;
    uart_subscription_context_t *buf_desc = (uart_subscription_context_t *)context;
    gw_uart_event_data_t *data = (gw_uart_event_data_t *)event_trigger;
    size_t sz = buf_desc->size - buf_desc->amount;
    if(sz > data->size) sz = data->size;
    mem_cpy(&buf_desc->buf[buf_desc->amount], data->buf, sz);
    buf_desc->amount += sz;
}

void api_handler_uart_work(app_context_t *app) {
    static uint8_t init = 0;
    static queue_handle_t cmd_queue;
    static uint8_t uart1_buf[UART_TMP_BUF_SIZE];
    static uint8_t uart2_buf[UART_TMP_BUF_SIZE];
    static uart_subscription_context_t uart_context[2];
    static delegate_t uart1_delegate;
    static delegate_t uart2_delegate;

    if(!init) {
        cmd_queue = queue_create(10, sizeof(void *));
        api_call_register(&_api_handler_uart1_cnf, ESP_WS_API_UART1_CNF, cmd_queue);
        api_call_register(&_api_handler_uart2_cnf, ESP_WS_API_UART2_CNF, cmd_queue);
        api_call_register(&_api_handler_uart_raw_rx, ESP_WS_API_UART1_RAW_RX, cmd_queue);
        api_call_register(&_api_handler_uart_raw_rx, ESP_WS_API_UART2_RAW_RX, cmd_queue);
        api_call_register(&_api_handler_uart1_raw_tx, ESP_WS_API_UART1_RAW_TX, cmd_queue);
        api_call_register(&_api_handler_uart2_raw_tx, ESP_WS_API_UART2_RAW_TX, cmd_queue);

        uart_context[0].buf = uart1_buf;
        uart_context[0].amount = 0;
        uart_context[0].size = sizeof(uart1_buf);
        uart1_delegate.handler = &uart_event_on_rx;
        uart1_delegate.context = &uart_context[0];

        uart_context[1].buf = uart2_buf;
        uart_context[1].amount = 0;
        uart_context[1].size = sizeof(uart2_buf);
        uart2_delegate.context = &uart_context[1];
        uart2_delegate.handler = &uart_event_on_rx;

        gw_uart_on_receive_subscribe(&app->uart.port[0].desc, &uart1_delegate);
        gw_uart_on_receive_subscribe(&app->uart.port[1].desc, &uart2_delegate);

        init = 1;
    }
    api_cmd_uart_t *cmd = NULL;
    const uint32_t port_rx_fid[2] = {ESP_WS_API_UART1_RAW_RX, ESP_WS_API_UART2_RAW_RX};
    const uint32_t port_cnf_fid[2] = {ESP_WS_API_UART1_CNF, ESP_WS_API_UART2_CNF};
    do {
        cmd = NULL;
        queue_receive(cmd_queue, &cmd, pdMS_TO_TICKS(0));
        if(cmd != NULL) {
            struct app_uart_t *app_uart = &app->uart.port[cmd->port_no];
            if(cmd->data) {
                gw_uart_write(&app_uart->desc, cmd->data, cmd->size);
            }
            else {
                gw_uart_config_t cnf;
                gw_uart_get(&app_uart->desc ,&cnf);
                if(cmd->boud_set) cnf.boud = cmd->boud;
                if(cmd->par_set) cnf.parity = cmd->par;
                if(cmd->sb_set) cnf.stop = cmd->sb;
                if(cmd->wl_set) cnf.bits = cmd->wl;
                gw_uart_set(&app_uart->desc, &cnf);
                uint8_t tmpbuf[64];
                uint32_t len = sprintf((char *)tmpbuf, 
                    "{\"BR\":\"0x%08lx\",\"WL\":\"0x%02x\",\"PAR\":\"0x%02x\",\"SB\":\"0x%02x\"}", 
                    cnf.boud, cnf.bits, cnf.parity, cnf.stop);
                ESP_LOGI(TAG, "Uart new config %s", tmpbuf);
                api_call_send_json_fid_group(port_cnf_fid[cmd->port_no], tmpbuf, len);
            }
            api_call_send_status(cmd->call, API_CALL_STATUS_COMPLETE);
            api_call_complete(cmd->call);
            free(cmd);
        }
    } while (cmd);
    uint32_t now = task_get_tick_count();
    if(CL_TIME_ELAPSED(app->uart.raw_sent_ts, UART_API_SEND_DELAY, now)) {
        app->uart.raw_sent_ts = now;
        for(uint8_t no = 0; no < 2; no++) {
            if(uart_context[no].amount) {
                int32_t tmp_buf_len = lBase64EncodeBufferRequired(uart_context[no].amount);
                uint8_t buf[tmp_buf_len + 2];
                buf[0] = buf[tmp_buf_len + 1] = '\"';
                base64_encode(&buf[1], tmp_buf_len, uart_context[no].buf, uart_context[no].amount);
                api_call_send_json_fid_group(port_rx_fid[no], buf, 2 + tmp_buf_len);
            }
            uart_context[no].amount = 0;
        }
    }
}
