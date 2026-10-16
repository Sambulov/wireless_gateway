#ifdef APIH_UART_TEST
#  include "apih_uart_app_types.h"
#else
#  include "app.h"
#endif
#include "uart.h"
#include "GwJson.h"

/* Токенов на разбор ARG UART: WL/BR/PAR/SB/MODE (или E) — с большим запасом. */
#define UART_ARG_MAX_TOKENS 16
#include "web_api.h"
#include "ws_hal.h"
#include "uart_rxbuf.h"

typedef struct {
    __linked_list_object__
    void *call;
    uint8_t *data;
    uint32_t size;
    uint8_t wl_set   : 1,
            boud_set : 1,
            par_set  : 1,
            sb_set   : 1,
            mode_set : 1,
            idle_set : 1;
    uint8_t port_no  : 2,
            wl       : 2,
            par      : 2,
            sb       : 2,
            /* Режимов три (обычный UART, RS-485, однопроводный) — одного бита
             * на них уже не хватало: значение 2 превращалось в 0. */
            mode     : 2,
            /* Порог простоя линии, знаков (1..255) */
            idle     : 8;
    uint32_t boud;
} api_cmd_uart_t;

ws_queue_t cmd_queue;

ws_queue_t get_uart_worker_queue(void)
{
	return cmd_queue;
}

typedef struct {
    /* Кольцо приёма: куски [метка][данные] с границами передач (см. uart_rxbuf.h).
     * Само кольцо (массив байт) — статический буфер этого файла, дескриптор здесь. */
    gw_uart_rxbuf_t rxbuf;
    uint8_t *storage;
    uint16_t storage_size;
    ws_mutex_t lock;
    /* Принятое уходит клиенту СРАЗУ по приходу, а не по опросу подписки: флаг
     * говорит, что кадр уже заказан, чтобы не будить задачу на те же байты
     * второй раз (иначе поток данных порождал бы по сообщению на байт). */
    volatile uint8_t pending;
    char fid[API_FID_NAME_MAX];
} uart_subscription_context_t;


#define UART_API_SEND_DELAY   20

/* ── Темп кадров приёма ──────────────────────────────────────────────────────
 *
 * Данные из порта уходят кадром не чаще, чем раз в UART_RX_FRAME_MS: если байты
 * сыплются чаще, сервис копит их и отправляет всё, что накопилось к сроку.
 * Смысл — не гнать по кадру на каждый байт (эхо набора в терминале давало 38
 * кадров на одну набранную строку, и рабочая очередь httpd захлёбывалась).
 *
 * Размер накопителя выбран от предельной скорости порта: 500000 бод — это
 * 50000 Б/с, то есть 3750 байт за 75 мс. 4096 > 3750 — при таком темпе терять
 * нечего. */
#define UART_RX_FRAME_MS      75
#define UART_RX_ACC_SIZE      4096
#define UART_B64_MAX          (4 * ((UART_RX_ACC_SIZE + 2) / 3))

/* Заказать задаче кадр: неблокирующая посылка в очередь. Делегат зовётся из
 * задачи событий UART, поэтому отправлять данные оттуда нельзя — только будить. */
static void uart_rx_wake(uart_subscription_context_t *ctx) {
    if (ctx->pending)
        return;
    webapi_msg_t *wake = calloc(1, sizeof(webapi_msg_t));
    if (!wake)
        return;
    bApiFidSet(wake->fid, ctx->fid);
    wake->id = 0;                  /* 0 — рассылка подписчикам FID */
    wake->fd = -1;                 /* сообщение периферии, не клиента */
    ctx->pending = 1;
    if (!ws_hal_queue_send(get_uart_worker_queue(), &wake, WS_HAL_WAIT_NONE)) {
        ctx->pending = 0;
        free(wake);
    }
}

