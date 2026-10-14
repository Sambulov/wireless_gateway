
#include "app.h"
#include "uart.h"
#include "GwJson.h"
#include "web_api.h"
#include "ModBusHelpers.h"

#define MB_PAYLOAD_SIZE    255
#define MB_DEFAULT_AWT_MS  500

/* Токенов на разбор ARG: ключи ADR/FN/RA/RVC/AWT — по два токена, плюс "RD" и
 * его элементы. Список значений на запись длиннее этого отвергается разбором
 * (клиент шлёт регистры маленькими пачками). */
#define MB_ARG_MAX_TOKENS  48

static uint32_t mb_timer_fn(const void *ctx) {
    (void)ctx;
    return xTaskGetTickCount() * portTICK_PERIOD_MS;
}

/* В новом CL приём устроен наоборот: библиотека больше не читает сама, а
 * получает байты через modbus_receive_data (см. mb_rx_handler). В интерфейсе
 * остались только передача и таймер. */
static const modbus_iface_t mb_iface = {
    .pfWrite = gw_uart_write,
    .pfTimer = mb_timer_fn,
};

typedef struct {
    modbus_t          mb;
    uint8_t           payload[MB_PAYLOAD_SIZE];
    queue_handle_t    queue;
    struct app_uart_t *app_uart;
    int               port_no;
    delegate_t        rx_delegate;   /* приём байт линии в библиотеку Modbus */
    void             *owner;         /* вызов, занимающий линию */
} modbus_worker_t;

typedef struct {
    modbus_frame_t    frame;
    uint8_t           data[MB_PAYLOAD_SIZE];
    volatile uint8_t  done;
} mb_cb_ctx_t;

/* Состояние одного вызова API: разобранный запрос, идущая транзакция и её
 * результат. Живёт в контексте вызова, который принадлежит обработчику. */
#define MB_CTX_MAGIC 0x4d424354u   /* 'MBCT' — признак своего контекста */

typedef struct {
    uint32_t         magic;
    void            *call;
    modbus_worker_t *worker;
    mb_cb_ctx_t      cb;
    modbus_frame_t   req;                       /* разобранный запрос клиента */
    uint8_t          wr_buf[MB_PAYLOAD_SIZE];
    uint16_t         awt_ms;
    TickType_t       t_req;
    TickType_t       deadline;
    uint8_t          started;                   /* транзакция запущена */
} mb_call_ctx_t;

static modbus_worker_t mb_workers[2];

/* Колбэк нового CL: (кадр, дескриптор, контекст). Длину данных берём из
 * ucLengthCode — поля ucBufferSize у кадра больше нет. */
static void mb_on_complete(modbus_frame_t *frame, modbus_t *mb, void *ctx) {
    (void)mb;
    mb_cb_ctx_t *cb = (mb_cb_ctx_t *)ctx;
    if (frame) {
        cb->frame = *frame;
        uint8_t sz = frame->ucLengthCode;
        if (frame->pucData && sz > 0) {
            memcpy(cb->data, frame->pucData, sz);
            cb->frame.pucData = cb->data;
            cb->frame.ucLengthCode = sz;
        } else {
            cb->frame.pucData = NULL;
            cb->frame.ucLengthCode = 0;
        }
    } else {
        memset(&cb->frame, 0, sizeof(modbus_frame_t));
    }
    cb->done = 1;
}

/* Байты из UART отдаём библиотеке сами. */
static void mb_rx_handler(void *trigger, void *sender, void *context) {
    (void)sender;
    modbus_worker_t *w = (modbus_worker_t *)context;
    gw_uart_event_data_t *ev = (gw_uart_event_data_t *)trigger;
    if (w && ev && ev->buf && ev->size) {
        modbus_receive_data(&w->mb, ev->buf, (uint16_t)ev->size, ev->b_end);
    }
}

