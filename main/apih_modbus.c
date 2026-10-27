
#include "app.h"
#include "uart.h"
#include "GwJson.h"
#include "api.h"
#include "ModBusHelpers.h"

#define MB_PAYLOAD_SIZE    255
#define MB_DEFAULT_AWT_MS  500

/* токенов на разбор ARG: ADR/FN/RA/RVC/AWT и список RD; длиннее — отказ разбора */
#define MB_ARG_MAX_TOKENS  48

static uint32_t mb_timer_fn(const void *ctx) {
    (void)ctx;
    return xTaskGetTickCount() * portTICK_PERIOD_MS;
}

/* приём: байты линии библиотеке отдаёт mb_rx_handler, в интерфейсе — передача и
 * таймер */
static const modbus_iface_t mb_iface = {
    .pfWrite = gw_uart_write,
    .pfTimer = mb_timer_fn,
};

typedef struct mb_call_ctx mb_call_ctx_t;

/* порт и его очередь запросов: голова очереди отдаётся драйверу, когда он
 * освободится, поэтому важен порядок прихода, а не «кто просит» */
typedef struct {
    modbus_t           mb;
    uint8_t            payload[MB_PAYLOAD_SIZE];
    struct app_uart_t *app_uart;
    int                port_no;
    delegate_t         rx_delegate;
    mb_call_ctx_t     *head;
    mb_call_ctx_t     *tail;
    uint8_t            queue_len;
    mb_call_ctx_t     *active;       /* чья транзакция сейчас на линии */
} modbus_worker_t;

/* сколько запросов порт держит в очереди: каждый — контекст в куче, копить их
 * бесконечно нельзя. Кто не влез — получает отказ «занято» и может повторить. */
#define MB_QUEUE_MAX 8

/* отказы каркаса в ответе клиенту; ERR в кадре прибора — другое: код исключения
 * прибора */
#define MB_ERR_BUSY    5
#define MB_ERR_TIMEOUT 4
#define MB_ERR_START   3
#define MB_ERR_ARG     2
#define MB_ERR_MEM     1

/* Состояние одного вызова API: разобранный запрос и место в очереди порта.
 * Живёт в контексте вызова, который принадлежит обработчику. Ответ клиенту
 * формирует и отправляет драйвер из своего колбэка: api_reply берёт ядро из
 * дескриптора вызова, поэтому ядро обработчику не нужно. */
#define MB_CTX_MAGIC 0x4d424354u   /* 'MBCT' — признак своего контекста */

struct mb_call_ctx {
    uint32_t         magic;
    void            *call;                      /* NULL — вызов уже снят ядром */
    modbus_worker_t *worker;
    modbus_frame_t   req;
    uint8_t          wr_buf[MB_PAYLOAD_SIZE];
    uint16_t         awt_ms;
    TickType_t       deadline;
    mb_call_ctx_t   *next;
    uint8_t          queued;
    volatile uint8_t done;                      /* драйвер ответил, ответ отправлен */
};

static modbus_worker_t mb_workers[2];

static void send_mb_frame(void *call, modbus_frame_t *f);


/* Колбэк драйвера: транзакция закончилась (ответ, ошибка прибора или таймаут).
 * Здесь уходит ответ клиенту и ставится отметка done, по которой обработчик
 * сообщит ядру API, что вызов обработан: ядро либо закроет его, либо повторит. */
static void mb_on_complete(modbus_frame_t *frame, modbus_t *mb, void *ctx) {
    (void)mb;
    mb_call_ctx_t *c = (mb_call_ctx_t *)ctx;
    if (!c || c->magic != MB_CTX_MAGIC)
        return;

    if (c->worker && c->worker->active == c)
        c->worker->active = NULL;

    if (c->call) {
        send_mb_frame(c->call, frame);
        c->done = 1;
    } else {
        /* Вызов сняли, пока транзакция шла: ответ отправлять некому, а
         * обработчик этого контекста уже не получит — освобождаем здесь. */
        free(c);
    }
}

static void mb_queue_push(modbus_worker_t *w, mb_call_ctx_t *c) {
    c->next   = NULL;
    c->queued = 1;
    if (w->tail) w->tail->next = c;
    else         w->head = c;
    w->tail = c;
    w->queue_len++;
}

static void mb_queue_pop(modbus_worker_t *w) {
    mb_call_ctx_t *c = w->head;
    if (!c) return;
    w->head = c->next;
    if (w->tail == c) w->tail = NULL;
    c->next   = NULL;
    c->queued = 0;
    if (w->queue_len) w->queue_len--;
}

