#ifdef APIH_UART_TEST
#  include "apih_uart_app_types.h"
#else
#  include "app.h"
#endif
#include "uart.h"
#include "GwJson.h"

/* Токенов на разбор ARG UART: WL/BR/PAR/SB (или E) — с большим запасом. */
#define UART_ARG_MAX_TOKENS 16
#include "web_api.h"
#include "ws_hal.h"

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

ws_queue_t cmd_queue;

ws_queue_t get_uart_worker_queue(void)
{
	return cmd_queue;
}

typedef struct {
    uint32_t amount;
    uint32_t size;
    uint8_t *buf;
    ws_mutex_t lock;
} uart_subscription_context_t;


#define UART_API_SEND_DELAY   20
#define UART_TMP_BUF_SIZE     CL_SIZE_ALIGN4((UART_MAX_SPEED / 10) / (1000 / UART_API_SEND_DELAY))

static void uart_event_on_rx(void *event_trigger, void *sender, void *context) {
    (void)sender;
    uart_subscription_context_t *buf_desc = (uart_subscription_context_t *)context;
    gw_uart_event_data_t *data = (gw_uart_event_data_t *)event_trigger;

    ws_hal_mutex_take(buf_desc->lock, WS_HAL_WAIT_FOREVER);
    size_t sz = buf_desc->size - buf_desc->amount;
    if(sz > data->size)
        sz = data->size;
    mem_cpy(&buf_desc->buf[buf_desc->amount], data->buf, sz);
    buf_desc->amount += sz;
    ws_hal_mutex_give(buf_desc->lock);
}


/* Буферы ответов статические: задача обслуживает запросы по одному, а стек у
 * ws_uart всего 4 КБ — на нём уже лежат tmp[UART_TMP_BUF_SIZE] и массив под
 * base64. Сборка идёт прямо в буфер, дерева и узлов нет. */
#define UART_RESP_MAX 96
static char uart_resp_buf[UART_RESP_MAX];

/* Ответ RAW_RX — это base64 (4/3 от принятого) в кавычках: сам буфер, кавычки и
 * завершающий ноль. */
#define UART_RAW_B64_MAX (4 * ((UART_TMP_BUF_SIZE + 2) / 3))
static char uart_raw_buf[UART_RAW_B64_MAX + 3];