/* Ответ клиенту отправляем через сам вызов — ядро само обрамит его в FID/SID. */
static void send_mb_msg(void *call, const char *json_str, size_t len) {
    if (json_str && len > 0)
        bApiCallSendJson(call, (const uint8_t *)json_str, len);
}

static void send_mb_error(void *call, int code) {
    char tmp[24];
    int  len = snprintf(tmp, sizeof(tmp), "{\"ERR\":%d}", code);
    send_mb_msg(call, tmp, len);
}

static void format_and_send_mb_response(void *call, mb_cb_ctx_t *cb) {
    modbus_frame_t *f = &cb->frame;
    uint8_t code = 0, amount = 0, size = 0;
    uint8_t *regs = pucModbusExtructFrameData(f, &code, &amount, &size);

    /* Estimate buffer: header ~80 bytes + per-register up to 7 bytes each */
    size_t buf_sz = 96 + (amount > 0 ? (size_t)(amount) * 9 : 0);
    char  *buf    = malloc(buf_sz);
    if (!buf) {
        send_mb_error(call, 1);
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

/* Запуск транзакции по первичному запросу клиента. Возвращает 1, если
 * транзакция пошла (ответ отправим из такта работы), 0 — если не удалось. */
static uint8_t mb_call_start(mb_call_ctx_t *ctx, uint8_t *data, uint32_t len) {
    if (!data || !len) {
        send_mb_error(ctx->call, 2);
        return 0;
    }

    /* Разбор ARG: jsmn, без дерева и без выделения памяти на элемент. Массив
     * RD ограничен числом токенов, а не кучей: слишком длинный список значений
     * отвергается разбором. */
    GW_JSON_TOKENS(toks, MB_ARG_MAX_TOKENS);
    gw_json_t json;
    if (!gw_json_parse(&json, (const char *)data, len, toks, MB_ARG_MAX_TOKENS)) {
        send_mb_error(ctx->call, 2);
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
        send_mb_error(ctx->call, 2);
        return 0;
    }

    /* Линию здесь НЕ трогаем: сброс и настройка делаются в момент старта
     * транзакции. Иначе разбор второго одновременного запроса сбрасывал бы
     * линию под уже идущей транзакцией первого, и таймаут получали все. */
    ctx->req       = frame;                 /* pucData уже указывает на wr_buf */
    ctx->awt_ms    = (uint16_t)awt_ms;
    ctx->t_req     = xTaskGetTickCount();
    ctx->deadline  = ctx->t_req + pdMS_TO_TICKS(awt_ms + 500);
    ctx->started   = 0;
    return 1;
}

/* Запуск транзакции на линии (когда порт свободен) и её продолжение по такту
 * работы. Возвращает 1, пока вызов жив, и 0, когда ответ отправлен. */
static uint8_t mb_call_step(mb_call_ctx_t *ctx) {
    modbus_worker_t *w = ctx->worker;

    if (!ctx->started) {
        if (w->owner && w->owner != ctx) return 1;   /* линия занята — ждём */
        w->owner = ctx;
        /* Линия наша: сбрасываем состояние (могли остаться байты прошлого
         * обмена) и задаём таймауты этого запроса. */
        modbus_reset(&w->mb);
        w->payload[0] = 0;
        modbus_config_t cfg_req = {
            .pxIface             = &mb_iface,
            .pxTxContext         = &w->app_uart->desc,
            .pxTimerContext      = NULL,
            .pxRequestContext    = NULL,
            .pucPayLoadBuffer    = w->payload,
            .ucPayLoadBufferSize = MB_PAYLOAD_SIZE,
            .usTxTimeout         = ctx->awt_ms,
            .usRxTimeout         = ctx->awt_ms,
            .eMode               = MB_MODE_RTU,
            .bIsServer           = 0,
            .pfOnRequest         = NULL,
        };
        modbus_init(&w->mb, &cfg_req);
        /* Отсчёт таймаута — от старта транзакции, а не от прихода запроса:
         * пока вызов ждал линию, его время не должно сгорать. */
        ctx->t_req    = xTaskGetTickCount();
        ctx->deadline = ctx->t_req + pdMS_TO_TICKS(ctx->awt_ms + 500);
        uint16_t tid = modbus_request(&w->mb, &ctx->req, mb_on_complete, &ctx->cb,
                                      ctx->awt_ms);
        if (!tid) {
            w->owner = NULL;
            send_mb_error(ctx->call, 3);
            return 0;
        }
        ctx->started = 1;
        /* Кадр уходит в линию сразу, не дожидаясь следующего такта: иначе
         * отправка стоит ещё один такт работы. */
        modbus_work(&w->mb);
        return 1;
    }

    modbus_work(&w->mb);

    if (ctx->cb.done) {
        w->owner = NULL;
        format_and_send_mb_response(ctx->call, &ctx->cb);
        return 0;
    }

    if ((int32_t)(xTaskGetTickCount() - ctx->deadline) >= 0) {
        modbus_reset(&w->mb);
        w->owner = NULL;
        ESP_LOGW(TAG, "modbus: таймаут транзакции на порту %d", w->port_no);
        send_mb_error(ctx->call, 4);
        return 0;
    }
    return 1;
}

/* Обработчик запросов к прибору по контракту API:
 *   data != NULL — первичный запрос клиента: разбираем ARG и готовим транзакцию;
 *   data == NULL — такт работы: держим линию и, получив ответ, отправляем его;
 *   call == NULL — принудительное завершение: освобождаем контекст вызова.
 * Ожидания внутри нет: пока прибор не ответил, обработчик возвращает TRUE. */
static uint8_t mb_api_handler(void *call, void **context, uint8_t *data, uint32_t data_len) {
    if (!call) {
        mb_call_ctx_t *ctx = (mb_call_ctx_t *)*context;
        if (ctx) {
            if (ctx->worker && ctx->worker->owner == ctx) {
                ctx->worker->owner = NULL;
                modbus_reset(&ctx->worker->mb);
            }
            free(ctx);
        }
        *context = NULL;
        return 0;
    }

    mb_call_ctx_t *ctx = (mb_call_ctx_t *)*context;

    if (data != NULL) {
        if (ctx && ctx->magic == MB_CTX_MAGIC) {
            /* Повторный запрос по тому же вызову (длинная подписка): начинаем
             * заново, контекст не создаём заново, иначе он утечёт. */
            if (ctx->worker && ctx->worker->owner == ctx) {
                ctx->worker->owner = NULL;
                modbus_reset(&ctx->worker->mb);
            }
            ctx->cb.done = 0;
        } else {
            /* Первичный вызов: контекст регистрации (порт) заменяем своим. */
            ctx = calloc(1, sizeof(*ctx));
            if (!ctx) {
                send_mb_error(call, 1);
                return 0;
            }
            ctx->magic  = MB_CTX_MAGIC;
            ctx->call   = call;
            ctx->worker = (modbus_worker_t *)*context;
            *context    = ctx;
        }
        if (!mb_call_start(ctx, data, data_len)) {
            /* не разобрали запрос — ответ уже отправлен, вызов завершён */
            *context = ctx->worker;
            free(ctx);
            return 0;
        }
        return 1;
    }

    if (!ctx) return 0;
    return mb_call_step(ctx);
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

    /* Запросы к прибору идут через ядро API: оно само вызывает обработчик и в
     * такте работы, поэтому своей задачи и очереди здесь больше нет. */
    if (!bApiCallRegister(mb_api_handler, ESP_WS_API_UART1_MODBUS_REQ, &mb_workers[0]) ||
        !bApiCallRegister(mb_api_handler, ESP_WS_API_UART2_MODBUS_REQ, &mb_workers[1])) {
        ESP_LOGE(TAG, "не удалось зарегистрировать обработчики Modbus");
        return ESP_FAIL;
    }
    return ESP_OK;
}
