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
#include "CodeLib.h"          /* кольцо приёма — CodeLibrary/CL/SegmentedRing */

#define GW_LOG_MODULE GW_LOG_UART
#include "gw_log.h"

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

/* Ядра API, чьи подписчики получают кадры порта. Сервис зарегистрирован на ОБОИХ
 * ядрах, поэтому кадр приёма и смена настроек рассылаются по имени FID на каждом
 * (api_broadcast), а ответ на запрос уходит по дескриптору вызова. */
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
 * Действие известно по РЕГИСТРАЦИИ, а не по разбору имени: обработчик получает
 * контекст регистрации, а в нём порт, действие и имя FID (имя нужно для
 * рассылки — у вызова его не спросить). */
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

/* По контексту на каждую пару «порт × действие», порты — аппаратные номера
 * (uart0 — консоль, uart1, uart2). Контексты общие для обоих ядер. */
#define UART_PORTS 3
static uart_handler_ctx_t uart_handler_ctx[UART_PORTS][UART_ACT_COUNT];


/* Порт, чей кадр приёма транспорт не принял (его рабочая очередь полна). Кадр
 * остаётся собранным в uart_raw_buf и повторяется на следующем такте: данные из
 * кольца уже забраны, и потерять их молча — худший из вариантов. -1 — нечего
 * повторять. */
/* Кадр, который транспорт не принял, остаётся собранным в буфере СВОЕГО порта и
 * повторяется на следующем такте: данные из кольца уже забраны. Буфер — по
 * порту, иначе второй порт перезаписывает кадр первого. */
static uint8_t  uart_rx_pending[UART_PORTS];      /* 1 — в буфере лежит кадр */
static uint16_t uart_rx_pending_len[UART_PORTS];  /* его длина */
static uint32_t uart_rx_pending_tick[UART_PORTS]; /* когда отложили */

/* Сколько времени держим отложенный кадр. Держать вечно нельзя: пока кадр в
 * буфере, порт не отдаёт НИЧЕГО. */
#define UART_RX_PENDING_HOLD_MS 500


typedef struct {
    /* Кольцо приёма: сегменты [метка][данные] с границами передач (см. SegmentedRing.h).
     * Само кольцо (массив байт) — статический буфер этого файла, дескриптор здесь. */
    seg_ring_t ring;
    uint8_t *storage;
    uint16_t storage_size;
    platform_mutex_t lock;
    uint8_t port;               /* 0 — "uart1", 1 — "uart2" */
    /* Ядро — не здесь, а в uart_apis: ядер у сервиса два. */
} uart_subscription_context_t;

/* ── Приём: кольцо и кадр ────────────────────────────────────────────────────
 *
 * Кольцо (uart{0,1,2}_buf) — сколько копим: 4 КБ при 115200 бод (864 Б за 75 мс
 * темпа) это ~355 мс непрерывного потока, запас на пики и на отставший сервис.
 * Кадр (UART_RX_FRAME_DATA_MAX) — сколько отдаём клиенту за раз: 2048 байт дают
 * ~2,7 КБ base64, то есть одно сообщение транспорта без фрагментов. Размеры НЕ
 * связаны: кольцо переживает пики линии, кадр задаёт порцию отдачи.
 *
 * Темп: кадр не чаще раза в UART_RX_FRAME_MS, если байты идут чаще — сервис
 * копит их и отдаёт всё накопленное к сроку. Не гнать по кадру на каждый байт:
 * эхо набора в терминале захлёбывало рабочую очередь httpd. */
#define UART_RX_FRAME_MS      75
#define UART_RX_ACC_SIZE      4096
/* Данных приёма в одном кадре API (base64 выйдет ~4/3 от этого). */
#define UART_RX_FRAME_DATA_MAX 2048
/* Место под base64 одного кадра плюс разделители '#' и '~' на границах передач. */
#define UART_B64_MAX          (4 * ((UART_RX_FRAME_DATA_MAX + 2) / 3) + 256)

