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

queue_handle_t cmd_queue;

queue_handle_t get_uart_worker_queue(void)
{
	return cmd_queue;
}

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
                if(val > UART_MAX_SPEED) { break; }
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
    uint8_t valid_arg = arg && (arg_len > 2) && (arg[0] == '"') && (arg[arg_len - 1] == '"');
    if (!valid_arg) {
        api_call_send_status(call, API_CALL_ERROR_STATUS_BAD_ARG);
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

static uint8_t _api_handler_uart_echo(void *call, void **context, uint32_t pending, uint8_t *arg, uint32_t arg_len, uint8_t port_no) {
    if(!pending)
        return 1;
    app_context_t *app = (app_context_t *)*context;
    struct app_uart_t *app_uart = &app->uart.port[port_no];
    uint32_t status = API_CALL_STATUS_COMPLETE;

    if(arg == NULL) {
        /* If called with no args, return current echo status */
    }
    else {
        cJSON *json = json_parse_with_length_opts((char *)arg, arg_len, 0, 0);
        if(json) {
            uint32_t val;
            if(json_parse_int(json, "E", &val)) {
                gw_uart_set_echo(&app_uart->desc, val ? 1 : 0);
            }
            json_delete(json);
        }
        else {
            status = API_CALL_ERROR_STATUS_BAD_ARG;
        }
    }

    /* Send response with current echo state */
    if(status == API_CALL_STATUS_COMPLETE) {
        uint8_t echo_state = gw_uart_get_echo(&app_uart->desc);
        uint8_t tmpbuf[16];
        uint32_t len = sprintf((char *)tmpbuf, "{\"E\":%d}", echo_state);
        api_call_send_json(call, tmpbuf, len);
    }
    api_call_send_status(call, status);
    api_call_complete(call);
    return 1;
}

static uint8_t _api_handler_uart1_echo(void *call, void **context, uint32_t pending, uint8_t *arg, uint32_t arg_len) {
    return _api_handler_uart_echo(call, context, pending, arg, arg_len, 0);
}

static uint8_t _api_handler_uart2_echo(void *call, void **context, uint32_t pending, uint8_t *arg, uint32_t arg_len) {
    return _api_handler_uart_echo(call, context, pending, arg, arg_len, 1);
}

typedef struct {
    uint32_t amount;
    uint32_t size;
    uint8_t *buf;
} uart_subscription_context_t;


#define UART_API_SEND_DELAY   20
#define UART_TMP_BUF_SIZE     CL_SIZE_ALIGN4((UART_MAX_SPEED / 10) / (1000 / UART_API_SEND_DELAY))

static void uart_event_on_rx(void *event_trigger, void *sender, void *context) {
    (void)sender;
    uart_subscription_context_t *buf_desc = (uart_subscription_context_t *)context;
    gw_uart_event_data_t *data = (gw_uart_event_data_t *)event_trigger;
    size_t sz = buf_desc->size - buf_desc->amount;

    if(sz > data->size)
        sz = data->size;

    mem_cpy(&buf_desc->buf[buf_desc->amount], data->buf, sz);
    buf_desc->amount += sz;
}


static cJSON *format_uart_resp(const gw_uart_config_t *cnf)
{
	cJSON *json = cJSON_CreateObject();

	if (!json)
		return NULL;
	cJSON_AddNumberToObject(json, "BR",  cnf->boud);
	cJSON_AddNumberToObject(json, "WL",  cnf->bits);
	cJSON_AddNumberToObject(json, "PAR", cnf->parity);
	cJSON_AddNumberToObject(json, "SB",  cnf->stop);
	return json;
}

static uint8_t uart1_buf[UART_TMP_BUF_SIZE];
static uint8_t uart2_buf[UART_TMP_BUF_SIZE];
static uart_subscription_context_t uart_context[2];
static delegate_t uart1_delegate;
static delegate_t uart2_delegate;

static gw_uart_config_t uart_config(struct app_uart_t *uart, void *new_cfg, size_t new_cfg_len)
{
    api_cmd_uart_t *cfg = (api_cmd_uart_t *)new_cfg;
    gw_uart_config_t gw_cfg = {0};

    gw_uart_get(&uart->desc, &gw_cfg);

    if(cfg->boud_set)
        gw_cfg.boud = cfg->boud;
    if(cfg->par_set)
        gw_cfg.parity = cfg->par;
    if(cfg->sb_set)
        gw_cfg.stop = cfg->sb;
    if(cfg->wl_set)
        gw_cfg.bits = cfg->wl;

    gw_uart_set(&uart->desc, &gw_cfg);
    
    return gw_cfg;
}

static void send_uart_response(int id, int fid, cJSON *json)
{
	webapi_msg_t *msg = malloc(sizeof(webapi_msg_t));

	if (!msg)
		return;
	msg->data = (uint8_t *)cJSON_PrintUnformatted(json);
	if (!msg->data) {
		free(msg);
		return;
	}
	msg->len = strlen((char *)msg->data);
	msg->id  = id;
	msg->fid = fid;
	queue_send(get_ws_worker_queue(), &msg, pdMS_TO_TICKS(0));
}

static uint8_t parse_uart_params(const uint8_t *data, size_t len,
				 api_cmd_uart_t *cmd)
{
	cJSON *json = json_parse_with_length_opts((char *)data, len, 0, 0);
	uint32_t val;

	if (!json)
		return 0;
	cmd->wl_set = json_parse_int(json, "WL", &val);
	if (cmd->wl_set)
		cmd->wl = val;
	cmd->boud_set = json_parse_int(json, "BR", &val);
	if (cmd->boud_set)
		cmd->boud = val;
	cmd->par_set = json_parse_int(json, "PAR", &val);
	if (cmd->par_set)
		cmd->par = val;
	cmd->sb_set = json_parse_int(json, "SB", &val);
	if (cmd->sb_set)
		cmd->sb = val;
	json_delete(json);
	return 1;
}

static void handle_msg(app_context_t *app, webapi_msg_t *in_msg)
{
	struct app_uart_t *app_uart = NULL;
	uart_subscription_context_t *ctx = NULL;
	gw_uart_config_t new_cfg;

	switch (in_msg->fid) {
	case ESP_WS_API_UART1_CNF:
		app_uart = &app->uart.port[0];
	case ESP_WS_API_UART2_CNF:
		if (!app_uart)
			app_uart = &app->uart.port[1];

		if (!in_msg->data)
			break;
		api_cmd_uart_t cmd = {0};
		if (parse_uart_params(in_msg->data, in_msg->len, &cmd)) {
			new_cfg = uart_config(app_uart, &cmd, sizeof(cmd));
			cJSON *resp_json = format_uart_resp(&new_cfg);
			if (resp_json) {
				send_uart_response(in_msg->id, in_msg->fid, resp_json);
				json_delete(resp_json);
			}
		}
		break;
	case ESP_WS_API_UART1_RAW_RX:
	case ESP_WS_API_UART2_RAW_RX:
		break;
	case ESP_WS_API_UART1_RAW_TX:
		ctx = &uart_context[0];
	case ESP_WS_API_UART2_RAW_TX:
		if (!ctx)
			ctx = &uart_context[1];

		if (in_msg->data && in_msg->len > 2) {
			uint8_t *b64 = in_msg->data + 1;       /* skip leading '"' */
			size_t b64_len = in_msg->len - 2;       /* strip both '"' */
			int32_t buf_size = base64_decode_buffer_required(b64, b64_len);

			if (buf_size > 0) {
				uint8_t buf[buf_size];
				int32_t decoded = base64_decode(buf, buf_size, b64, b64_len);

				if (decoded > 0) {
					int port = (ctx == &uart_context[0]) ? 0 : 1;
					int32_t written = gw_uart_write(&app->uart.port[port].desc, buf, decoded);

					cJSON *resp_json = cJSON_CreateObject();
					if (resp_json) {
						cJSON_AddNumberToObject(resp_json, "written", written);
						send_uart_response(in_msg->id, in_msg->fid, resp_json);
						json_delete(resp_json);
					}
				}
			}
		}
		break;
	default:
		ESP_LOGI(TAG, "UART have no FID (%d) handler\n", in_msg->fid);
		break;
	}
}

//void api_handler_uart_work(app_context_t *app) {
void ws_uart_task(void *param) {
    webapi_msg_t *in_msg = NULL;
    app_context_t *app = param;

    for (;;) {
        if (queue_receive(cmd_queue, &in_msg, portMAX_DELAY) == pdPASS) {
            handle_msg(app, in_msg);
            free(in_msg->data);
            free(in_msg);
            in_msg = NULL;
        }
    }
}

esp_err_t ws_uart_run(app_context_t *app)
{
        cmd_queue = queue_create(10, sizeof(void *));
        api_call_register(&_api_handler_uart1_cnf, ESP_WS_API_UART1_CNF, cmd_queue);
        api_call_register(&_api_handler_uart2_cnf, ESP_WS_API_UART2_CNF, cmd_queue);
        api_call_register(&_api_handler_uart_raw_rx, ESP_WS_API_UART1_RAW_RX, cmd_queue);
        api_call_register(&_api_handler_uart_raw_rx, ESP_WS_API_UART2_RAW_RX, cmd_queue);
        api_call_register(&_api_handler_uart1_raw_tx, ESP_WS_API_UART1_RAW_TX, cmd_queue);
        api_call_register(&_api_handler_uart2_raw_tx, ESP_WS_API_UART2_RAW_TX, cmd_queue);
        api_call_register(&_api_handler_uart1_echo, ESP_WS_API_UART1_ECHO, app);
        api_call_register(&_api_handler_uart2_echo, ESP_WS_API_UART2_ECHO, app);

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

        return xTaskCreatePinnedToCore(ws_uart_task, "ws_uart", 4096, app, 5, NULL, tskNO_AFFINITY);
}