static void uart_event_on_rx(void *event_trigger, void *sender, void *context) {
    (void)sender;
    uart_subscription_context_t *buf_desc = (uart_subscription_context_t *)context;
    gw_uart_event_data_t *data = (gw_uart_event_data_t *)event_trigger;

    /* Кусок кладём в кольцо вместе с признаком «линия замолчала» — по нему
     * читатель видит конец передачи. Признак и границы передач пишутся здесь,
     * в момент приёма, а не при отдаче: отдача может агрегировать сколько угодно
     * кусков в одно сообщение и ничего при этом не склеивает. */
    size_t sz = data->size;
    if (sz > 0xFFFF) sz = 0xFFFF;
    ws_hal_mutex_take(buf_desc->lock, WS_HAL_WAIT_FOREVER);
    uint16_t taken = gw_uart_rxbuf_push(&buf_desc->rxbuf, data->buf, (uint16_t)sz,
                                        data->b_end ? 1 : 0);
    ws_hal_mutex_give(buf_desc->lock);

    /* Потеря приёма — не молчаливая: о ней должно быть видно в логе, иначе
     * «терминал иногда теряет вывод» выглядит как загадка. */
    if(taken < sz)
        ws_hal_log_w(TAG, "UART rx: кольцо полно, потеряно %u байт\n",
                     (unsigned)(sz - taken));

    /* Будим задачу: она отправит принятое кадром сразу, не дожидаясь опроса
     * подписки. Раньше данные лежали в накопителе до следующего опроса, и
     * задержка вывода упиралась в период переспроса (страница просила 50 мс).
     * Делегат зовётся из задачи событий UART, поэтому здесь только неблокирующая
     * посылка в очередь — без ожиданий и без отправки из чужой задачи. */
    uart_rx_wake(buf_desc);
}


/* Буферы ответов статические: задача обслуживает запросы по одному, стек у неё
 * ограничен, а принятого бывает несколько килобайт. Ни накопители приёма, ни
 * массив под base64, ни декодирование передачи на стеке не живут (см. ниже).
 * Сборка идёт прямо в буфер, дерева и узлов нет. */
#define UART_RESP_MAX 96
static char uart_resp_buf[UART_RESP_MAX];

/* Буфер ответа приёма: куски кодируются в base64 и разделяются '#', плюс
 * обёртка {"RX":"…"}. Размер — как под одно самое большое содержимое кольца:
 * кусок в сообщение идёт только целиком, остальное уходит следующим.
 * Буферы СТАТИЧЕСКИЕ, а не на стеке задачи: принятого бывает до 4 КБ, то есть
 * base64 — больше 5 КБ, и такой буфер на стеке (4 КБ) ронял задачу. */
#define UART_RAW_OVERHEAD 64
static char uart_raw_buf[UART_B64_MAX + UART_RAW_OVERHEAD];
static char uart_b64_buf[UART_B64_MAX + 1];
/* Кусок, забранный из кольца: копируем под замком, кодируем уже без него —
 * делегат приёма (из задачи событий UART) ждать base64 не должен. */
static uint8_t uart_rx_snapshot[UART_RX_ACC_SIZE];

static void format_uart_resp(gw_jsonw_t *w, const gw_uart_config_t *cnf)
{
	gw_jsonw_init(w, uart_resp_buf, sizeof(uart_resp_buf));
	gw_jsonw_obj(w, NULL);
	gw_jsonw_int(w, "BR",  cnf->boud);
	gw_jsonw_int(w, "WL",  cnf->bits);
	gw_jsonw_int(w, "PAR", cnf->parity);
	gw_jsonw_int(w, "SB",  cnf->stop);
	/* Режим работы порта: 0 — обычный UART, 1 — RS-485 с автонаправлением */
	gw_jsonw_int(w, "MODE", cnf->mode);
	/* Порог простоя линии в знаках: сколько тишины считать концом кадра */
	gw_jsonw_int(w, "IDLE_CH", cnf->idle_ch);
}

static uint8_t uart1_buf[UART_RX_ACC_SIZE];
static uint8_t uart2_buf[UART_RX_ACC_SIZE];
/* Когда с каждого порта ушёл предыдущий кадр — по нему считается темп. */
static uint32_t uart_rx_frame_tick[2];
static uint8_t  uart_rx_frame_sent[2];
static uart_subscription_context_t uart_context[2];
static delegate_t uart1_delegate;
static delegate_t uart2_delegate;