static void mb_queue_remove(modbus_worker_t *w, mb_call_ctx_t *c) {
    mb_call_ctx_t **pp = &w->head;
    while (*pp) {
        if (*pp == c) {
            *pp = c->next;
            if (w->tail == c) {                 /* хвост пересчитываем */
                w->tail = w->head;
                while (w->tail && w->tail->next) w->tail = w->tail->next;
            }
            c->next   = NULL;
            c->queued = 0;
            if (w->queue_len) w->queue_len--;
            return;
        }
        pp = &(*pp)->next;
    }
}

/* отдать голову очереди драйверу; 0 — драйвер занят, вызов подождёт такта */
static uint8_t mb_worker_start(modbus_worker_t *w) {
    mb_call_ctx_t *c = w->head;
    if (!c || modbus_busy(&w->mb))
        return 0;

    modbus_reset(&w->mb);
    uint16_t tid = modbus_request(&w->mb, &c->req, mb_on_complete, c, c->awt_ms);
    if (!tid)
        return 0;

    mb_queue_pop(w);
    w->active = c;
    /* Таймаут транзакции считается от её начала: пока вызов ждал линию, его
     * время не должно сгорать (иначе последний в очереди не успевает). */
    c->deadline = xTaskGetTickCount() + pdMS_TO_TICKS(c->awt_ms + 500);
    /* Кадр уходит в линию сразу, не дожидаясь следующего такта. */
    modbus_work(&w->mb);
    return 1;
}

static void mb_rx_handler(void *trigger, void *sender, void *context) {
    (void)sender;
    modbus_worker_t *w = (modbus_worker_t *)context;
    gw_uart_event_data_t *ev = (gw_uart_event_data_t *)trigger;
    if (w && ev && ev->buf && ev->size) {
        modbus_receive_data(&w->mb, ev->buf, (uint16_t)ev->size, ev->b_end);
    }
}

static void send_mb_msg(void *call, const char *json_str, size_t len) {
    if (json_str && len > 0)
        api_reply(call, (const uint8_t *)json_str, len);
}

static void send_mb_error(void *call, int code) {
    char tmp[24];
    int  len = snprintf(tmp, sizeof(tmp), "{\"ERR\":%d}", code);
    send_mb_msg(call, tmp, len);
}

static void send_mb_frame(void *call, modbus_frame_t *f) {
    modbus_frame_t zero = {0};
    if (!f) f = &zero;                 /* драйвер не дал кадр — отвечаем пустым */
    uint8_t code = 0, amount = 0, size = 0;
    uint8_t *regs = pucModbusExtructFrameData(f, &code, &amount, &size);

    /* запас под заголовок ~80 байт и до 9 байт на регистр в ответе */
    size_t buf_sz = 96 + (amount > 0 ? (size_t)(amount) * 9 : 0);
    char  *buf    = malloc(buf_sz);
    if (!buf) {
        send_mb_error(call, MB_ERR_MEM);
        return;
    }

    int is_err = modbus_is_error_frame(f) ? 1 : 0;
    int off = snprintf(buf, buf_sz,
        "{\"ADR\":%u,\"FN\":%u,\"CV\":%u,\"RA\":%u,\"RC\":%u,\"ERR\":%d,\"RD\":[",
        f->ucAddr,
        (unsigned)(f->ucFunc & ~MODBUS_ERROR_FLAG),
        code,
        f->usRegAddr,
        amount,
        is_err);

    if (!is_err && regs && amount > 0) {
        for (uint8_t i = 0; i < amount; i++) {
            if (i > 0 && off < (int)buf_sz - 1)
                buf[off++] = ',';
            if (size == 2) {
                /* Регистры идут старшим байтом вперёд — как в линии. */
                uint16_t v = ((uint16_t)regs[i * 2] << 8) | (uint16_t)regs[i * 2 + 1];
                off += snprintf(buf + off, buf_sz - off, "%u", v);
            } else {
                off += snprintf(buf + off, buf_sz - off, "%u", (unsigned)regs[i]);
            }
        }
    }

    if (off < (int)buf_sz - 2) {
        buf[off++] = ']';
        buf[off++] = '}';
        buf[off]   = '\0';
    }

    send_mb_msg(call, buf, off);
    free(buf);
}

/* Разбор ARG запроса. Возвращает 1, если запрос готов к выдаче драйверу, и 0,
 * если разобрать не удалось (ответ об ошибке уже отправлен). */