static void format_uart_resp(gw_jsonw_t *w, const gw_uart_config_t *cnf)
{
	gw_jsonw_init(w, uart_resp_buf, sizeof(uart_resp_buf));
	gw_jsonw_obj(w, NULL);
	gw_jsonw_int(w, "BR",  cnf->boud);
	gw_jsonw_int(w, "WL",  cnf->bits);
	gw_jsonw_int(w, "PAR", cnf->parity);
	gw_jsonw_int(w, "SB",  cnf->stop);
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

/* w == NULL — отвечать нечем (пустой опрос RAW_RX): уходит пустой ответ, как и
 * раньше, когда дерева не было вовсе. */
static void send_uart_response(int id, const char *fid, gw_jsonw_t *w)
{
	/* Peripheral-originated answer: no client socket of its own */
	webapi_msg_t msg = { .fd = -1 };

	if (w) {
		uint8_t room = gw_jsonw_ok(w);
		size_t  len  = gw_jsonw_finish(w);
		if (room && len) {
			/* +1 и ноль на конце — как было у прежнего сборщика */
			msg.data = malloc(len + 1);
			if (msg.data) {
				mem_cpy(msg.data, w->buf, len);
				msg.data[len] = '\0';
				msg.len = len;
			}
		}
	}

	msg.id  = id;
	bApiFidSet(msg.fid, fid);
	ws_hal_queue_send(get_ws_worker_queue(), &msg, WS_HAL_WAIT_NONE);
}

static uint8_t parse_uart_params(const uint8_t *data, size_t len,
				 api_cmd_uart_t *cmd)
{
	/* Разбор ARG через jsmn: токены на стеке, память на поля не выделяется. */
	GW_JSON_TOKENS(toks, UART_ARG_MAX_TOKENS);
	gw_json_t json;
	int32_t val;

	if (!gw_json_parse(&json, (const char *)data, len, toks, UART_ARG_MAX_TOKENS))
		return 0;
	cmd->wl_set = gw_json_int(&json, "WL", &val);
	if (cmd->wl_set) {
		/* Принимаем и число бит (7/8), и значение перечисления (0/1):
		 * в структуре поле всего двухбитное, поэтому 8 превращалось в 0
		 * и устройство молча переключалось на 7 бит данных. */
		if (val == 7 || val == 0)
			cmd->wl = GW_UART_WORD_7BIT;
		else if (val == 8 || val == 1)
			cmd->wl = GW_UART_WORD_8BIT;
		else
			cmd->wl_set = 0;
	}
	cmd->boud_set = gw_json_int(&json, "BR", &val);
	if (cmd->boud_set)
		cmd->boud = val;
	cmd->par_set = gw_json_int(&json, "PAR", &val);
	if (cmd->par_set)
		cmd->par = val;
	cmd->sb_set = gw_json_int(&json, "SB", &val);
	if (cmd->sb_set)
		cmd->sb = val;
	return 1;
}

/* Порт из имени FID: "uart1.*" → 0, "uart2.*" → 1, иначе -1. Разбор строгий —
 * ровно "uart" + одна цифра + точка, чтобы "uart12.rx" не попал в первый порт. */
static int uart_fid_port(const char *fid)
{
	if (!fid || strncmp(fid, "uart", 4) != 0)
		return -1;
	if (fid[4] != '1' && fid[4] != '2')
		return -1;
	if (fid[5] != '.')
		return -1;
	return (fid[4] == '2') ? 1 : 0;
}

/* Действие из имени: хвост после "uartN.". */
static const char *uart_fid_action(const char *fid)
{
	if (!fid)
		return "";
	const char *dot = strchr(fid, '.');
	return dot ? (dot + 1) : "";
}

static void handle_msg(app_context_t *app, webapi_msg_t *in_msg)
{
	struct app_uart_t *app_uart = NULL;
	uart_subscription_context_t *ctx = NULL;
	gw_uart_config_t new_cfg;
	uint32_t amount = 0;
	uint8_t tmp[UART_TMP_BUF_SIZE];
	/* Имя разбирается на порт и действие: префикс "uartN." задаёт порт, хвост —
	 * что делать. Сравнение полное: "uart2.rx" и "uart2.rx2" — разные вызовы. */
	const char *fid  = in_msg->fid;
	int         port = uart_fid_port(fid);
	const char *act  = uart_fid_action(fid);

	if (port < 0) {
		ws_hal_log_i(TAG, "UART have no FID (%s) handler\n", fid);
		return;
	}

	if (strcmp(act, "config") == 0) {
		app_uart = &app->uart.port[port];

		if (!in_msg->data)
			return;
		api_cmd_uart_t cmd = {0};
		if (parse_uart_params(in_msg->data, in_msg->len, &cmd)) {
			new_cfg = uart_config(app_uart, &cmd, sizeof(cmd));
			gw_jsonw_t w;
			format_uart_resp(&w, &new_cfg);
			send_uart_response(in_msg->id, fid, &w);
		}
		return;
	}

	if (strcmp(act, "rx") == 0) {
		ctx = &uart_context[port];

		ws_hal_mutex_take(ctx->lock, WS_HAL_WAIT_FOREVER);
		amount = ctx->amount;
		if (amount > 0) {
			mem_cpy(tmp, ctx->buf, amount);
			ctx->amount = 0;
		}
		ws_hal_mutex_give(ctx->lock);

		if (amount > 0) {
			int32_t b64_size = base64_encode_buffer_required(amount);
			//todo: fix. use malloc
			uint8_t b64_buf[b64_size + 1];
			int32_t b64_len = base64_encode(b64_buf, b64_size, tmp, amount);
			if (b64_len > 0) {
				b64_buf[b64_len] = '\0';
				/* Ответ — голая строка JSON с base64, без объекта */
				gw_jsonw_t w;
				gw_jsonw_init(&w, uart_raw_buf, sizeof(uart_raw_buf));
				gw_jsonw_str(&w, NULL, (char *)b64_buf);
				/* id=0: broadcast to all subscribers of this FID */
				send_uart_response(0, fid, &w);
			}
		} else {
			/* id=0: broadcast (empty poll — no data, no response needed) */
			send_uart_response(0, fid, NULL);
		}
		return;
	}

	if (strcmp(act, "tx") == 0) {
		ctx = &uart_context[port];

		if (in_msg->data && in_msg->len > 2) {
			uint8_t *b64 = in_msg->data + 1;       /* skip leading '"' */
			size_t b64_len = in_msg->len - 2;       /* strip both '"' */
			int32_t buf_size = base64_decode_buffer_required(b64, b64_len);

			if (buf_size > 0) {
				uint8_t buf[buf_size];
				int32_t decoded = base64_decode(buf, buf_size, b64, b64_len);

				if (decoded > 0) {
					int32_t written = gw_uart_write(&app->uart.port[port].desc, buf, decoded);

					gw_jsonw_t w;
					gw_jsonw_init(&w, uart_resp_buf, sizeof(uart_resp_buf));
					gw_jsonw_obj(&w, NULL);
					gw_jsonw_int(&w, "written", written);
					send_uart_response(in_msg->id, fid, &w);
				}
			}
		}
		return;
	}

	if (strcmp(act, "echo") == 0) {
		app_uart = &app->uart.port[port];

		if (in_msg->data && in_msg->len) {
			GW_JSON_TOKENS(toks, UART_ARG_MAX_TOKENS);
			gw_json_t json;
			int32_t val;
			if (gw_json_parse(&json, (const char *)in_msg->data, in_msg->len,
					  toks, UART_ARG_MAX_TOKENS)
			    && gw_json_int(&json, "E", &val))
				gw_uart_set_echo(&app_uart->desc, val ? 1 : 0);
		}
		{
			gw_jsonw_t w;
			gw_jsonw_init(&w, uart_resp_buf, sizeof(uart_resp_buf));
			gw_jsonw_obj(&w, NULL);
			gw_jsonw_int(&w, "E", gw_uart_get_echo(&app_uart->desc));
			send_uart_response(in_msg->id, fid, &w);
		}
		return;
	}

	ws_hal_log_i(TAG, "UART have no FID (%s) handler\n", fid);
}

//void api_handler_uart_work(app_context_t *app) {
void ws_uart_task(void *param) {
    webapi_msg_t *in_msg = NULL;
    app_context_t *app = param;

    for (;;) {
        if (ws_hal_queue_receive(cmd_queue, &in_msg, WS_HAL_WAIT_FOREVER) != 0) {
            handle_msg(app, in_msg);
            free(in_msg->data);
            free(in_msg);
            in_msg = NULL;
        }
    }
}

esp_err_t ws_uart_run(app_context_t *app)
{
        cmd_queue = ws_hal_queue_create(10, sizeof(void *));
        /* Порт и его данные доступны с любого интерфейса: политики у UART нет,
         * поэтому одна регистрация на маску «любой интерфейс». */
        ws_server_register_fid_queue(ESP_WS_API_UART1_CNF,    GW_IFACE_MASK_ANY, cmd_queue);
        ws_server_register_fid_queue(ESP_WS_API_UART1_RAW_RX, GW_IFACE_MASK_ANY, cmd_queue);
        ws_server_register_fid_queue(ESP_WS_API_UART1_RAW_TX, GW_IFACE_MASK_ANY, cmd_queue);
        ws_server_register_fid_queue(ESP_WS_API_UART1_ECHO,   GW_IFACE_MASK_ANY, cmd_queue);
        ws_server_register_fid_queue(ESP_WS_API_UART2_CNF,    GW_IFACE_MASK_ANY, cmd_queue);
        ws_server_register_fid_queue(ESP_WS_API_UART2_RAW_RX, GW_IFACE_MASK_ANY, cmd_queue);
        ws_server_register_fid_queue(ESP_WS_API_UART2_RAW_TX, GW_IFACE_MASK_ANY, cmd_queue);
        ws_server_register_fid_queue(ESP_WS_API_UART2_ECHO,   GW_IFACE_MASK_ANY, cmd_queue);

        uart_context[0].buf = uart1_buf;
        uart_context[0].amount = 0;
        uart_context[0].size = sizeof(uart1_buf);
        uart_context[0].lock = ws_hal_mutex_create();
        uart1_delegate.handler = &uart_event_on_rx;
        uart1_delegate.context = &uart_context[0];

        uart_context[1].buf = uart2_buf;
        uart_context[1].amount = 0;
        uart_context[1].size = sizeof(uart2_buf);
        uart_context[1].lock = ws_hal_mutex_create();
        uart2_delegate.context = &uart_context[1];
        uart2_delegate.handler = &uart_event_on_rx;

        gw_uart_on_receive_subscribe(&app->uart.port[0].desc, &uart1_delegate);
        gw_uart_on_receive_subscribe(&app->uart.port[1].desc, &uart2_delegate);

        ws_hal_task_create(ws_uart_task, "ws_uart", 4096, app, 5);
        return ESP_OK;
}

#ifdef APIH_UART_TEST
void apih_uart_test_init(void) {
    uart_context[0].buf    = uart1_buf;
    uart_context[0].size   = sizeof(uart1_buf);
    uart_context[0].amount = 0;
    uart_context[0].lock   = ws_hal_mutex_create();
    uart_context[1].buf    = uart2_buf;
    uart_context[1].size   = sizeof(uart2_buf);
    uart_context[1].amount = 0;
    uart_context[1].lock   = ws_hal_mutex_create();
}

void apih_uart_test_reset(void) {
    uart_context[0].amount = 0;
    uart_context[1].amount = 0;
}

void apih_uart_test_fill_rx(int port, const uint8_t *data, uint32_t len) {
    uart_subscription_context_t *ctx = &uart_context[port];
    uint32_t sz = ctx->size < len ? ctx->size : len;
    mem_cpy(ctx->buf, data, sz);
    ctx->amount = sz;
}

void apih_uart_test_handle_msg(app_context_t *app, webapi_msg_t *msg) {
    handle_msg(app, msg);
}
#endif