/* Настройки порта: что просили — меняем, остальное (в том числе режим) оставляем
 * как есть. Изменения применяются ТОЛЬКО когда их просили: пустой ARG — это
 * чтение настроек, и дёргать драйвер (uart_param_config/uart_set_mode) на каждое
 * чтение незачем. */
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
    if(cfg->mode_set)
        gw_cfg.mode = cfg->mode;
    if(cfg->idle_set)
        gw_cfg.idle_ch = cfg->idle;

    if (cfg->boud_set || cfg->par_set || cfg->sb_set || cfg->wl_set || cfg->mode_set
        || cfg->idle_set)
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

/* Отвечает тем, что стоит на порту сейчас. Нужен там, где применять нечего:
 * вызов без ARG — это чтение, а неразобранный ARG применять нельзя. */
static void send_uart_state(int id, const char *fid, struct app_uart_t *app_uart)
{
	gw_uart_config_t cur = {0};
	gw_jsonw_t w;

	gw_uart_get(&app_uart->desc, &cur);
	format_uart_resp(&w, &cur);
	send_uart_response(id, fid, &w);
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
	if (cmd->boud_set) {
		/* Скорость вне диапазона драйвер не примет — отвергаем сразу, чтобы не
		 * отвечать «применено», когда на порту осталось прежнее значение. */
		if (val < 50 || (uint32_t)val > UART_MAX_SPEED) {
			ws_hal_log_i(TAG, "UART boud %d out of range, ignored\n", (int)val);
			cmd->boud_set = 0;
		} else {
			cmd->boud = val;
		}
	}
	cmd->par_set = gw_json_int(&json, "PAR", &val);
	if (cmd->par_set)
		cmd->par = val;
	cmd->sb_set = gw_json_int(&json, "SB", &val);
	if (cmd->sb_set)
		cmd->sb = val;
	/* IDLE_CH — порог «линия молчит», в знаках: сколько знаков тишины считать
	 * концом кадра. По нему драйвер выставляет признак конца, а страница делит
	 * поток данных на кадры. */
	cmd->idle_set = gw_json_int(&json, "IDLE_CH", &val);
	if (cmd->idle_set) {
		if (val >= 1 && val <= 255)
			cmd->idle = (uint8_t)val;
		else
			cmd->idle_set = 0;
	}
	cmd->mode_set = gw_json_int(&json, "MODE", &val);
	if (cmd->mode_set) {
		/* Режимы: 0 — обычный UART, 1 — RS-485 с автонаправлением (приём глушит
		 * драйвер), 2 — однопроводный UART (приём глушим сами: своё переданное
		 * возвращается эхом). Прочее не принимаем. */
		if (val == GW_UART_MODE_UART || val == GW_UART_MODE_RS485_HALF_DUPLEX
		    || val == GW_UART_MODE_UART_SINGLE_WIRE)
			cmd->mode = val;
		else
			cmd->mode_set = 0;
	}
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

		if (!in_msg->data || in_msg->len == 0) {
			/* Применять нечего: это чтение, и на него надо ответить.
			 * Раньше такой вызов уходил без ответа и висел до таймаута клиента. */
			send_uart_state(in_msg->id, fid, app_uart);
			return;
		}
		api_cmd_uart_t cmd = {0};
		if (!parse_uart_params(in_msg->data, in_msg->len, &cmd)) {
			/* ARG не разобран — применять его нельзя, но и молчать нечем:
			 * отвечаем текущим состоянием, о причине пишем в журнал. */
			ws_hal_log_i(TAG, "UART config ARG not parsed (%s), state sent\n", fid);
			send_uart_state(in_msg->id, fid, app_uart);
			return;
		}
		{
			/* Что стоит на порту ДО применения — чтобы отличить настоящую
			 * смену настроек от записи тех же самых значений. */
			gw_uart_config_t before = {0};
			gw_uart_get(&app_uart->desc, &before);

			new_cfg = uart_config(app_uart, &cmd, sizeof(cmd));
			gw_jsonw_t w;
			format_uart_resp(&w, &new_cfg);

			/* Ответ тому, кто просил */
			send_uart_response(in_msg->id, fid, &w);

			/* И рассылка подписчикам — ТОЛЬКО если настройки действительно
			 * изменились: смена режима или скорости обесценивает работу
			 * остальных (данные пойдут на других настройках), поэтому узнать об
			 * этом надо сразу, а не по переспросу. Запись тех же значений ничего
			 * не меняет, и будить ею всех подписчиков незачем: терминал не должен
			 * отключаться из-за чужого «применить то же самое».
			 * Буфер один на всех — как и у данных порта. */
			if (before.boud != new_cfg.boud || before.bits != new_cfg.bits
			    || before.parity != new_cfg.parity || before.stop != new_cfg.stop
			    || before.mode != new_cfg.mode || before.idle_ch != new_cfg.idle_ch)
				send_uart_response(0, fid, &w);
		}
		return;
	}

	if (strcmp(act, "rx") == 0) {
		ctx = &uart_context[port];

		/* Флаг снимаем ДО отправки: если во время неё придут новые байты,
		 * делегат закажет следующий кадр сам. */
		ctx->pending = 0;

		/* Есть ли что отдавать — спрашиваем, не дожидаясь: пустой опрос
		 * (подписка без данных) должен отвечать сразу, а не держать темп. */
		ws_hal_mutex_take(ctx->lock, WS_HAL_WAIT_FOREVER);
		uint8_t ready = gw_uart_rxbuf_peek(&ctx->rxbuf, NULL, NULL, NULL);
		ws_hal_mutex_give(ctx->lock);

		if (!ready) {
			/* id=0: пустой опрос — данных нет, ответа нет */
			send_uart_response(0, fid, NULL);
			return;
		}

		/* Держим темп кадров: если предыдущий ушёл только что, ждём до срока —
		 * за это время накопится ещё, и всё уйдёт ОДНИМ сообщением. Границы
		 * передач при этом не теряются: они уже лежат в кольце метками. */
		if (uart_rx_frame_sent[port]) {
			uint32_t gap  = ws_hal_ms_to_ticks(UART_RX_FRAME_MS);
			int32_t  left = (int32_t)(uart_rx_frame_tick[port] + gap - ws_hal_tick());
			if (left > 0)
				ws_hal_delay_ms(ws_hal_ticks_to_ms((uint32_t)left));
		}

		/* Забираем готовые куски и складываем их в ОДНО сообщение. Кусок идёт в
		 * сообщение только целиком: забранный вернуть в кольцо нельзя, поэтому
		 * сначала смотрим (peek), влезает ли, и лишь потом забираем. Не влезло —
		 * уйдёт следующим сообщением, его отправим сразу, не ожидая темпа. */
		size_t out_len = 0;
		uint8_t lost = 0, leftovers = 0;
		int pieces = 0;
		/* Разделители в данных — символы, которых не бывает в base64:
		 * '#' — передача на линии закончилась (idle), '~' — здесь потеряли
		 * часть данных. Ставятся прямо в поток, между кусками base64. */
		for (;;) {
			uint16_t piece_len = 0;
			uint8_t  frame_end = 0, piece_lost = 0;
			int32_t  b64_size;

			ws_hal_mutex_take(ctx->lock, WS_HAL_WAIT_FOREVER);
			if (!gw_uart_rxbuf_peek(&ctx->rxbuf, &piece_len, &frame_end, &piece_lost)) {
				ws_hal_mutex_give(ctx->lock);
				break;
			}
			b64_size = base64_encode_buffer_required(piece_len);
			/* Запас на два разделителя: '~' перед куском и '#' после него. */
			if (out_len + (size_t)b64_size + 3 > sizeof(uart_b64_buf)) {
				leftovers = 1;               /* не влезло — следующим сообщением */
				ws_hal_mutex_give(ctx->lock);
				break;
			}
			gw_uart_rxbuf_piece_t piece;
			if (!gw_uart_rxbuf_take(&ctx->rxbuf, (uint8_t *)uart_rx_snapshot,
						sizeof(uart_rx_snapshot), &piece)) {
				leftovers = 1;               /* гонка с приёмом: попробуем ещё раз */
				ws_hal_mutex_give(ctx->lock);
				break;
			}
			ws_hal_mutex_give(ctx->lock);

			/* '~' — перед куском, следующим за потерей: он стоит ровно там,
			 * где данные пропали. */
			if (piece.lost_before) {
				uart_b64_buf[out_len++] = '~';
				lost = 1;
			}
			int32_t b64_len = base64_encode((uint8_t *)&uart_b64_buf[out_len], b64_size,
							uart_rx_snapshot, piece.len);
			if (b64_len <= 0)
				break;
			out_len += (size_t)b64_len;
			/* '#' — ПОСЛЕ куска, которым закончилась передача на линии. Кусок с
			 * признаком «продолжение» идёт без разделителя. */
			if (piece.frame_end)
				uart_b64_buf[out_len++] = '#';
			pieces++;
		}

		if (pieces > 0) {
			uart_b64_buf[out_len] = '\0';
			uart_rx_frame_tick[port] = ws_hal_tick();
			uart_rx_frame_sent[port] = 1;

			/* Данные одним куском текста: '#' — конец передачи на линии, '~' —
			 * потеря данных здесь. Кусок без разделителя — продолжение
			 * предыдущего (длинный кадр выгружается частями). */
			gw_jsonw_t w;
			gw_jsonw_init(&w, uart_raw_buf, sizeof(uart_raw_buf));
			gw_jsonw_obj(&w, NULL);
			gw_jsonw_str(&w, "RX", uart_b64_buf);
			/* id=0: broadcast to all subscribers of this FID */
			send_uart_response(0, fid, &w);

			if (lost)
				ws_hal_log_w(TAG, "UART rx: приём терялся, клиенту сообщено\n");
		}

		/* Осталось на потом — будим задачу сразу, не ожидая темпа: иначе данные
		 * пролежат ещё кадр темпа без всякой причины. */
		if (leftovers)
			uart_rx_wake(ctx);

		return;
	}

	if (strcmp(act, "tx") == 0) {
		ctx = &uart_context[port];

		if (in_msg->data && in_msg->len > 2) {
			/* ARG приходит срезом исходного JSON. jsmn отдаёт строку БЕЗ
			 * кавычек, а прежний разбор отдавал вместе с ними — принимаем оба
			 * вида. Раньше здесь безусловно срезались первый и последний
			 * символ, поэтому на нынешнем разборе base64 портился, декодер
			 * отвечал отказом и в порт не уходило НИЧЕГО: терминал молчал, а
			 * ответ клиенту не отправлялся (проверено: TX не отвечал ни на
			 * одну посылку). */
			uint8_t *b64 = in_msg->data;
			size_t   b64_len = in_msg->len;
			if (b64_len >= 2 && b64[0] == '"' && b64[b64_len - 1] == '"') {
				b64++;
				b64_len -= 2;
			}
			int32_t buf_size = base64_decode_buffer_required(b64, b64_len);

			if (buf_size > 0) {
				/* Декодируем в КУЧУ: нагрузка вызова доходит до предела сообщения
				 * транспорта (16 КБ), а буфер такого размера на стеке задачи (4 КБ)
				 * её ронял — в логе платы при вставке 3 КБ в терминал:
				 * «***ERROR*** A stack overflow in task ws_uart has been detected». */
				uint8_t *buf = malloc((size_t)buf_size);
				if (!buf) {
					gw_jsonw_t w;
					gw_jsonw_init(&w, uart_resp_buf, sizeof(uart_resp_buf));
					gw_jsonw_obj(&w, NULL);
					gw_jsonw_int(&w, "written", -1);
					send_uart_response(in_msg->id, fid, &w);
					return;
				}
				int32_t decoded = base64_decode(buf, buf_size, b64, b64_len);

				if (decoded > 0) {
					int32_t written = gw_uart_write(&app->uart.port[port].desc, buf, decoded);

					gw_jsonw_t w;
					gw_jsonw_init(&w, uart_resp_buf, sizeof(uart_resp_buf));
					gw_jsonw_obj(&w, NULL);
					gw_jsonw_int(&w, "written", written);
					send_uart_response(in_msg->id, fid, &w);
					free(buf);
					return;
				}
				free(buf);
				ws_hal_log_w(TAG, "UART tx: base64 broken, len %u\n", (unsigned)b64_len);
			}
			/* Данные не распознаны — всё равно отвечаем: молчание клиент ждёт
			 * до своего таймаута и не понимает, что посылка не ушла. */
			gw_jsonw_t w;
			gw_jsonw_init(&w, uart_resp_buf, sizeof(uart_resp_buf));
			gw_jsonw_obj(&w, NULL);
			gw_jsonw_int(&w, "written", 0);
			send_uart_response(in_msg->id, fid, &w);
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

        uart_context[0].storage      = uart1_buf;
        uart_context[0].storage_size = sizeof(uart1_buf);
        gw_uart_rxbuf_init(&uart_context[0].rxbuf, uart_context[0].storage,
                           uart_context[0].storage_size);
        uart_context[0].lock = ws_hal_mutex_create();
        bApiFidSet(uart_context[0].fid, ESP_WS_API_UART1_RAW_RX);
        uart1_delegate.handler = &uart_event_on_rx;
        uart1_delegate.context = &uart_context[0];

        uart_context[1].storage      = uart2_buf;
        uart_context[1].storage_size = sizeof(uart2_buf);
        gw_uart_rxbuf_init(&uart_context[1].rxbuf, uart_context[1].storage,
                           uart_context[1].storage_size);
        uart_context[1].lock = ws_hal_mutex_create();
        bApiFidSet(uart_context[1].fid, ESP_WS_API_UART2_RAW_RX);
        uart2_delegate.context = &uart_context[1];
        uart2_delegate.handler = &uart_event_on_rx;

        gw_uart_on_receive_subscribe(&app->uart.port[0].desc, &uart1_delegate);
        gw_uart_on_receive_subscribe(&app->uart.port[1].desc, &uart2_delegate);

        /* Стек с запасом: раньше здесь стояли буферы приёма и декодирования
         * (до 4 КБ каждый), из-за чего задача падала на длинной посылке. Сами
         * буферы теперь статические, но base64 и сборщик ответа тоже едят стек. */
        ws_hal_task_create(ws_uart_task, "ws_uart", 5120, app, 5);
        return ESP_OK;
}

#ifdef APIH_UART_TEST
void apih_uart_test_init(void) {
    uart_context[0].storage      = uart1_buf;
    uart_context[0].storage_size = sizeof(uart1_buf);
    uart_context[0].lock         = ws_hal_mutex_create();
    uart_context[1].storage      = uart2_buf;
    uart_context[1].storage_size = sizeof(uart2_buf);
    uart_context[1].lock         = ws_hal_mutex_create();
    apih_uart_test_reset();
}

void apih_uart_test_reset(void) {
    for (int i = 0; i < 2; i++) {
        gw_uart_rxbuf_init(&uart_context[i].rxbuf, uart_context[i].storage,
                           uart_context[i].storage_size);
        uart_context[i].pending = 0;
    }
    uart_rx_frame_sent[0] = 0;
    uart_rx_frame_sent[1] = 0;
}

/* «Данные готовы к отдаче» — это законченная передача на линии: незакрытый
 * кусок читателю не отдаётся, пока не перевалит за порог частичной выгрузки. */
void apih_uart_test_fill_rx(int port, const uint8_t *data, uint32_t len) {
    apih_uart_test_fill_rx_idle(port, data, len, 0, 1);
}

/* idle — «линия замолчала после этих байт», то есть передача закончилась.
 * Простоя в буфере больше нет: границы передач хранит сама метка куска. */
void apih_uart_test_fill_rx_idle(int port, const uint8_t *data, uint32_t len,
                                 uint32_t gap_us, uint8_t idle) {
    uart_subscription_context_t *ctx = &uart_context[port];
    (void)gap_us;
    if (len > 0xFFFF) len = 0xFFFF;
    gw_uart_rxbuf_push(&ctx->rxbuf, data, (uint16_t)len, idle ? 1 : 0);
}

void apih_uart_test_handle_msg(app_context_t *app, webapi_msg_t *msg) {
    handle_msg(app, msg);
}
#endif