static uint8_t mb_call_parse(mb_call_ctx_t *ctx, uint8_t *data, uint32_t len) {
    if (!data || !len) {
        send_mb_error(ctx->call, MB_ERR_ARG);
        return 0;
    }

    /* разбор ARG — jsmn, без дерева и без памяти на элемент: слишком длинный
     * список значений отвергается разбором, а не кучей */
    GW_JSON_TOKENS(toks, MB_ARG_MAX_TOKENS);
    gw_json_t json;
    if (!gw_json_parse(&json, (const char *)data, len, toks, MB_ARG_MAX_TOKENS)) {
        send_mb_error(ctx->call, MB_ERR_ARG);
        return 0;
    }

    modbus_frame_t frame    = {0};
    uint8_t        wr_buf[MB_PAYLOAD_SIZE] = {0};
    uint32_t       awt_ms   = MB_DEFAULT_AWT_MS;
    uint8_t        parse_ok = 0;

    do {
        int32_t val;
        if (!gw_json_int(&json, "ADR", &val) || val < 0 || val > 255) break;
        frame.ucAddr = (uint8_t)val;

        if (!gw_json_int(&json, "FN", &val) || val < 0 || val > 127) break;
        frame.ucFunc = (uint8_t)val;

        if (gw_json_int(&json, "RA",  &val) && val >= 0) frame.usRegAddr       = (uint16_t)val;
        if (gw_json_int(&json, "RVC", &val) && val >= 0) frame.usRegValueCount = (uint16_t)val;
        if (gw_json_int(&json, "AWT", &val) && val > 0)  awt_ms                = (uint32_t)val;

        int n;
        if (gw_json_arr_len(&json, "RD", &n) && n > 0) {
            int stride = (frame.ucFunc == MB_FUNC_WRITE_COILS) ? 1 : 2;
            if (n * stride > MB_PAYLOAD_SIZE) break;
            frame.ucLengthCode = (uint8_t)(n * stride);
            frame.pucData      = wr_buf;
            for (int i = 0; i < n; i++) {
                int32_t v;
                if (!gw_json_arr_int(&json, "RD", i, &v)) break;
                uint32_t u = (uint32_t)v;
                if (stride == 2) {
                    wr_buf[i * 2]     = (u >> 8) & 0xff;
                    wr_buf[i * 2 + 1] = u & 0xff;
                } else {
                    wr_buf[i] = u & 0xff;
                }
            }
        }

        parse_ok = 1;
    } while (0);

    if (!parse_ok) {
        send_mb_error(ctx->call, MB_ERR_ARG);
        return 0;
    }

    /* Линию здесь НЕ трогаем: сброс и настройка — в момент старта транзакции.
     * Иначе разбор второго запроса сбрасывал бы линию под транзакцией первого. */
    ctx->req       = frame;                 /* pucData уже указывает на wr_buf */
    ctx->awt_ms    = (uint16_t)awt_ms;
    ctx->done      = 0;
    ctx->queued    = 0;
    return 1;
}

/* Обработчик по контракту API: data != NULL — запрос клиента (первичный или
 * переопрос подписки), разбираем ARG и ставим вызов в очередь порта; data == NULL
 * — такт работы, двигаем транзакцию и сообщаем ядру о завершении; call == NULL —
 * вызов снимают, убираем его из очереди и освобождаем. Никаких ожиданий внутри:
 * пока линия занята, обработчик возвращает TRUE. */
