#ifdef APIH_UART_TEST
#  include "apih_uart_app_types.h"
#else
#  include "app.h"
#endif
#include "uart.h"
#include "GwJson.h"

/* Токенов на разбор ARG UART: WL/BR/PAR/SB/MODE (или E) — с большим запасом. */
#define UART_ARG_MAX_TOKENS 16
#include "api.h"
#include "platform.h"
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
            /* Режимов три (обычный UART, RS-485, однопроводный), поэтому под
             * них два бита: на одном бите значение 2 превращалось бы в 0. */
            mode     : 2,
            /* Порог простоя линии, знаков (1..255) */
            idle     : 8;
    uint32_t boud;
} api_cmd_uart_t;

/* Ядра API, чьи подписчики получают кадры порта. Сервис UART зарегистрирован на
 * ОБОИХ ядрах (точка доступа и станция), поэтому кадр приёма и смена настроек
 * рассылаются по имени FID на каждом из них — публичным вызовом рассылки
 * (api_broadcast), а ядра для этого сервис хранит сам. Ответ на запрос уходит
 * тому вызову, который его принёс, — по дескриптору вызова. */
#define UART_API_MAX 2
static api_t *uart_apis[UART_API_MAX];
static uint8_t uart_api_num;

static void uart_apis_set(api_t *first, api_t *second)
{
	uart_api_num = 0;
	uart_apis[uart_api_num++] = first;
	if (second != NULL)
		uart_apis[uart_api_num++] = second;
}

/* ── Обработчики FID ────────────────────────────────────────────────────────
 *
 * Запрос сервису ядро доставляет ВЫЗОВОМ ОБРАБОТЧИКА (очередей у ядра больше
 * нет): обработчик либо отвечает сразу и возвращает FALSE, либо возвращает TRUE
 * и получает вызов заново на следующем такте. Своей задачи у сервиса нет:
 * такт ему даёт задача приложения, которая крутит api_worker.
 *
 * Действие известно по РЕГИСТРАЦИИ, а не по разбору имени: обработчик получает
 * контекст регистрации, а в нём — порт, действие и имя FID (имя нужно для
 * рассылки, у вызова его не спросить: ядро имени не выдаёт, ответ и так уходит
 * с тем же FID, что прислал клиент). */
typedef enum {
	UART_ACT_CNF = 0,
	UART_ACT_RX,
	UART_ACT_TX,
	UART_ACT_ECHO,
	UART_ACT_COUNT
} uart_act_t;

typedef struct {
	app_context_t *app;
	const char    *fid;   /* имя FID этой регистрации (строковый литерал app.h) */
	uint8_t        port;  /* 0 — "uart1.*", 1 — "uart2.*" */
	uint8_t        act;   /* uart_act_t */
} uart_handler_ctx_t;

/* По контексту на каждую пару «порт × действие». Контексты общие для обоих ядер:
 * обработчик их только читает, а состояние порта (кольцо приёма) и так общее. */
static uart_handler_ctx_t uart_handler_ctx[2][UART_ACT_COUNT];

/* Замок СБОРКИ КАДРА — один на оба порта: буферы base64, снимок куска и буфер
 * кадра статические и общие. Собирать кадр могут две задачи: задача приложения
 * (обработчик FID в такте ядра) и задача событий UART (приём пришёл сам).
 * Прядок захвата везде один — сначала этот замок, потом замок порта, — поэтому
 * двойного захвата в обратном порядке не бывает. */
static platform_mutex_t uart_frame_lock;

typedef struct {
    /* Кольцо приёма: куски [метка][данные] с границами передач (см. uart_rxbuf.h).
     * Само кольцо (массив байт) — статический буфер этого файла, дескриптор здесь. */
    gw_uart_rxbuf_t rxbuf;
    uint8_t *storage;
    uint16_t storage_size;
    platform_mutex_t lock;
    uint8_t port;               /* 0 — "uart1", 1 — "uart2" */
    /* Ядро — не здесь, а в uart_apis: сервис зарегистрирован на обоих ядрах, и
     * кадр приёма рассылается подписчикам каждого из них. */
} uart_subscription_context_t;

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

