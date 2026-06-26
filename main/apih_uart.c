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

typedef struct {
    uint32_t amount;
    uint32_t size;
    uint8_t *buf;
    SemaphoreHandle_t lock;
} uart_subscription_context_t;


#define UART_API_SEND_DELAY   20
#define UART_TMP_BUF_SIZE     CL_SIZE_ALIGN4((UART_MAX_SPEED / 10) / (1000 / UART_API_SEND_DELAY))

static void uart_event_on_rx(void *event_trigger, void *sender, void *context) {
    (void)sender;
    uart_subscription_context_t *buf_desc = (uart_subscription_context_t *)context;
    gw_uart_event_data_t *data = (gw_uart_event_data_t *)event_trigger;

    xSemaphoreTake(buf_desc->lock, portMAX_DELAY);
    size_t sz = buf_desc->size - buf_desc->amount;
    if(sz > data->size)
        sz = data->size;
    mem_cpy(&buf_desc->buf[buf_desc->amount], data->buf, sz);
    buf_desc->amount += sz;
    xSemaphoreGive(buf_desc->lock);
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
	webapi_msg_t msg;

    //todo: refactor
    if (json) {
    	msg.data = (uint8_t *)cJSON_PrintUnformatted(json);
    	if (!msg.data) {
            msg.data = NULL;
            msg.len = 0; 
        } else {
	        msg.len = strlen((char *)msg.data);
        }
    } else {
        msg.data = NULL;
        msg.len = 0; 
    }

	msg.id  = id;
	msg.fid = fid;
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
	uint32_t amount = 0;
	uint8_t tmp[UART_TMP_BUF_SIZE];

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
		ctx = &uart_context[0];
	case ESP_WS_API_UART2_RAW_RX:
		if (!ctx)
			ctx = &uart_context[1];

		xSemaphoreTake(ctx->lock, portMAX_DELAY);
		amount = ctx->amount;
		if (amount > 0) {
			mem_cpy(tmp, ctx->buf, amount);
			ctx->amount = 0;
		}
		xSemaphoreGive(ctx->lock);

		if (amount > 0) {
			int32_t b64_size = base64_encode_buffer_required(amount);
			//todo: fix. use malloc
			uint8_t b64_buf[b64_size + 1];
			int32_t b64_len = base64_encode(b64_buf, b64_size, tmp, amount);
			if (b64_len > 0) {
				b64_buf[b64_len] = '\0';
				cJSON *json = cJSON_CreateString((char *)b64_buf);
				if (json) {
					/* id=0: broadcast to all subscribers of this FID */
					send_uart_response(0, in_msg->fid, json);
					json_delete(json);
				}
			}
		} else {
			/* id=0: broadcast (empty poll — no data, no response needed) */
			send_uart_response(0, in_msg->fid, NULL);
		}
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
	case ESP_WS_API_UART1_ECHO:
		app_uart = &app->uart.port[0];
		/* fall through */
	case ESP_WS_API_UART2_ECHO:
		if (!app_uart)
			app_uart = &app->uart.port[1];

		if (in_msg->data && in_msg->len) {
			cJSON *json = json_parse_with_length_opts((char *)in_msg->data, in_msg->len, 0, 0);
			if (json) {
				uint32_t val;
				if (json_parse_int(json, "E", &val))
					gw_uart_set_echo(&app_uart->desc, val ? 1 : 0);
				json_delete(json);
			}
		}
		{
			cJSON *resp = cJSON_CreateObject();
			if (resp) {
				cJSON_AddNumberToObject(resp, "E", gw_uart_get_echo(&app_uart->desc));
				send_uart_response(in_msg->id, in_msg->fid, resp);
				json_delete(resp);
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
        ws_server_register_fid_queue(ESP_WS_API_UART1_CNF,    cmd_queue);
        ws_server_register_fid_queue(ESP_WS_API_UART1_RAW_RX, cmd_queue);
        ws_server_register_fid_queue(ESP_WS_API_UART1_RAW_TX, cmd_queue);
        ws_server_register_fid_queue(ESP_WS_API_UART1_ECHO,   cmd_queue);
        ws_server_register_fid_queue(ESP_WS_API_UART2_CNF,    cmd_queue);
        ws_server_register_fid_queue(ESP_WS_API_UART2_RAW_RX, cmd_queue);
        ws_server_register_fid_queue(ESP_WS_API_UART2_RAW_TX, cmd_queue);
        ws_server_register_fid_queue(ESP_WS_API_UART2_ECHO,   cmd_queue);

        uart_context[0].buf = uart1_buf;
        uart_context[0].amount = 0;
        uart_context[0].size = sizeof(uart1_buf);
        uart_context[0].lock = xSemaphoreCreateMutex();
        uart1_delegate.handler = &uart_event_on_rx;
        uart1_delegate.context = &uart_context[0];

        uart_context[1].buf = uart2_buf;
        uart_context[1].amount = 0;
        uart_context[1].size = sizeof(uart2_buf);
        uart_context[1].lock = xSemaphoreCreateMutex();
        uart2_delegate.context = &uart_context[1];
        uart2_delegate.handler = &uart_event_on_rx;

        gw_uart_on_receive_subscribe(&app->uart.port[0].desc, &uart1_delegate);
        gw_uart_on_receive_subscribe(&app->uart.port[1].desc, &uart2_delegate);

        return xTaskCreatePinnedToCore(ws_uart_task, "ws_uart", 4096, app, 5, NULL, tskNO_AFFINITY);
}