static uint8_t mb_api_handler(void *call, void **context, uint8_t *data, uint32_t data_len) {
    if (!call) {
        mb_call_ctx_t *c = (mb_call_ctx_t *)*context;
        /* в *context лежит либо наш контекст вызова, либо контекст регистрации
         * (воркер порта): различить их можно только по метке */
        if (c && c->magic == MB_CTX_MAGIC) {
            if (c->worker) {
                if (c->worker->active == c) {
                    /* Транзакция уже на линии, а вызов снят: гасим её и
                     * освобождаем контекст сразу. Ждать колбэк нельзя — после
                     * сброса драйвер его не позовёт, а линия осталась бы
                     * «занятой» мёртвой транзакцией. */
                    modbus_worker_t *w = c->worker;
                    modbus_reset(&w->mb);
                    w->active = NULL;
                    free(c);
                    *context = w;             /* контекст регистрации — на место */
                    return 0;
                }
                if (c->queued)
                    mb_queue_remove(c->worker, c);
            }
            modbus_worker_t *w = c->worker;
            free(c);
            *context = w;                 /* контекст регистрации — на место */
        }
        return 0;
    }

    mb_call_ctx_t *c = (mb_call_ctx_t *)*context;

    if (data != NULL) {
        if (c && c->magic == MB_CTX_MAGIC) {
            /* переопрос подписки: если запрос ещё в очереди или на линии, второй
             * раз его не ставим — он уже будет выполнен */
            if (c->queued || (c->worker && c->worker->active == c))
                return 1;
            c->done = 0;
        } else {
            c = calloc(1, sizeof(*c));
            if (!c) {
                send_mb_error(call, MB_ERR_MEM);
                return 0;
            }
            c->magic  = MB_CTX_MAGIC;
            c->call   = call;
            c->worker = (modbus_worker_t *)*context;
            *context  = c;
        }

        if (!mb_call_parse(c, data, data_len)) {
            *context = c->worker;
            free(c);
            return 0;
        }
        if (c->worker->queue_len >= MB_QUEUE_MAX) {
            ESP_LOGW(TAG, "modbus порт %d: очередь полна (%d)", c->worker->port_no, MB_QUEUE_MAX);
            send_mb_error(call, MB_ERR_BUSY);
            *context = c->worker;
            free(c);
            return 0;
        }
        mb_queue_push(c->worker, c);
        /* Попробуем отдать драйверу сразу; если занят — очередь подождёт такта
         * работы, а обработчик задачу не блокирует. */
        mb_worker_start(c->worker);
        return 1;
    }

    if (!c)
        return 0;

    if (c->done) {
        /* Драйвер ответил и ответ уже ушёл клиенту: говорим ядру, что вызов
         * обработан — закрыть его или повторить (подписка) решает оно. */
        *context = c->worker;
        free(c);
        return 0;
    }

    modbus_worker_t *w = c->worker;

    if (w->active == c) {
        modbus_work(&w->mb);
        if (c->done) {
            *context = w;
            free(c);
            return 0;
        }
        if ((int32_t)(xTaskGetTickCount() - c->deadline) >= 0) {
            ESP_LOGW(TAG, "modbus: прибор не ответил на порту %d", w->port_no);
            modbus_reset(&w->mb);
            w->active = NULL;
            if (c->call) send_mb_error(c->call, MB_ERR_TIMEOUT);
            *context = w;
            free(c);
            return 0;
        }
        return 1;
    }

    /* Стоим в очереди: ждём без своего лимита по времени — вызов снимет ядро,
     * если клиент отключится. Продвигаем очередь, когда линия освободилась. */
    if (c->queued && w->head == c)
        mb_worker_start(w);
    return 1;
}

/* оба FID обязательны: порт, зарегистрированный молча, выглядел бы как «не
 * отвечает» */
static uint8_t mb_register(api_t *api) {
    uint8_t ok = 1;
    ok &= api_register_handler(api, mb_api_handler, ESP_WS_API_UART1_MODBUS_REQ, &mb_workers[0]);
    ok &= api_register_handler(api, mb_api_handler, ESP_WS_API_UART2_MODBUS_REQ, &mb_workers[1]);
    return ok;
}

esp_err_t ws_modbus_run(app_context_t *app) {
    for (int i = 0; i < 2; i++) {
        modbus_worker_t *w = &mb_workers[i];
        w->app_uart = &app->uart.port[i];
        w->port_no  = i;

        modbus_config_t cfg = {
            .pxIface             = &mb_iface,
            .pxTxContext         = &w->app_uart->desc,
            .pxTimerContext      = NULL,
            .pxRequestContext    = NULL,
            .pucPayLoadBuffer    = w->payload,
            .ucPayLoadBufferSize = MB_PAYLOAD_SIZE,
            .usTxTimeout         = MB_DEFAULT_AWT_MS,
            .usRxTimeout         = MB_DEFAULT_AWT_MS,
            .eMode               = MB_MODE_RTU,
            .bIsServer           = 0,
            .pfOnRequest         = NULL,
        };

        if (!modbus_init(&w->mb, &cfg)) {
            ESP_LOGE(TAG, "modbus_init failed for port %d", i);
            return ESP_FAIL;
        }

        app->uart.port[i].proto_context = &w->mb;

        /* Байты линии приходят событиями UART — отдаём их библиотеке. */
        w->rx_delegate.handler = mb_rx_handler;
        w->rx_delegate.context = w;
        gw_uart_on_receive_subscribe(&w->app_uart->desc, &w->rx_delegate);
    }

    /* Запросы к прибору идут через ядро API (оно зовёт обработчик и в такте
     * работы), поэтому своей задачи и очереди здесь нет; контекст регистрации —
     * воркер порта. Политики по интерфейсам у Modbus нет, поэтому оба FID
     * регистрируются на ОБОИХ ядрах, а ответ уходит через то ядро, откуда пришёл
     * вызов. */
    if (!mb_register(app->api_ap) || !mb_register(app->api_sta)) {
        ESP_LOGE(TAG, "не удалось зарегистрировать обработчики Modbus");
        return ESP_FAIL;
    }
    return ESP_OK;
}