/* Кадр приёма собирает и отдаёт подписчикам САМА задача приёма порта: это
 * внутреннее дело сервиса, и ядру для этого ничего не нужно — рассылка
 * api_broadcast публичная. Такт ядра тоже может собрать кадр (клиент
 * спросил порт вызовом), поэтому сборка защищена замком сборки — см. uart_rx_flush.
 *
 * Возврат: TRUE — вызов надо повторить (темп кадров ещё не вышел), FALSE —
 * отдавать больше нечего. */
static uint8_t uart_rx_flush(uart_handler_ctx_t *hc);

static void uart_event_on_rx(void *event_trigger, void *sender, void *context) {
    (void)sender;
    uart_subscription_context_t *buf_desc = (uart_subscription_context_t *)context;
    gw_uart_event_data_t *data = (gw_uart_event_data_t *)event_trigger;
    if (!buf_desc || !data || !data->buf || !data->size)
        return;

    /* Кусок кладём в кольцо вместе с признаком «линия замолчала» — по нему
     * читатель видит конец передачи. Признак и границы передач пишутся здесь,
     * в момент приёма, а не при отдаче: отдача может агрегировать сколько угодно
     * кусков в одно сообщение и ничего при этом не склеивает. */
    size_t sz = data->size;
    if (sz > 0xFFFF) sz = 0xFFFF;
    uint16_t taken;
    platform_mutex_take(uart_frame_lock, PLATFORM_WAIT_FOREVER);
    platform_mutex_take(buf_desc->lock, PLATFORM_WAIT_FOREVER);
    taken = gw_uart_rxbuf_push(&buf_desc->rxbuf, data->buf, (uint16_t)sz,
                               data->b_end ? 1 : 0);
    platform_mutex_give(buf_desc->lock);
    /* Кадр уходит сразу, не дожидаясь опроса подписки: иначе вывод упирался бы
     * в период переспроса (страница просит 50 мс). Ожиданий здесь нет: если темп
     * кадров ещё не вышел, данные останутся в кольце до следующего куска или
     * опроса. */
    (void)uart_rx_flush(&uart_handler_ctx[buf_desc->port][UART_ACT_RX]);
    platform_mutex_give(uart_frame_lock);

    /* Потеря приёма — не молчаливая: о ней должно быть видно в логе, иначе
     * «терминал иногда теряет вывод» выглядит как загадка. */
    if(taken < sz)
        platform_log_w(TAG, "UART rx: кольцо полно, потеряно %u байт\n",
                     (unsigned)(sz - taken));
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
/* Запас +4 — под разделитель '#' и '\0': кусок в кадр идёт только целиком, и
 * самое большое содержимое кольца обязано влезть в ОДИН кадр, иначе данные
 * застряли бы в накопителе навсегда. */
static char uart_raw_buf[UART_B64_MAX + UART_RAW_OVERHEAD];
static char uart_b64_buf[UART_B64_MAX + 4];
/* Кусок, забранный из кольца: копируем под замком порта, кодируем уже без него —
 * приём (задача событий UART) ждать base64 не должен. */
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
    (void)new_cfg_len;
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

/* ── Ответы ─────────────────────────────────────────────────────────────────
 *
 * Ответ на ЗАПРОС уходит тому вызову, который его принёс: api_reply сам
 * оборачивает данные в конверт {"FID":<то же имя>,"SID":<SID клиента>} и
 * отправляет в соединение этого вызова (ядро берётся из самого вызова). Ждать
 * ничего не надо — отправка ставится в очередь транспорта.
 *
 * РАССЫЛКА (кадр приёма порта, смена настроек) адресована не вызову, а всем
 * подписчикам имени, и у каждого ядра свои. Это публичный вызов ядра
 * api_broadcast: он находит подписчиков ЭТОГО ядра и отправляет им
 * ОДИН общий буфер — копии на каждого не собираются. Ядро сервис передаёт сам
 * (см. uart_apis): рассылающего обработчика ядро не знает.
 *
 * Оба пути неблокирующие: обработчик, вызванный тактом ядра, не имеет права
 * ждать, а рассылка из задачи приёма UART — тем более. */

/* Собранный JSON — вызову (call != NULL) или рассылкой (call == NULL: по всем
 * ядрам сервиса). */
static void uart_send_to(void *call, const char *fid, gw_jsonw_t *w)
{
	const char *json = NULL;
	size_t      len  = 0;

	if (w) {
		uint8_t room = gw_jsonw_ok(w);
		size_t  done = gw_jsonw_finish(w);
		if (room && done) {
			json = w->buf;
			len  = done;
		}
	}
	if (!json || !len)
		return;                 /* отвечать нечем — и кадра нет */

	if (call != NULL) {
		api_reply(call, (const uint8_t *)json, len);
		return;
	}
	for (uint8_t i = 0; i < uart_api_num; i++)
		api_broadcast(uart_apis[i], fid, (const uint8_t *)json, len);
}

/* Отвечает тем, что стоит на порту сейчас. Нужен там, где применять нечего:
 * вызов без ARG — это чтение, а неразобранный ARG применять нельзя. */
static void send_uart_state(void *call, const char *fid,
			    struct app_uart_t *app_uart)
{
	gw_uart_config_t cur = {0};
	gw_jsonw_t w;

	gw_uart_get(&app_uart->desc, &cur);
	format_uart_resp(&w, &cur);
	uart_send_to(call, fid, &w);
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
		 * поле в структуре двухбитное, поэтому 8 надо перевести в
		 * перечисление, иначе оно превратится в 0 и порт молча
		 * переключится на 7 бит данных. */
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
		 * отвечать «применено», когда на порту значение не изменилось. */
		if (val < 50 || (uint32_t)val > UART_MAX_SPEED) {
			platform_log_i(TAG, "UART boud %d out of range, ignored\n", (int)val);
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

/* ── Обработчик FID ─────────────────────────────────────────────────────────
 *
 * Контракт ядра: обработчик либо отвечает сразу и возвращает FALSE, либо
 * возвращает TRUE — и ядро позовёт его снова на следующем такте. Ждать внутри
 * нельзя: такт ядра один, и «подождать» для него значит вернуть управление.
 * Поэтому все действия быстрые и делаются прямо здесь:
 *   config — применить/прочитать настройки порта;
 *   tx     — декодировать base64 и записать в порт;
 *   echo   — включить/прочитать эхо;
 *   rx     — забрать накопленное из кольца и разослать кадром (если темп кадров
 *            ещё не вышел — вернуть TRUE, это и есть «подождать»).
 *
 * data == NULL — это такт работы, а не запрос клиента: делать по нему что-то
 * есть смысл только приёму (накопленное ждёт темпа). */

/* Кадр приёма: забрать из кольца столько кусков, сколько влезает в один кадр, и
 * разослать подписчикам этого FID на всех ядрах. Мьютексы рекурсивные, поэтому
 * вложенный захват замков (задача приёма уже держит их) безопасен. */
static uint8_t uart_rx_flush_locked(uart_handler_ctx_t *hc)
{
	uart_subscription_context_t *ctx = &uart_context[hc->port];
	const int port = hc->port;

	/* Есть ли что отдавать — спрашиваем, не дожидаясь: пустой опрос
	 * (подписка без данных) должен отвечать сразу, а не держать темп. */
	platform_mutex_take(ctx->lock, PLATFORM_WAIT_FOREVER);
	uint8_t ready = gw_uart_rxbuf_peek(&ctx->rxbuf, NULL, NULL, NULL);
	platform_mutex_give(ctx->lock);
	if (!ready)
		return 0;

	/* Темп кадров: не чаще раза в UART_RX_FRAME_MS. Не вышел срок — НЕ ЖДЁМ
	 * (обработчик не имеет права спать), а просим ещё такт: вызов останется
	 * живым и вернётся сюда следующим тактом задачи приложения. */
	if (uart_rx_frame_sent[port]) {
		uint32_t gap  = platform_ms_to_ticks(UART_RX_FRAME_MS);
		int32_t  left = (int32_t)(uart_rx_frame_tick[port] + gap - platform_tick());
		if (left > 0)
			return 1;
	}

	/* Забираем готовые куски и складываем их в ОДИН кадр. Кусок идёт в кадр
	 * только целиком: забранный вернуть в кольцо нельзя, поэтому сначала
	 * смотрим (peek), влезает ли, и лишь потом забираем. Не влезло — уйдёт
	 * следующим кадром сразу же: это не «лишний» кадр, а выгрузка того, что не
	 * поместилось, и темпа она не ждёт. */
	for (;;) {
		size_t  out_len = 0;
		uint8_t lost = 0, dropped = 0;
		int     pieces = 0;

		/* Разделители в данных — символы, которых не бывает в base64:
		 * '#' — передача на линии закончилась (idle), '~' — здесь потеряли
		 * часть данных. Ставятся прямо в поток, между кусками base64. */
		for (;;) {
			uint16_t piece_len = 0;
			uint8_t  frame_end = 0, piece_lost = 0;
			int32_t  b64_size;

			platform_mutex_take(ctx->lock, PLATFORM_WAIT_FOREVER);
			if (!gw_uart_rxbuf_peek(&ctx->rxbuf, &piece_len, &frame_end, &piece_lost)) {
				platform_mutex_give(ctx->lock);
				break;
			}
			b64_size = base64_encode_buffer_required(piece_len);
			/* Запас на два разделителя: '~' перед куском и '#' после него. */
			if (out_len + (size_t)b64_size + 3 > sizeof(uart_b64_buf)) {
				/* Кусок не влезает в ПУСТОЙ кадр — такого быть не должно
				 * (буфер рассчитан на всё кольцо), но застрять на нём навсегда
				 * хуже, чем потерять его с записью в журнал. */
				if (pieces == 0) {
					gw_uart_rxbuf_piece_t big;
					if (gw_uart_rxbuf_take(&ctx->rxbuf, (uint8_t *)uart_rx_snapshot,
								sizeof(uart_rx_snapshot), &big))
						dropped = 1;
				}
				platform_mutex_give(ctx->lock);
				break;                       /* остальное — следующим кадром */
			}
			gw_uart_rxbuf_piece_t piece;
			if (!gw_uart_rxbuf_take(&ctx->rxbuf, (uint8_t *)uart_rx_snapshot,
						sizeof(uart_rx_snapshot), &piece)) {
				platform_mutex_give(ctx->lock);
				break;                       /* гонка с приёмом: попробуем позже */
			}
			platform_mutex_give(ctx->lock);

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

		if (dropped)
			platform_log_w(TAG, "UART rx: кусок больше кадра, потерян\n");

		if (pieces == 0)
			break;                           /* отдавать больше нечего */

		uart_b64_buf[out_len] = '\0';
		uart_rx_frame_tick[port] = platform_tick();
		uart_rx_frame_sent[port] = 1;

		/* Данные одним куском текста: '#' — конец передачи на линии, '~' —
		 * потеря данных здесь. Кусок без разделителя — продолжение
		 * предыдущего (длинный кадр выгружается частями). Адресат — подписчики
		 * этого FID на каждом ядре: кадр порта не принадлежит ни одному
		 * конкретному вызову. */
		gw_jsonw_t w;
		gw_jsonw_init(&w, uart_raw_buf, sizeof(uart_raw_buf));
		gw_jsonw_obj(&w, NULL);
		gw_jsonw_str(&w, "RX", uart_b64_buf);
		uart_send_to(NULL, hc->fid, &w);

		if (lost)
			platform_log_w(TAG, "UART rx: приём терялся, клиенту сообщено\n");

		/* Кольцо опустело — кадр был последним. */
		platform_mutex_take(ctx->lock, PLATFORM_WAIT_FOREVER);
		uint8_t more = gw_uart_rxbuf_peek(&ctx->rxbuf, NULL, NULL, NULL);
		platform_mutex_give(ctx->lock);
		if (!more)
			break;
	}

	return 0;
}

/* Обёртка: сборка кадра всегда идёт под замком сборки (см. uart_frame_lock) —
 * так статические буферы не делят между собой задача приёма и задача приложения. */
static uint8_t uart_rx_flush(uart_handler_ctx_t *hc)
{
	platform_mutex_take(uart_frame_lock, PLATFORM_WAIT_FOREVER);
	uint8_t more = uart_rx_flush_locked(hc);
	platform_mutex_give(uart_frame_lock);
	return more;
}

/* Настройки порта: что просили — меняем, остальное (в том числе режим) оставляем
 * как есть. Пустой ARG — это чтение, и на него надо ответить: молчание клиент
 * ждёт до своего таймаута. */
static uint8_t uart_cmd_config(uart_handler_ctx_t *hc, void *call,
			       const uint8_t *data, uint32_t len)
{
	struct app_uart_t *app_uart = &hc->app->uart.port[hc->port];

	if (!data || len == 0) {
		send_uart_state(call, hc->fid, app_uart);
		return 0;
	}
	api_cmd_uart_t cmd = {0};
	if (!parse_uart_params(data, len, &cmd)) {
		/* ARG не разобран — применять его нельзя, но и молчать нечем: отвечаем
		 * текущим состоянием, о причине пишем в журнал. */
		platform_log_i(TAG, "UART config ARG not parsed (%s), state sent\n", hc->fid);
		send_uart_state(call, hc->fid, app_uart);
		return 0;
	}

	/* Что стоит на порту ДО применения — чтобы отличить настоящую смену
	 * настроек от записи тех же самых значений. */
	gw_uart_config_t before = {0};
	gw_uart_get(&app_uart->desc, &before);

	gw_uart_config_t new_cfg = uart_config(app_uart, &cmd, sizeof(cmd));
	gw_jsonw_t w;
	format_uart_resp(&w, &new_cfg);

	/* Ответ тому, кто просил */
	uart_send_to(call, hc->fid, &w);

	/* И рассылка подписчикам — ТОЛЬКО если настройки действительно изменились:
	 * смена режима или скорости обесценивает работу остальных (данные пойдут на
	 * других настройках), поэтому узнать об этом надо сразу, а не по переспросу.
	 * Запись тех же значений ничего не меняет, и будить ею всех подписчиков
	 * незачем: терминал не должен отключаться из-за чужого «применить то же
	 * самое». Буфер один на всех — как и у данных порта. */
	if (before.boud != new_cfg.boud || before.bits != new_cfg.bits
	    || before.parity != new_cfg.parity || before.stop != new_cfg.stop
	    || before.mode != new_cfg.mode || before.idle_ch != new_cfg.idle_ch)
		uart_send_to(NULL, hc->fid, &w);
	return 0;
}

/* Передача в порт: ARG — base64 одной строкой. */
static uint8_t uart_cmd_tx(uart_handler_ctx_t *hc, void *call,
			   const uint8_t *data, uint32_t len)
{
	if (!data || len <= 2)
		return 0;

	/* ARG приходит срезом исходного JSON: jsmn отдаёт строку БЕЗ кавычек.
	 * Принимаем и вариант с кавычками — определяем его по первому и последнему
	 * символу. Безусловно срезать крайние символы нельзя: на срезе без кавычек
	 * base64 испортится, декодер ответит отказом и в порт не уйдёт ничего. */
	const uint8_t *b64 = data;
	size_t   b64_len = len;
	if (b64_len >= 2 && b64[0] == '"' && b64[b64_len - 1] == '"') {
		b64++;
		b64_len -= 2;
	}
	int32_t buf_size = base64_decode_buffer_required(b64, b64_len);

	if (buf_size > 0) {
		/* Декодируем в КУЧУ: нагрузка вызова доходит до предела сообщения
		 * транспорта (16 КБ), а буфер такого размера на стеке задачи (4 КБ) её
		 * ронял — в логе платы при вставке 3 КБ в терминал:
		 * «***ERROR*** A stack overflow in task ws_uart has been detected». */
		uint8_t *buf = malloc((size_t)buf_size);
		if (!buf) {
			gw_jsonw_t w;
			gw_jsonw_init(&w, uart_resp_buf, sizeof(uart_resp_buf));
			gw_jsonw_obj(&w, NULL);
			gw_jsonw_int(&w, "written", -1);
			uart_send_to(call, hc->fid, &w);
			return 0;
		}
		int32_t decoded = base64_decode(buf, buf_size, b64, b64_len);

		if (decoded > 0) {
			int32_t written = gw_uart_write(&hc->app->uart.port[hc->port].desc, buf, decoded);

			gw_jsonw_t w;
			gw_jsonw_init(&w, uart_resp_buf, sizeof(uart_resp_buf));
			gw_jsonw_obj(&w, NULL);
			gw_jsonw_int(&w, "written", written);
			uart_send_to(call, hc->fid, &w);
			free(buf);
			return 0;
		}
		free(buf);
		platform_log_w(TAG, "UART tx: base64 broken, len %u\n", (unsigned)b64_len);
	}
	/* Данные не распознаны — всё равно отвечаем: молчание клиент ждёт до своего
	 * таймаута и не понимает, что посылка не ушла. */
	gw_jsonw_t w;
	gw_jsonw_init(&w, uart_resp_buf, sizeof(uart_resp_buf));
	gw_jsonw_obj(&w, NULL);
	gw_jsonw_int(&w, "written", 0);
	uart_send_to(call, hc->fid, &w);
	return 0;
}

/* Эхо порта: включить/выключить и ответить текущим состоянием. */
static uint8_t uart_cmd_echo(uart_handler_ctx_t *hc, void *call,
			     const uint8_t *data, uint32_t len)
{
	struct app_uart_t *app_uart = &hc->app->uart.port[hc->port];

	if (data && len) {
		GW_JSON_TOKENS(toks, UART_ARG_MAX_TOKENS);
		gw_json_t json;
		int32_t val;
		if (gw_json_parse(&json, (const char *)data, len,
				  toks, UART_ARG_MAX_TOKENS)
		    && gw_json_int(&json, "E", &val))
			gw_uart_set_echo(&app_uart->desc, val ? 1 : 0);
	}
	gw_jsonw_t w;
	gw_jsonw_init(&w, uart_resp_buf, sizeof(uart_resp_buf));
	gw_jsonw_obj(&w, NULL);
	gw_jsonw_int(&w, "E", gw_uart_get_echo(&app_uart->desc));
	uart_send_to(call, hc->fid, &w);
	return 0;
}

static uint8_t uart_api_handler(void *call, void **context,
				uint8_t *data, uint32_t data_len)
{
	uart_handler_ctx_t *hc = (uart_handler_ctx_t *)*context;
	/* Между вызовами обработчик ничего не держит (накопитель приёма принадлежит
	 * порту, а не вызову), поэтому снятие вызова — это просто FALSE: освобождать
	 * нечего. */
	if (call == NULL)
		return 0;
	if ((hc == NULL) || (hc->app == NULL))
		return 0;

	if (hc->act == UART_ACT_RX)
		return uart_rx_flush(hc);          /* и такт, и опрос — одно и то же */

	if (data == NULL)
		return 0;                          /* такт: остальным делать нечего */

	switch (hc->act) {
	case UART_ACT_CNF:  return uart_cmd_config(hc, call, data, data_len);
	case UART_ACT_TX:   return uart_cmd_tx(hc, call, data, data_len);
	case UART_ACT_ECHO: return uart_cmd_echo(hc, call, data, data_len);
	default:            return 0;
	}
}

/* Регистрация одного FID на одном ядре: контекст — порт и действие, поэтому
 * обработчику не нужно разбирать имя (а имя нужно только для рассылки). */
static uint8_t uart_register(api_t *api, uart_handler_ctx_t *hc, app_context_t *app,
			     uint8_t port, const char *fid, uart_act_t act)
{
	hc->app  = app;
	hc->port = port;
	hc->act  = act;
	hc->fid  = fid;
	return api_register_handler(api, uart_api_handler, fid, hc);
}

/* Готовит состояние портов: накопитель приёма, замок и подписка на события
 * драйвера. Общее для прошивки и хостовых тестов — чтобы тест проверял ровно
 * ту же подготовку, что и цель. */
static void uart_ports_init(app_context_t *app)
{
        uart_apis_set(app->api_ap, app->api_sta);
        uart_frame_lock = platform_mutex_create();

        uart_context[0].port         = 0;
        uart_context[0].storage      = uart1_buf;
        uart_context[0].storage_size = sizeof(uart1_buf);
        gw_uart_rxbuf_init(&uart_context[0].rxbuf, uart_context[0].storage,
                           uart_context[0].storage_size);
        uart_context[0].lock = platform_mutex_create();
        uart1_delegate.handler = &uart_event_on_rx;
        uart1_delegate.context = &uart_context[0];

        uart_context[1].port         = 1;
        uart_context[1].storage      = uart2_buf;
        uart_context[1].storage_size = sizeof(uart2_buf);
        gw_uart_rxbuf_init(&uart_context[1].rxbuf, uart_context[1].storage,
                           uart_context[1].storage_size);
        uart_context[1].lock = platform_mutex_create();
        uart2_delegate.context = &uart_context[1];
        uart2_delegate.handler = &uart_event_on_rx;

        gw_uart_on_receive_subscribe(&app->uart.port[0].desc, &uart1_delegate);
        gw_uart_on_receive_subscribe(&app->uart.port[1].desc, &uart2_delegate);
}

/* Регистрирует обработчики всех FID портов на переданных ядрах. NULL-ядро
 * пропускается: в тестах бывает одно. */
static uint8_t uart_fids_register(app_context_t *app, api_t *api_ap, api_t *api_sta)
{
        static const char *const fids[2][UART_ACT_COUNT] = {
                { ESP_WS_API_UART1_CNF, ESP_WS_API_UART1_RAW_RX,
                  ESP_WS_API_UART1_RAW_TX, ESP_WS_API_UART1_ECHO },
                { ESP_WS_API_UART2_CNF, ESP_WS_API_UART2_RAW_RX,
                  ESP_WS_API_UART2_RAW_TX, ESP_WS_API_UART2_ECHO },
        };
        api_t *cores[2] = { api_ap, api_sta };

        for (uint8_t port = 0; port < 2; port++) {
                for (uint8_t act = 0; act < UART_ACT_COUNT; act++) {
                        for (uint8_t c = 0; c < 2; c++) {
                                if (cores[c] == NULL)
                                        continue;
                                if (!uart_register(cores[c], &uart_handler_ctx[port][act], app,
                                                   port, fids[port][act], (uart_act_t)act))
                                        return 0;
                        }
                }
        }
        return 1;
}

esp_err_t ws_uart_run(app_context_t *app)
{
        /* Порт и его данные доступны с любого интерфейса: политики у UART нет,
         * поэтому обработчики FID регистрируются на ОБОИХ ядрах — вызов
         * приходит в то ядро, через чей транспорт пришёл клиент, а ответ уходит
         * по дескриптору вызова, то есть обратно туда же. */
        uart_ports_init(app);

        /* Своей задачи у сервиса нет: и приём (делегат драйвера), и запросы
         * клиентов (обработчики FID в такте ядра) обслуживаются без очередей
         * ядра и без ожиданий. */
        if (!uart_fids_register(app, app->api_ap, app->api_sta)) {
                platform_log_w(TAG, "UART: не удалось зарегистрировать FID\n");
                return ESP_FAIL;
        }
        return ESP_OK;
}

#ifdef APIH_UART_TEST
/* Хостовая обвязка: то же, что ws_uart_run, но без драйвера портов. Ядро у
 * теста одно — им же играет роль клиента транспорта. */
void apih_uart_test_init(api_t *api, app_context_t *app) {
    uart_ports_init(app);
    uart_fids_register(app, api, app->api_sta);
    uart_rx_frame_sent[0] = 0;
    uart_rx_frame_sent[1] = 0;
    uart_rx_frame_tick[0] = 0;
    uart_rx_frame_tick[1] = 0;
}

void apih_uart_test_reset(void) {
    for (int i = 0; i < 2; i++)
        gw_uart_rxbuf_init(&uart_context[i].rxbuf, uart_context[i].storage,
                           uart_context[i].storage_size);
    uart_rx_frame_sent[0] = 0;
    uart_rx_frame_sent[1] = 0;
}

#ifdef APIH_UART_TEST
void apih_uart_test_fill_rx_idle(int port, const uint8_t *data, uint32_t len,
                                 uint32_t gap_us, uint8_t idle);
#endif

/* «Данные готовы к отдаче» — это законченная передача на линии: незакрытый
 * кусок читателю не отдаётся, пока не перевалит за порог частичной выгрузки. */
void apih_uart_test_fill_rx(int port, const uint8_t *data, uint32_t len) {
    apih_uart_test_fill_rx_idle(port, data, len, 0, 1);
}

/* idle — «линия замолчала после этих байт», то есть передача закончилась.
 * Границы передач хранит сама метка куска. */
void apih_uart_test_fill_rx_idle(int port, const uint8_t *data, uint32_t len,
                                 uint32_t gap_us, uint8_t idle) {
    uart_subscription_context_t *ctx = &uart_context[port];
    (void)gap_us;
    if (len > 0xFFFF) len = 0xFFFF;
    gw_uart_rxbuf_push(&ctx->rxbuf, data, (uint16_t)len, idle ? 1 : 0);
}
#endif