/* Кадр собирает и отдаёт подписчикам ТАКТ ЯДРА (см. uart_rx_flush): задача
 * приёма только складывает байты в кольцо. Иначе ей пришлось бы ждать отправки
 * в транспорт, приём встал бы и терял данные; плюс сборка из двух задач — это
 * гонка с тактом, который в тот же момент обрабатывает вызовы. Один отправитель
 * (такт) такой гонки не имеет. */

static void uart_event_on_rx(void *event_trigger, void *sender, void *context) {
    (void)sender;
    uart_subscription_context_t *buf_desc = (uart_subscription_context_t *)context;
    gw_uart_event_data_t *data = (gw_uart_event_data_t *)event_trigger;
    if (!buf_desc || !data || !data->buf || !data->size)
        return;

    /* Кусок в кольцо вместе с признаком «линия замолчала»: границы передач
     * пишутся в момент приёма, отдача их только читает. */
    size_t sz = data->size;
    if (sz > 0xFFFF) sz = 0xFFFF;
    uint16_t taken;

    platform_mutex_take(buf_desc->lock, PLATFORM_WAIT_FOREVER);
    taken = seg_ring_write(&buf_desc->ring, data->buf, (uint16_t)sz, data->b_end);
    if (taken < sz) {
        /* Переполнение: закрываем сегмент пустой меткой СРАЗУ. Иначе признак
         * потери защёлкивается в открытом сегменте, а снимается только его
         * закрытием — то есть простоем линии, которого у непрерывного потока
         * нет: порт замолкает навсегда. */
        seg_ring_write(&buf_desc->ring, NULL, 0, 1);
    }
    platform_mutex_give(buf_desc->lock);

    /* О потере пишем не чаще раза в секунду: иначе печать усиливает потери. */
    if(taken < sz) {
        static uint32_t lost_ms[UART_PORTS];
        static uint32_t lost_bytes[UART_PORTS];
        lost_bytes[buf_desc->port] += (uint32_t)(sz - taken);
        uint32_t now = platform_ticks_to_ms(platform_tick());
        if ((now - lost_ms[buf_desc->port]) >= 1000) {
            LOGI(TAG, "UART rx: кольцо полно, потеряно %lu байт за %lu мс\n",
                 (unsigned long)lost_bytes[buf_desc->port],
                 (unsigned long)(now - lost_ms[buf_desc->port]));
            lost_bytes[buf_desc->port] = 0;
            lost_ms[buf_desc->port] = now;
        }
    }
}


/* Буферы ответов статические: стек задачи ограничен, а принятого бывает
 * несколько килобайт. Сборка идёт прямо в буфер, дерева и узлов нет. */
#define UART_RESP_MAX 96
static char uart_resp_buf[UART_RESP_MAX];

/* Буфер ответа приёма: данные кадра кодируются в base64 одной строкой, на
 * границах передач встают разделители, сверху обёртка {"RX":"…"}. СТАТИЧЕСКИЙ,
 * а не на стеке: такой буфер (4 КБ) на стеке задачи ронял её. */
#define UART_RAW_OVERHEAD 64
static char uart_raw_buf[UART_PORTS][UART_B64_MAX + UART_RAW_OVERHEAD];
static char uart_b64_buf[UART_B64_MAX + 4];
/* Кусок, забранный из кольца под замком порта; кодируется уже без него — приём
 * (задача событий UART) ждать base64 не должен. */
static uint8_t uart_rx_snapshot[UART_RX_FRAME_DATA_MAX];

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

static uint8_t uart0_buf[UART_RX_ACC_SIZE];
static uint8_t uart1_buf[UART_RX_ACC_SIZE];
static uint8_t uart2_buf[UART_RX_ACC_SIZE];
/* Когда с каждого порта ушёл предыдущий кадр — по нему считается темп. */
static uint32_t uart_rx_frame_tick[UART_PORTS];
static uint8_t  uart_rx_frame_sent[UART_PORTS];
static uart_subscription_context_t uart_context[UART_PORTS];
static delegate_t uart_delegate[UART_PORTS];

