
#include "app.h"
#include "uart.h"
#include "cJSON_helpers.h"
#include "web_api.h"

#define MB_PAYLOAD_SIZE    255
#define MB_DEFAULT_AWT_MS  500

static uint32_t mb_timer_fn(const void *ctx) {
    (void)ctx;
    return xTaskGetTickCount();
}

static const modbus_iface_t mb_iface = {
    .pfRead  = gw_uart_read,
    .pfWrite = gw_uart_write,
    .pfTimer = mb_timer_fn,
};

typedef struct {
    modbus_t          mb;
    uint8_t           payload[MB_PAYLOAD_SIZE];
    queue_handle_t    queue;
    struct app_uart_t *app_uart;
    int               port_no;
} modbus_worker_t;

typedef struct {
    modbus_frame_t    frame;
    uint8_t           data[MB_PAYLOAD_SIZE];
    volatile uint8_t  done;
} mb_cb_ctx_t;

static modbus_worker_t mb_workers[2];
static queue_handle_t  mb_queues[2];

queue_handle_t get_modbus_worker_queue(uint32_t fid) {
    if (fid == ESP_WS_API_UART1_MODBUS_REQ) return mb_queues[0];
    if (fid == ESP_WS_API_UART2_MODBUS_REQ) return mb_queues[1];
    return NULL;
}

static void mb_on_complete(modbus_t *mb, void *ctx, modbus_frame_t *frame) {
    (void)mb;
    mb_cb_ctx_t *cb = (mb_cb_ctx_t *)ctx;
    if (frame) {
        cb->frame = *frame;
        if (frame->pucData && frame->ucBufferSize > 0) {
            uint8_t sz = frame->ucBufferSize < MB_PAYLOAD_SIZE ? frame->ucBufferSize : MB_PAYLOAD_SIZE;
            memcpy(cb->data, frame->pucData, sz);
            cb->frame.pucData = cb->data;
        } else {
            cb->frame.pucData = NULL;
        }
    } else {
        memset(&cb->frame, 0, sizeof(modbus_frame_t));
    }
    cb->done = 1;
}

static void send_mb_msg(int id, int fid, const char *json_str, size_t len) {
    webapi_msg_t msg;
    msg.id  = id;
    msg.fid = fid;
    if (json_str && len > 0) {
        msg.data = malloc(len + 1);
        if (msg.data) {
            memcpy(msg.data, json_str, len);
            msg.data[len] = '\0';
            msg.len = len;
        } else {
            msg.data = NULL;
            msg.len  = 0;
        }
    } else {
        msg.data = NULL;
        msg.len  = 0;
    }
    queue_send(get_ws_worker_queue(), &msg, pdMS_TO_TICKS(0));
}

static void send_mb_error(int id, int fid, int code) {
    char tmp[24];
    int  len = snprintf(tmp, sizeof(tmp), "{\"ERR\":%d}", code);
    send_mb_msg(id, fid, tmp, len);
}

