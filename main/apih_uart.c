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
            /* TODO: trigger notifycation */
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
    uint8_t valid_arg = arg && (arg_len > 3) && ((arg_len & 1) == 0) && (arg[0] == '"') && ((arg[arg_len - 1] == '"'));
    arg_len -= 2;
    arg++;
    for(uint32_t i = 0; valid_arg && (i < arg_len); i++)
        valid_arg = IS_HEX(arg[i]);
    if(valid_arg) {
        status = API_CALL_ERROR_STATUS_NO_MEM;
        uint32_t buf_size = arg_len / 2;
        data = malloc(sizeof(api_cmd_uart_t) + buf_size);
        if(data) {
            api_cmd_uart_t *cmd = (api_cmd_uart_t *)data;
            memset(cmd, 0, sizeof(api_cmd_uart_t));
            uint8_t *buf = &data[sizeof(api_cmd_uart_t)];
            for(uint32_t i = 0, j = 0; i < buf_size; i++, j += 2)
                buf[i] = (CHAR_TO_HEX(arg[j]) << 4) | CHAR_TO_HEX(arg[j + 1]);
            cmd->call = call;
            cmd->data = buf;
            cmd->size = buf_size;
            cmd->port_no = port_no;
            status = API_CALL_STATUS_BUSY;
            queue_handle_t queue = (queue_handle_t)*context;
            if(queue_send(queue, (void *)&cmd, pdMS_TO_TICKS(0)) != pdPASS) {
                ESP_LOGI(TAG, "Uart data dropped");
            }
            else {
                ESP_LOGI(TAG, "Uart data enqueued");
                status = API_CALL_STATUS_EXECUTING;
            }
        }
    }
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

void api_handler_uart_work(app_context_t *app) {
    static uint8_t init = 0;
    static queue_handle_t cmd_queue;
    if(!init) {
        cmd_queue = queue_create(10, sizeof(void *));
        api_call_register(&_api_handler_uart1_cnf, ESP_WS_API_UART1_CNF, cmd_queue);
        api_call_register(&_api_handler_uart2_cnf, ESP_WS_API_UART2_CNF, cmd_queue);
        api_call_register(&_api_handler_uart_raw_rx, ESP_WS_API_UART1_RAW_RX, cmd_queue);
        api_call_register(&_api_handler_uart_raw_rx, ESP_WS_API_UART2_RAW_RX, cmd_queue);
        api_call_register(&_api_handler_uart1_raw_tx, ESP_WS_API_UART1_RAW_TX, cmd_queue);
        api_call_register(&_api_handler_uart2_raw_tx, ESP_WS_API_UART2_RAW_TX, cmd_queue);
        init = 1;
    }
    api_cmd_uart_t *cmd = NULL;
    const uint32_t port_rx_fid[2] = {ESP_WS_API_UART1_RAW_RX, ESP_WS_API_UART2_RAW_RX};
    const uint32_t port_cnf_fid[2] = {ESP_WS_API_UART1_CNF, ESP_WS_API_UART2_CNF};
    do {
        cmd = NULL;
        queue_receive(cmd_queue, &cmd, pdMS_TO_TICKS(0));
        if(cmd != NULL) {
            gw_uart_t *uart = &app->uart.port[cmd->port_no].desc;
            if(cmd->data) {
                gw_uart_write(uart, cmd->data, cmd->size);
            }
            else {
                gw_uart_config_t cnf;
                gw_uart_get(uart ,&cnf);
                if(cmd->boud_set) cnf.boud = cmd->boud;
                if(cmd->par_set) cnf.parity = cmd->par;
                if(cmd->sb_set) cnf.stop = cmd->sb;
                if(cmd->wl_set) cnf.bits = cmd->wl;
                gw_uart_set(uart, &cnf);
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
    if(CL_TIME_ELAPSED(app->uart.raw_sent_ts, 10, now)) {
        app->uart.raw_sent_ts = now;
        uint8_t raw_data[1024];
        for(uint8_t no = 0; no < 2; no++) {
            if(app->uart.port[no].mode == UART_PORT_MODE_RAW) {
                int32_t len = gw_uart_read(&app->uart.port[no].desc, raw_data, 1024);
                if(len > 0) {
                    uint8_t buf[len * 2 + 2];
                    buf[0] = buf[len * 2 + 1] = '\"';
                    for(int i = 0, j = 1; i < len; i++, j += 2) {
                        uint8_t q = ((raw_data[i] >> 4) & 0xf);
                        buf[j] = HEX_TO_CHAR(q);
                        q = (raw_data[i] & 0xf);
                        buf[j + 1] = HEX_TO_CHAR(q);
                    }
                    api_call_send_json_fid_group(port_rx_fid[no], buf, 2 + len * 2);
                }
            }
        }
    }
}