/* Настройки порта: меняем только то, что просили, остальное (в том числе режим)
 * оставляем как есть — пустой ARG это чтение, драйвер на него не дёргаем. */
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
 * оборачивает данные в конверт {"FID":<то же имя>,"SID":<SID клиента>} и шлёт в
 * соединение этого вызова.
 *
 * РАССЫЛКА (кадр приёма, смена настроек) адресована не вызову, а всем
 * подписчикам имени, и у каждого ядра свои: api_broadcast находит подписчиков
 * ЭТОГО ядра и отправляет им ОДИН общий буфер — копии не собираются.
 *
 * Оба пути неблокирующие: обработчик в такте ядра ждать не имеет права. */

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

/* Рассылка уже собранного кадра по всем ядрам сервиса. 1 — кадр принял хотя бы
 * один транспорт (значит, повторять нечего). */
static uint8_t uart_broadcast_built(const char *fid, const char *json, size_t len)
{
	uint8_t sent = 0;

	if (uart_api_num == 0)
		return 1;                       /* ядер нет: терять некуда */
	for (uint8_t i = 0; i < uart_api_num; i++)
		sent |= api_broadcast(uart_apis[i], fid, (const uint8_t *)json, len);
	return sent;
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
		/* Принимаем и число бит (7/8), и значение перечисления (0/1): в
		 * двухбитное поле 8 не влезет и превратится в 0 (7 бит). */
		if (val == 7 || val == 0)
			cmd->wl = GW_UART_WORD_7BIT;
		else if (val == 8 || val == 1)
			cmd->wl = GW_UART_WORD_8BIT;
		else
			cmd->wl_set = 0;
	}
	cmd->boud_set = gw_json_int(&json, "BR", &val);
	if (cmd->boud_set) {
		/* Скорость вне диапазона драйвер не примет — отвергаем сразу. */
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
	/* IDLE_CH — порог «линия молчит», в знаках: сколько тишины считать концом кадра. */
	cmd->idle_set = gw_json_int(&json, "IDLE_CH", &val);
	if (cmd->idle_set) {
		if (val >= 1 && val <= 255)
			cmd->idle = (uint8_t)val;
		else
			cmd->idle_set = 0;
	}
	cmd->mode_set = gw_json_int(&json, "MODE", &val);
	if (cmd->mode_set) {
		/* Режимы: 0 — обычный UART, 1 — RS-485 с автонаправлением, 2 —
		 * однопроводный (эхо глушим сами). Прочее не принимаем. */
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
 * возвращает TRUE — и ядро позовёт его на следующем такте. Ждать внутри нельзя:
 * такт ядра один, и «подождать» значит вернуть управление. Поэтому все действия
 * быстрые: config — настройки порта, tx — base64 в порт, echo — эхо, rx —
 * разослать накопленное кадром (темп не вышел — вернуть TRUE).
 *
 * data == NULL — это такт работы, а не запрос клиента: смысл он имеет только
 * для приёма. */

/* Кадр приёма: забрать из кольца сколько влезает в один кадр и разослать
 * подписчикам этого FID на всех ядрах. Зовётся ТОЛЬКО из такта ядра, поэтому
 * буферы base64 и снимок куска ни с кем не делятся. */
static uint8_t uart_rx_flush(uart_handler_ctx_t *hc)
{
	uart_subscription_context_t *ctx = &uart_context[hc->port];
	const int port = hc->port;

	/* Этот порт не дослал свой кадр — повторяем: данные из кольца уже забраны. */
	if (uart_rx_pending[port]) {
		if (uart_broadcast_built(hc->fid, uart_raw_buf[port], uart_rx_pending_len[port])) {
			uart_rx_pending[port] = 0;
		} else if ((uint32_t)(platform_tick() - uart_rx_pending_tick[port]) >=
			   platform_ms_to_ticks(UART_RX_PENDING_HOLD_MS)) {
			/* Транспорт молчит дольше отведённого — кадр отпускаем. */
			LOGI(TAG, "UART rx: кадр (%u Б) отброшен, транспорт не принял\n",
			     (unsigned)uart_rx_pending_len[port]);
			uart_rx_pending[port] = 0;
		}
		return 1;
	}

	/* Темп: не чаще раза в UART_RX_FRAME_MS. Спать в обработчике нельзя —
	 * возвращаем 1 и приходим следующим тактом. */
	if (uart_rx_frame_sent[port]) {
		uint32_t gap  = platform_ms_to_ticks(UART_RX_FRAME_MS);
		int32_t  left = (int32_t)(uart_rx_frame_tick[port] + gap - platform_tick());
		if (left > 0) {
			/* Не чаще раза в 75 мс — но не ровно раз в 75 мс: если в кольце
			 * уже целый кадр, отдаём сразу, иначе кольцо переполняется. */
			platform_mutex_take(ctx->lock, PLATFORM_WAIT_FOREVER);
			uint16_t used = (uint16_t)(UART_RX_ACC_SIZE - seg_ring_free(&ctx->ring));
			platform_mutex_give(ctx->lock);
			if (used < UART_RX_FRAME_DATA_MAX) {
				return 1;
			}
		}
	}

	/* Накопленное кодируется ЦЕЛИКОМ до границы передачи: у куска некратной
	 * длины появляется '=', а '=' в середине строки клиент не разбирает. */
	for (;;) {
		size_t  out_len = 0;
		uint8_t lost = 0;
		uint8_t frame_full = 0;
		size_t  staged = 0;              /* накоплено в снимке, ещё не закодировано */

		/* Кольцо читаем под ОДНИМ замком на весь кадр. */
		platform_mutex_take(ctx->lock, PLATFORM_WAIT_FOREVER);
		for (;;) {
			int32_t  space = (int32_t)sizeof(uart_b64_buf) - 1 - (int32_t)out_len;
			uint32_t budget;             /* данных влезает в кадр, кратно 3 */
			uint32_t want;
			seg_ring_chunk_t chunk;
			uint32_t b64_room;
			int32_t  b64_len;

			if (space <= 8) {
				frame_full = 1;
				break;               /* остальное — следующим кадром */
			}
			/* Запас 4 байта: разделитель и выравнивание; кратность трём даёт
			 * кодирование без '=' внутри части. */
			budget = (uint32_t)(((space - 4) / 4) * 3);
			if (budget > sizeof(uart_rx_snapshot))
				budget = (uint32_t)sizeof(uart_rx_snapshot);
			if (staged > budget) {
				frame_full = 1;
				break;
			}
			want = budget - (uint32_t)staged;
			if (want > sizeof(uart_rx_snapshot) - staged)
				want = (uint32_t)(sizeof(uart_rx_snapshot) - staged);
			if (want == 0) {
				frame_full = 1;
				break;
			}

			seg_ring_read(&ctx->ring, &uart_rx_snapshot[staged],
				      (uint16_t)want, &chunk);
			/* Пустой кусок = читать нечего (read возвращает результат вызова). */
			if (!chunk.len && !chunk.end && !chunk.lost)
				break;

			staged += chunk.len;

			/* Границы передачи нет — копим дальше. */
			if (!chunk.end && !chunk.lost && staged < budget)
				continue;

			/* Граница: кодируем накопленное ОДНОЙ строкой и ставим разделитель
			 * ('~' — потеря, '#' — нормальный конец передачи). */
			b64_room = (uint32_t)(space - 4);
			if (staged) {
				b64_len = base64_encode((uint8_t *)&uart_b64_buf[out_len], b64_room,
							uart_rx_snapshot, (uint32_t)staged);
				if (b64_len > 0)
					out_len += (size_t)b64_len;
				staged = 0;
			}
			if (chunk.lost) {
				uart_b64_buf[out_len++] = '~';
				lost = 1;
			} else if (chunk.end) {
				uart_b64_buf[out_len++] = '#';
			}
		}
		platform_mutex_give(ctx->lock);

		/* Кадр кончается, а накопленное не закодировано (границы части не
		 * было) — кодируем сейчас, с выравниванием: этим часть и закрывается. */
		if (staged) {
			int32_t space = (int32_t)sizeof(uart_b64_buf) - 1 - (int32_t)out_len;
			int32_t b64_len = base64_encode((uint8_t *)&uart_b64_buf[out_len],
							(uint32_t)(space - 2),
							uart_rx_snapshot, (uint32_t)staged);
			if (b64_len > 0)
				out_len += (size_t)b64_len;
		}

		if (out_len == 0) {
			break;                           /* отдавать больше нечего */
		}

		uart_b64_buf[out_len] = '\0';

		/* Данные одним куском текста: '#' — конец передачи на линии, '~' —
	 * потеря (разделитель один), кусок без разделителя — продолжение
	 * предыдущего. Адресат — подписчики этого FID на каждом ядре: кадр не
	 * принадлежит конкретному вызову. */
		gw_jsonw_t w;
		gw_jsonw_init(&w, uart_raw_buf[port], sizeof(uart_raw_buf[port]));
		gw_jsonw_obj(&w, NULL);
		gw_jsonw_str(&w, "RX", uart_b64_buf);
		uint8_t room = gw_jsonw_ok(&w);
		size_t  flen = gw_jsonw_finish(&w);
		if (!room || !flen)
			break;                   /* кадр не собрался: такой же дефект, как потеря */
		uart_rx_pending_len[port] = (uint16_t)flen;
		if (!uart_broadcast_built(hc->fid, uart_raw_buf[port], flen)) {
			/* Транспорт занят: кадр остаётся в буфере порта, повторим тактом. */
			uart_rx_pending[port]      = 1;
			uart_rx_pending_tick[port] = platform_tick();
			LOGI(TAG, "UART rx: транспорт занят, кадр (%u Б) отложен\n", (unsigned)flen);
		} else {
			/* Темп закрываем ТОЛЬКО по факту приёма транспортом. */
			uart_rx_frame_tick[port] = platform_tick();
			uart_rx_frame_sent[port] = 1;
		}

		if (lost)
			LOGI(TAG, "UART rx: приём терялся, клиенту сообщено\n");

		/* Уперлись в предел кадра — в кольце ещё есть данные, ждать темпа нельзя. */
		if (!frame_full)
			break;
	}

	return 0;
}

/* На чтение надо ответить: молчание клиент ждёт до своего таймаута. */
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
		/* ARG не разобран — применять нельзя, отвечаем текущим состоянием. */
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

	/* Рассылка подписчикам — ТОЛЬКО если настройки действительно изменились:
	 * смена режима или скорости обесценивает работу остальных, узнать об этом
	 * надо сразу. Запись тех же значений будить всех незачем: терминал не должен
	 * отключаться из-за чужого «применить то же самое». */
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

	/* ARG — срез исходного JSON, jsmn отдаёт строку БЕЗ кавычек, вариант с
	 * кавычками распознаём по крайним символам. Безусловно срезать нельзя:
	 * base64 испортится и в порт не уйдёт ничего. */
	const uint8_t *b64 = data;
	size_t   b64_len = len;
	if (b64_len >= 2 && b64[0] == '"' && b64[b64_len - 1] == '"') {
		b64++;
		b64_len -= 2;
	}
	int32_t buf_size = base64_decode_buffer_required(b64, b64_len);

	if (buf_size > 0) {
		/* Декодируем в КУЧУ: нагрузка доходит до предела сообщения транспорта
		 * (16 КБ), а буфер такого размера на стеке задачи (4 КБ) её ронял. */
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
	/* Между вызовами обработчик ничего не держит: накопитель приёма принадлежит
	 * порту, а не вызову. */
	if (call == NULL) {
		/* Вызов снят: отложенный кадр никому не нужен, а держать его — значит
		 * заблокировать порт для следующей подписки. */
		if (hc != NULL)
			uart_rx_pending[hc->port] = 0;
		return 0;
	}
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

/* Регистрация FID: контекст — порт и действие, имя нужно только для рассылки. */
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
        static uint8_t *const ring_storage[UART_PORTS] = { uart0_buf, uart1_buf, uart2_buf };

        uart_apis_set(app->api_ap, app->api_sta);

        for (uint8_t i = 0; i < UART_PORTS; i++) {
                /* Номер порта в службе — аппаратный (uart0/uart1/uart2), поэтому
                 * и кольцо, и драйвер адресуются одним и тем же номером. */
                uart_context[i].port         = i;
                uart_context[i].storage      = ring_storage[i];
                uart_context[i].storage_size = UART_RX_ACC_SIZE;
                seg_ring_init(&uart_context[i].ring, uart_context[i].storage,
                                 uart_context[i].storage_size);
                uart_context[i].lock = platform_mutex_create();

                uart_delegate[i].handler = &uart_event_on_rx;
                uart_delegate[i].context = &uart_context[i];
                gw_uart_on_receive_subscribe(&app->uart.port[i].desc, &uart_delegate[i]);
        }
}

/* Регистрирует обработчики всех FID портов на переданных ядрах. NULL-ядро
 * пропускается: в тестах бывает одно. */
static uint8_t uart_fids_register(app_context_t *app, api_t *api_ap, api_t *api_sta)
{
        static const char *const fids[UART_PORTS][UART_ACT_COUNT] = {
                { ESP_WS_API_UART0_CNF, ESP_WS_API_UART0_RAW_RX,
                  ESP_WS_API_UART0_RAW_TX, ESP_WS_API_UART0_ECHO },
                { ESP_WS_API_UART1_CNF, ESP_WS_API_UART1_RAW_RX,
                  ESP_WS_API_UART1_RAW_TX, ESP_WS_API_UART1_ECHO },
                { ESP_WS_API_UART2_CNF, ESP_WS_API_UART2_RAW_RX,
                  ESP_WS_API_UART2_RAW_TX, ESP_WS_API_UART2_ECHO },
        };
        api_t *cores[2] = { api_ap, api_sta };

        for (uint8_t port = 0; port < UART_PORTS; port++) {
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
        /* Политики у UART нет: обработчики FID регистрируются на ОБОИХ ядрах —
         * вызов приходит в то ядро, через чей транспорт пришёл клиент, а ответ
         * уходит по дескриптору вызова, то есть обратно туда же. */
        uart_ports_init(app);

        /* Своей задачи у сервиса нет: и приём (делегат драйвера), и запросы
         * клиентов (обработчики FID в такте ядра) обслуживаются без ожиданий. */
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
    for (int i = 0; i < UART_PORTS; i++)
        seg_ring_init(&uart_context[i].ring, uart_context[i].storage,
                         uart_context[i].storage_size);
    for (int i = 0; i < UART_PORTS; i++)
        uart_rx_frame_sent[i] = 0;
}

#ifdef APIH_UART_TEST
void apih_uart_test_fill_rx_idle(int port, const uint8_t *data, uint32_t len,
                                 uint32_t gap_us, uint8_t idle);
#endif

/* «Данные готовы к отдаче» — это то, что уже лежит в кольце; закрывать сегмент
 * для этого не нужно (незакрытый отдаётся с end == 0). */
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
    seg_ring_write(&ctx->ring, data, (uint16_t)len, idle ? 1 : 0);
}
#endif
