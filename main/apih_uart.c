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
#include "seg_ring.h"

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
/* Портов теперь три — по аппаратным номерам: uart0 (консоль), uart1, uart2. */
#define UART_PORTS 3
static uart_handler_ctx_t uart_handler_ctx[UART_PORTS][UART_ACT_COUNT];


/* Порт, чей кадр приёма транспорт не принял (его рабочая очередь полна). Кадр
 * остаётся собранным в uart_raw_buf и повторяется на следующем такте: данные из
 * кольца уже забраны, и потерять их молча — худший из вариантов. -1 — нечего
 * повторять. */
static int8_t   uart_rx_pending_port = -1;
static uint16_t uart_rx_pending_len;     /* длина отложенного кадра */
static uint8_t  uart_rx_pending_tries;   /* сколько раз уже пробовали отправить */

/* Сколько тактов удерживаем отложенный кадр. Транспорт, не принявший кадр за
 * столько попыток, не примет его и дальше (клиент ушёл или связь встала), а
 * держать кадр вечно нельзя: пока он в буфере, порт не отдаёт НИЧЕГО — так
 * терминал «просто перестаёт работать» до перезагрузки. */
#define UART_RX_PENDING_TRIES_MAX 20


typedef struct {
    /* Кольцо приёма: сегменты [метка][данные] с границами передач (см. seg_ring.h).
     * Само кольцо (массив байт) — статический буфер этого файла, дескриптор здесь. */
    gw_seg_ring_t ring;
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
 * Размер накопителя выбран от скорости порта: 115200 бод — это 11520 Б/с, то
 * есть 864 байта за 75 мс; 2048 — вдвое с лишним больше, а кадр такого размера
 * (в base64 ~2,7 КБ) уходит одним сообщением. Накопитель — это же и размер
 * кадра, и размер кольца порта: держать больше незачем, а память он занимает
 * сразу и навсегда (см. uart{0,1,2}_buf).
 *
 * Проверка на приборе: непрерывный поток 115200 бод с двух портов одновременно,
 * подписки с двух интерфейсов — см. local/tools/ws_stream_rate.py. */
#define UART_RX_FRAME_MS      75
#define UART_RX_ACC_SIZE      2048
/* Запас, начиная с которого сборка не ждёт темпа: столько в кольце — значит
 * сервис отстаёт, и ждать очередных 75 мс нельзя. Четверть кольца — заведомо
 * больше того, что накапливается за темп при ровном потоке, и заметно меньше
 * кольца, чтобы потерять было чего. */
#define UART_RX_DRAIN_LIMIT   (UART_RX_ACC_SIZE / 4)
#define UART_B64_MAX          (4 * ((UART_RX_ACC_SIZE + 2) / 3))

/* Кадр приёма собирает и отдаёт подписчикам ТАКТ ЯДРА — обработчик подписки в
 * контексте задачи приложения (см. uart_rx_flush). Задача приёма порта только
 * складывает байты в кольцо: если бы она ещё и собирала кадр, ей пришлось бы
 * ждать отправки в транспорт, а приём в это время встал бы — так терялись
 * данные, которых никто не считал потерянными (переполнение кольца драйвера).
 *
 * Кроме того, сборка из двух задач — это гонка с тактом ядра: рассылка ищет
 * подписчиков по спискам вызовов, а вызов в этот момент такт как раз
 * обрабатывает, и кадр уходил «в никуда» с записью «подписчиков нет». Один
 * отправитель (такт) такой гонки не имеет вовсе. */

static void uart_event_on_rx(void *event_trigger, void *sender, void *context) {
    (void)sender;
    uart_subscription_context_t *buf_desc = (uart_subscription_context_t *)context;
    gw_uart_event_data_t *data = (gw_uart_event_data_t *)event_trigger;
    if (!buf_desc || !data || !data->buf || !data->size)
        return;

    /* Кусок кладём в кольцо вместе с признаком «линия замолчала» — по нему
     * читатель видит конец передачи. Признак и границы передач пишутся здесь,
     * в момент приёма, а не при отдаче: отдача может агрегировать сколько угодно
     * сегментов в одно сообщение и ничего при этом не склеивает. */
    size_t sz = data->size;
    if (sz > 0xFFFF) sz = 0xFFFF;
    uint16_t taken;

    platform_mutex_take(buf_desc->lock, PLATFORM_WAIT_FOREVER);
    taken = gw_seg_ring_write(&buf_desc->ring, data->buf, (uint16_t)sz, data->b_end);
    platform_mutex_give(buf_desc->lock);

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

static uint8_t uart0_buf[UART_RX_ACC_SIZE];
static uint8_t uart1_buf[UART_RX_ACC_SIZE];
static uint8_t uart2_buf[UART_RX_ACC_SIZE];
/* Когда с каждого порта ушёл предыдущий кадр — по нему считается темп. */
static uint32_t uart_rx_frame_tick[UART_PORTS];
static uint8_t  uart_rx_frame_sent[UART_PORTS];
static uart_subscription_context_t uart_context[UART_PORTS];
static delegate_t uart_delegate[UART_PORTS];

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

/* Кадр приёма: забрать из кольца столько, сколько влезает в один кадр, и
 * разослать подписчикам этого FID на всех ядрах.
 *
 * Зовётся ТОЛЬКО из такта ядра (задача приложения), поэтому буферы base64 и
 * снимок куска ни с кем не делятся: задача приёма порта в них не заходит. */
static uint8_t uart_rx_flush(uart_handler_ctx_t *hc)
{
	uart_subscription_context_t *ctx = &uart_context[hc->port];
	const int port = hc->port;
	gw_seg_ring_chunk_t probe;

	/* Прошлый кадр транспорт не принял — повторяем его, а не собираем новый:
	 * данные из кольца уже забраны. Темп здесь не при чём, ждать нечего. */
	if (uart_rx_pending_port >= 0) {
		if (uart_rx_pending_port != port)
			return 1;               /* в буфере кадр другого порта */
		if (uart_broadcast_built(hc->fid, uart_raw_buf, uart_rx_pending_len)) {
			uart_rx_pending_port  = -1;
			uart_rx_pending_tries = 0;
		} else if (++uart_rx_pending_tries >= UART_RX_PENDING_TRIES_MAX) {
			/* Транспорт так и не принял. Кадр отпускаем и говорим об этом
			 * громко: иначе порт замолкает навсегда, а причина не видна. */
			platform_log_w(TAG, "UART rx: кадр (%u Б) отброшен, транспорт не принял\n",
				       (unsigned)uart_rx_pending_len);
			uart_rx_pending_port  = -1;
			uart_rx_pending_tries = 0;
		}
		return 1;
	}

	/* Есть ли что отдавать — спрашиваем, не дожидаясь: пустой опрос
	 * (подписка без данных) должен отвечать сразу, а не держать темп. */
	platform_mutex_take(ctx->lock, PLATFORM_WAIT_FOREVER);
	uint8_t ready = gw_seg_ring_read(&ctx->ring, NULL, 0, &probe);
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

	/* Кадр собирается из того, что отдаёт кольцо. Данные копятся в снимке и
	 * кодируются в base64 ЦЕЛИКОМ, а не кусками: у куска, длина которого не
	 * кратна трём, в конце появляется выравнивание '=', а строка со '=' в
	 * середине неразбираема — клиент декодировал такую часть до первого '=' и
	 * терял остальное (на приборе это выглядело как «прибор принял 12 КБ, а
	 * клиент получил 11»). Поэтому выравнивание допускается только на границе
	 * части — там, где стоит разделитель '#'/'~' или кончается кадр. */
	for (;;) {
		size_t  out_len = 0;
		uint8_t lost = 0;
		uint8_t frame_full = 0;
		size_t  staged = 0;              /* накоплено в снимке, ещё не закодировано */

		/* Разделители в данных — символы, которых не бывает в base64:
		 * '#' — сегмент кончился (передача на линии замолчала), '~' — сразу
		 * после этих байт данные терялись. Ставятся в поток, между частями
		 * base64, и относятся к тому, что стоит ПЕРЕД ними. */
		for (;;) {
			int32_t  space = (int32_t)sizeof(uart_b64_buf) - 1 - (int32_t)out_len;
			uint32_t budget;             /* данных влезает в кадр, кратно 3 */
			uint32_t want;
			gw_seg_ring_chunk_t chunk;
			uint32_t n, b64_room;
			int32_t  b64_len;

			if (space <= 8) {
				frame_full = 1;
				break;               /* остальное — следующим кадром */
			}
			/* Запас 4 байта: два разделителя и выравнивание. Кратность трём
			 * даёт кодирование без '=' внутри части. */
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

			platform_mutex_take(ctx->lock, PLATFORM_WAIT_FOREVER);
			uint8_t got = gw_seg_ring_read(&ctx->ring, &uart_rx_snapshot[staged],
						       (uint16_t)want, &chunk);
			platform_mutex_give(ctx->lock);
			if (!got)
				break;               /* отдавать больше нечего */

			staged += chunk.len;

			/* Кодируем накопленное и только когда часть ОБЯЗАНА кончиться:
			 * сегмент закончился или данные потеряны — после них должен встать
			 * '#' / '~'. Пока границы нет, копим дальше. Кодировать по
			 * пришедшему куску нельзя: кусок — это размер события UART
			 * (до 128 байт), а не единица протокола. */
			if (!chunk.end && !chunk.lost && staged < budget)
				continue;

			b64_room = (uint32_t)(space - 4);
			if (staged) {
				n = (uint32_t)staged;
				b64_len = base64_encode((uint8_t *)&uart_b64_buf[out_len], b64_room,
							uart_rx_snapshot, n);
				if (b64_len > 0)
					out_len += (size_t)b64_len;
				staged = 0;
			}
			/* Кусок без данных несёт только разметку: конец сегмента и/или
			 * отметку потери — так они приходят, даже если сами байты уже
			 * ушли раньше. */
			if (chunk.lost) {
				uart_b64_buf[out_len++] = '~';
				lost = 1;
			}
			if (chunk.end)
				uart_b64_buf[out_len++] = '#';
		}

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

		if (out_len == 0)
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
		uint8_t room = gw_jsonw_ok(&w);
		size_t  flen = gw_jsonw_finish(&w);
		if (!room || !flen)
			break;                   /* кадр не собрался: такой же дефект, как потеря */
		uart_rx_pending_len = (uint16_t)flen;
		if (!uart_broadcast_built(hc->fid, uart_raw_buf, flen)) {
			/* Транспорт не принял (очередь полна). Кадр остаётся в буфере:
			 * повторим на следующем такте. */
			uart_rx_pending_port  = (int8_t)port;
			uart_rx_pending_tries = 0;
			platform_log_w(TAG, "UART rx: транспорт занят, кадр (%u Б) отложен\n",
				       (unsigned)flen);
		}

		if (lost)
			platform_log_w(TAG, "UART rx: приём терялся, клиенту сообщено\n");

		/* Ещё один кадр сразу — в двух случаях: кадр УПЁРСЯ в предел (это
		 * выгрузка того же накопления) или в кольце скопился запас больше
		 * UART_RX_DRAIN_LIMIT (сервис отстаёт от линии, и ждать темпа нельзя —
		 * за это время накопится ещё столько же). Во всех прочих случаях кадр
		 * последний, а следующий пойдёт по темпу: без этой оговорки сборка
		 * превращалась в отправку на каждый пришедший кусок — кадры по 120
		 * байт, отправка каждого ~10 мс, и на 115200 бод сервис переставал
		 * успевать за линией: кольцо росло и переполнялось. */
		platform_mutex_take(ctx->lock, PLATFORM_WAIT_FOREVER);
		uint16_t pending = gw_seg_ring_used(&ctx->ring);
		platform_mutex_give(ctx->lock);
		if (!frame_full && pending < UART_RX_DRAIN_LIMIT)
			break;
	}

	return 0;
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
	if (call == NULL) {
		/* Вызов снят (клиент ушёл): отложенный кадр никому уже не нужен, а
		 * держать его — значит заблокировать порт для следующей подписки. */
		uart_rx_pending_port  = -1;
		uart_rx_pending_tries = 0;
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
        static uint8_t *const ring_storage[UART_PORTS] = { uart0_buf, uart1_buf, uart2_buf };

        uart_apis_set(app->api_ap, app->api_sta);

        for (uint8_t i = 0; i < UART_PORTS; i++) {
                /* Номер порта в службе — аппаратный (uart0/uart1/uart2), поэтому
                 * и кольцо, и драйвер адресуются одним и тем же номером. */
                uart_context[i].port         = i;
                uart_context[i].storage      = ring_storage[i];
                uart_context[i].storage_size = UART_RX_ACC_SIZE;
                gw_seg_ring_init(&uart_context[i].ring, uart_context[i].storage,
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
    for (int i = 0; i < UART_PORTS; i++)
        gw_seg_ring_init(&uart_context[i].ring, uart_context[i].storage,
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
    gw_seg_ring_write(&ctx->ring, data, (uint16_t)len, idle ? 1 : 0);
}
#endif