static void format_and_send_mb_response(int id, int fid, mb_cb_ctx_t *cb) {
    modbus_frame_t *f = &cb->frame;
    uint8_t code = 0, amount = 0, size = 0;
    uint8_t *regs = modbus_frame_data(f, &code, &amount, &size);

    /* Estimate buffer: header ~80 bytes + per-register up to 7 bytes each */
    size_t buf_sz = 96 + (amount > 0 ? (size_t)(amount) * 9 : 0);
    char  *buf    = malloc(buf_sz);
    if (!buf) {
        send_mb_error(id, fid, 1);
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
                uint16_t v = ((uint16_t)regs[i * 2] << 8) | regs[i * 2 + 1];
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

    send_mb_msg(id, fid, buf, off);
    free(buf);
}

static void handle_modbus_msg(modbus_worker_t *w, webapi_msg_t *msg) {
    if (!msg->data || !msg->len) {
        send_mb_error(msg->id, msg->fid, 2);
        return;
    }

    cJSON *json = cJSON_ParseWithLengthOpts((char *)msg->data, msg->len, 0, 0);
    if (!json) {
        send_mb_error(msg->id, msg->fid, 2);
        return;
    }

    modbus_frame_t frame    = {0};
    uint8_t        wr_buf[MB_PAYLOAD_SIZE] = {0};
    uint32_t       awt_ms   = MB_DEFAULT_AWT_MS;
    uint8_t        parse_ok = 0;

    do {
        uint32_t val;
        if (!json_parse_int(json, "ADR", &val) || val > 255) break;
        frame.ucAddr = (uint8_t)val;

        if (!json_parse_int(json, "FN", &val) || val > 127) break;
        frame.ucFunc = (uint8_t)val;

        if (json_parse_int(json, "RA",  &val)) frame.usRegAddr       = (uint16_t)val;
        if (json_parse_int(json, "RVC", &val)) frame.usRegValueCount = (uint16_t)val;
        if (json_parse_int(json, "AWT", &val) && val > 0) awt_ms     = val;

        cJSON *rd = cJSON_GetObjectItem(json, "RD");
        if (rd && cJSON_IsArray(rd)) {
            int n = cJSON_GetArraySize(rd);
            if (n > 0) {
                int stride = (frame.ucFunc == MB_FUNC_WRITE_COILS) ? 1 : 2;
                if (n * stride > MB_PAYLOAD_SIZE) break;
                frame.ucBufferSize = (uint8_t)(n * stride);
                frame.pucData = wr_buf;
                cJSON *elem = rd->child;
                for (int i = 0; i < n && elem; i++, elem = elem->next) {
                    uint32_t v = (uint32_t)(int)elem->valueint;
                    if (stride == 2) {
                        wr_buf[i * 2]     = (v >> 8) & 0xff;
                        wr_buf[i * 2 + 1] = v & 0xff;
                    } else {
                        wr_buf[i] = v & 0xff;
                    }
                }
            }
        }

        parse_ok = 1;
    } while (0);

    cJSON_Delete(json);

    if (!parse_ok) {
        send_mb_error(msg->id, msg->fid, 2);
        return;
    }

    mb_cb_ctx_t cb = {0};

    /* Point the library's receive buffer at w->payload; cb copies it out in the callback */
    w->payload[0] = 0; /* clear */
    modbus_config_t cfg_update = {
        .pxIface            = &mb_iface,
        .pxRxContext        = &w->app_uart->desc,
        .pxTxContext        = &w->app_uart->desc,
        .pxTimerContext     = NULL,
        .pucPayLoadBuffer   = w->payload,
        .ucPayLoadBufferSize = MB_PAYLOAD_SIZE,
        .rx_timeout         = (uint16_t)(awt_ms),
        .tx_timeout         = (uint16_t)(awt_ms),
        .bAsciiMode         = 0,
        .bPduMode           = 0,
    };
    modbus_init(&w->mb, &cfg_update);

    uint32_t tid = modbus_request(&w->mb, &frame, mb_on_complete, &cb);
    if (!tid) {
        send_mb_error(msg->id, msg->fid, 3);
        return;
    }

    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(awt_ms + 500);
    while (!cb.done && (int32_t)(xTaskGetTickCount() - deadline) < 0) {
        modbus_work(&w->mb);
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    if (!cb.done) {
        modbus_cancel_request(&w->mb, tid);
        send_mb_error(msg->id, msg->fid, 4);
        return;
    }

    format_and_send_mb_response(msg->id, msg->fid, &cb);
}

static uint8_t mb_api_handler(void *call, void **context, uint32_t pending,
                               uint8_t *arg, uint32_t arg_len) {
    (void)call; (void)context; (void)pending; (void)arg; (void)arg_len;
    return 1;
}

static void ws_modbus_task(void *param) {
    modbus_worker_t *w = (modbus_worker_t *)param;
    webapi_msg_t    *msg;
    for (;;) {
        if (queue_receive(w->queue, &msg, portMAX_DELAY) == pdPASS) {
            handle_modbus_msg(w, msg);
            free(msg->data);
            free(msg);
        }
    }
}

esp_err_t ws_modbus_run(app_context_t *app) {
    for (int i = 0; i < 2; i++) {
        modbus_worker_t *w = &mb_workers[i];
        w->app_uart = &app->uart.port[i];
        w->port_no  = i;

        modbus_config_t cfg = {
            .pxIface            = &mb_iface,
            .pxRxContext        = &w->app_uart->desc,
            .pxTxContext        = &w->app_uart->desc,
            .pxTimerContext     = NULL,
            .pucPayLoadBuffer   = w->payload,
            .ucPayLoadBufferSize = MB_PAYLOAD_SIZE,
            .rx_timeout         = MB_DEFAULT_AWT_MS,
            .tx_timeout         = MB_DEFAULT_AWT_MS,
            .bAsciiMode         = 0,
            .bPduMode           = 0,
        };

        if (!modbus_init(&w->mb, &cfg)) {
            ESP_LOGE(TAG, "modbus_init failed for port %d", i);
            return ESP_FAIL;
        }

        mb_queues[i] = queue_create(5, sizeof(void *));
        w->queue = mb_queues[i];

        app->uart.port[i].proto_context = &w->mb;
    }

    api_call_register(&mb_api_handler, ESP_WS_API_UART1_MODBUS_REQ, NULL);
    api_call_register(&mb_api_handler, ESP_WS_API_UART2_MODBUS_REQ, NULL);

    BaseType_t r0 = xTaskCreatePinnedToCore(ws_modbus_task, "ws_mb0", 4096,
                                            &mb_workers[0], 5, NULL, tskNO_AFFINITY);
    BaseType_t r1 = xTaskCreatePinnedToCore(ws_modbus_task, "ws_mb1", 4096,
                                            &mb_workers[1], 5, NULL, tskNO_AFFINITY);

    return (r0 == pdPASS && r1 == pdPASS) ? ESP_OK : ESP_FAIL;
}
