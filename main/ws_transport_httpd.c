#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "CodeLib.h"
#include "connection.h"
#include "web_api.h"
#include "ws_transport.h"

static const char *TAG = "ws_transport";

/* esp_http_server answers the WebSocket handshake itself since ESP-IDF v6 and
 * returns before invoking the URI handler ("If the request is websocket
 * handshake, then do not call the uri->handler", httpd_uri.c). The handshake
 * request is handed to ws_post_handshake_cb instead, so the per-session
 * conn object has to be allocated from that callback. Up to v5.x the handler was
 * still called with method == HTTP_GET, which is where the allocation used to
 * live; that path is kept for older IDF versions. */
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
#define WS_CONN_FROM_POST_HANDSHAKE_CB 1
#else
#define WS_CONN_FROM_POST_HANDSHAKE_CB 0
#endif

#if WS_CONN_FROM_POST_HANDSHAKE_CB && !CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT
#error "ESP-IDF >= 6.0 never calls the URI handler for the WebSocket handshake: enable CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT"
#endif

/* Пределы транспорта. Они ровно те же, что были у каркаса, когда фрагменты
 * склеивал он: одиночный кадр 16 КБ и собранное сообщение 16 КБ — иначе
 * разрезанное сообщение и целый кадр имели бы разные пределы. При превышении
 * закрываем соединение (RFC 6455 §7.4.1, код 1009 — сообщение слишком велико).
 *
 * 16 КБ — по просьбе. Штатные запросы сильно меньше: запрос регистров ~80 байт,
 * строка терминала — base64 введённого текста, список файлов карт — сотни байт.
 */
#define WS_TRANSPORT_FRAME_MAX_LEN   16384
#define WS_TRANSPORT_MESSAGE_MAX_LEN 16384

/* Управляющий кадр WebSocket: payload не длиннее 125 байт (RFC 6455 §5.5). */
#define WS_CONTROL_PAYLOAD_MAX_LEN   125

/* Дескриптор соединения транспорта. Ядру он непрозрачен: ядро использует его
 * как ключ своей сессии и не знает ни этого типа, ни httpd. */
typedef struct {
    httpd_handle_t hd;
    int fd;
    delegate_t delegate;
    /* Через какой сетевой интерфейс пришёл клиент (GW_IFACE_*). Определяется
     * ОДИН раз при установлении соединения: интерфейс соединения не меняется, и
     * дёргать сеть на каждом событии незачем. Ядру это отдаётся через
     * peer_iface(). */
    uint32_t ulIface;
    /* Сборка СООБЩЕНИЯ из фрагментов: WebSocket разрешает резать одно сообщение
     * на кадры (первый с FIN=0, дальше продолжения). Буфер принадлежит
     * транспорту и живёт ровно до возврата из обработчика события RECEIVED. */
    uint8_t *pucMsg;
    uint32_t ulMsgLen;
    uint32_t ulMsgCap;
    /* Сколько отправок сейчас в работе. Дескриптор нельзя освободить, пока
     * хоть одна из них не завершилась: колбэк httpd приходит позже закрытия
     * сессии и получает указатель на него. */
    uint32_t ulSendsInFlight;
    /* Сессия ядра уже закрыта (CLOSED отправлен), но отправки ещё в работе:
     * память дескриптора освободит последний колбэк. */
    uint8_t  bDetached;
} ws_transport_httpd_conn_t;

/* Контекст одной асинхронной отправки: httpd отдаёт колбэку только свой arg, а
 * нам нужно помнить, чья это отправка и какие данные ядру вернуть. */
typedef struct {
    ws_transport_httpd_conn_t *conn;
    const void *data;
    uint32_t length;
} ws_transport_httpd_send_t;

/* Дескриптор транспорта как модуля (self для init/open/subscribe). Живёт
 * статически: транспорт в системе один. */
typedef struct {
    httpd_uri_t *uri;
    uint8_t opened;
    ws_transport_event_handler_t handler;
    void *context;      /* хранится транспортом; сигнатуре события не нужен */
} ws_transport_httpd_module_t;

static ws_transport_httpd_module_t xModule;

/* Настройки транспорта. */
typedef struct {
    const char *uri;
} ws_transport_httpd_config_t;

/* Обработчик URI и открытие соединения описаны ниже: iface ссылается на них
 * из своей таблицы. */
static esp_err_t wsHttpdHandler(httpd_req_t *req);
#if WS_CONN_FROM_POST_HANDSHAKE_CB
static esp_err_t ws_httpd_post_handshake(httpd_req_t *req);
#endif
static ws_transport_httpd_conn_t *pxConnOpen(httpd_req_t *req, int fd);

/* --- Служебное ------------------------------------------------------------ */

static void vNotify(ws_transport_httpd_conn_t *conn, const ws_transport_event_trigger_t *ev) {
    if(xModule.handler != NULL) xModule.handler(conn, ev);
}

/* free_ctx у httpd принимает void*, поэтому обёртка: внутри — своя логика на
 * типизированном дескрипторе. */
static void vSessionFreeCtx(void *ctx) {
    ws_transport_httpd_conn_t *conn = (ws_transport_httpd_conn_t *)ctx;
    if(conn == NULL) return;
    event_unsubscribe(&conn->delegate);
    free(conn->pucMsg);
    conn->pucMsg    = NULL;
    conn->ulMsgLen  = 0;
    conn->ulMsgCap  = 0;

    /* Ядру сообщаем о разрыве: свою сессию оно освободит по этому событию.
     * Дескриптор при этом ещё жив: ядро могло закрыть соединение само. */
    ws_transport_event_trigger_t ev = { .event = WS_TRANSPORT_CLOSED };
    vNotify(conn, &ev);

    if(conn->ulSendsInFlight) {
        /* Отправки ещё в работе и получат этот указатель в колбэке — освободит
         * память последний из них (см. wsHttpdSendDone). */
        conn->bDetached = 1;
        return;
    }
    free(conn);
}

static void link_event_handler(void *event_trigger, void *sender, void *context) {
    (void)event_trigger; (void)sender;
    ws_transport_httpd_conn_t *conn = (ws_transport_httpd_conn_t *)context;
    ESP_LOGI(TAG, "Link down, triggering close for fd=%d", conn->fd);
    httpd_sess_trigger_close(conn->hd, conn->fd);
}

/* --- Операции транспорта как модуля --------------------------------------- */

static ws_transport_result_t wsHttpdInit(const void *self, const void *config) {
    ws_transport_httpd_module_t *m = (ws_transport_httpd_module_t *)self;
    const ws_transport_httpd_config_t *cfg = (const ws_transport_httpd_config_t *)config;
    if((m == NULL) || (cfg == NULL) || (cfg->uri == NULL)) return WS_TRANSPORT_BAD_CALL;
    if(m->uri != NULL) return WS_TRANSPORT_BUSY;   /* уже инициализирован */

    httpd_uri_t *h = calloc(1, sizeof(httpd_uri_t));
    if(h == NULL) return WS_TRANSPORT_FAULT;

    h->uri                      = cfg->uri;
    h->method                   = HTTP_GET;
    h->handler                  = wsHttpdHandler;
    h->is_websocket             = true;
    /* Управляющие кадры (PING/PONG/CLOSE) забирает обработчик транспорта — с
     * этим флагом httpd отдаёт их нам, а не разбирает сам. Ядру API они не
     * приходят вовсе (см. wsHttpdControlFrame). */
    h->handle_ws_control_frames = true;
#if WS_CONN_FROM_POST_HANDSHAKE_CB
    h->ws_post_handshake_cb     = ws_httpd_post_handshake;
#endif
    m->uri = h;
    ESP_LOGI(TAG, "Init, uri=%s", cfg->uri);
    return WS_TRANSPORT_OK;
}

/* «Начать слушать». esp_http_server поднимает web_server.c, а соединения
 * принимает httpd, поэтому отдельного «начали слушать» у транспорта нет: open
 * означает, что обработчик URI готов и его можно регистрировать. */
static ws_transport_result_t wsHttpdOpen(const void *self, const void *config) {
    (void)config;
    ws_transport_httpd_module_t *m = (ws_transport_httpd_module_t *)self;
    if((m == NULL) || (m->uri == NULL)) return WS_TRANSPORT_BAD_CALL;
    m->opened = 1;
    ESP_LOGI(TAG, "Open, uri=%s ready for registration", m->uri->uri);
    return WS_TRANSPORT_OK;
}

static ws_transport_result_t wsHttpdSubscribe(const void *self, ws_transport_event_handler_t handler,
                                              void *context) {
    ws_transport_httpd_module_t *m = (ws_transport_httpd_module_t *)self;
    if((m == NULL) || (handler == NULL)) return WS_TRANSPORT_BAD_CALL;
    /* Подписчик один — ядро API. context храним, но событию он не нужен:
     * дескриптор соединения в событии уже говорит, о ком речь. */
    m->handler = handler;
    m->context = context;
    ESP_LOGI(TAG, "Subscribed");
    return WS_TRANSPORT_OK;
}

/* --- Операции соединения -------------------------------------------------- */

static ws_transport_result_t wsHttpdClose(const void *self) {
    ws_transport_httpd_conn_t *conn = (ws_transport_httpd_conn_t *)self;
    if(conn == NULL) return WS_TRANSPORT_BAD_CALL;
    esp_err_t err = httpd_sess_trigger_close(conn->hd, conn->fd);
    if(err != ESP_OK) {
        ESP_LOGW(TAG, "Close fd=%d failed (err %d)", conn->fd, err);
        return WS_TRANSPORT_BAD_CALL;   /* сессии уже нет */
    }
    return WS_TRANSPORT_OK;
}

/* Отправка идёт одним куском: сообщение целиком уходит в httpd, он сам строит
 * WS-кадр и сам режет его на TCP-сегменты. Прерывать нечего. */
static ws_transport_result_t wsHttpdBreakSending(const void *self) {
    (void)self;
    return WS_TRANSPORT_OK;
}

static void wsHttpdSendDone(esp_err_t err, int fd, void *arg) {
    (void)fd;
    ws_transport_httpd_send_t *snd = (ws_transport_httpd_send_t *)arg;
    ws_transport_httpd_conn_t *conn = snd->conn;

    ws_transport_event_trigger_t ev = {
        .event = err ? WS_TRANSPORT_TRANSFER_ERR : WS_TRANSPORT_TRANSFER_COMPLETE,
    };
    ev.data.ptr    = snd->data;      /* тот же указатель, что дало ядро */
    ev.data.length = snd->length;
    ev.data.left   = 0;              /* сообщение отдано целиком */
    if(err) {
        /* Отправка не удалась: соединение считаем мёртвым, ядро закроет его
         * само (см. TRANSFER_ERR в ws_server.c). */
        ESP_LOGW(TAG, "Send failed (err %d), fd=%d", err, conn->fd);
    }

    ESP_LOGD(TAG, "write fd=%d %lu Б: %s", conn->fd, (unsigned long)snd->length,
             err ? esp_err_to_name(err) : "ок");
    free(snd);
    if(conn->ulSendsInFlight) conn->ulSendsInFlight--;

    /* Ядро должно узнать о завершении отправки даже у уже закрытой сессии:
     * так оно освобождает свой буфер ответа. В этот момент дескриптор ещё жив. */
    vNotify(conn, &ev);

    if(conn->bDetached && !conn->ulSendsInFlight) free(conn);
}

/* Сколько ждать места в рабочей очереди httpd, прежде чем признать отправку
 * неудавшейся. Очередь разбирается задачей httpd за единицы миллисекунд, поэтому
 * ожидание короткое, а потерянных кадров не остаётся. */
#define WS_TRANSPORT_SEND_RETRY_MAX 200
#define WS_TRANSPORT_SEND_RETRY_MS  1

static ws_transport_result_t wsHttpdBeginSend(const void *self, const void *data, uint32_t length) {
    ws_transport_httpd_conn_t *conn = (ws_transport_httpd_conn_t *)self;
    if((conn == NULL) || (data == NULL) || (length == 0)) return WS_TRANSPORT_BAD_CALL;

    ws_transport_httpd_send_t *snd = malloc(sizeof(*snd));
    if(snd == NULL) return WS_TRANSPORT_FAULT;
    snd->conn   = conn;
    snd->data   = data;
    snd->length = length;

    /* httpd только читает payload, поэтому const снимаем осознанно: данные
     * принадлежат ядру и живут до события завершения отправки. */
    httpd_ws_frame_t f = {
        .type       = HTTPD_WS_TYPE_TEXT,
        .final      = true,
        .fragmented = false,
        .payload    = (uint8_t *)data,
        .len        = length,
    };

    /* Отправка идёт через рабочую очередь httpd, а она ограничена по глубине. На
     * потоке мелких кадров (эхо набора в терминале — кадр на символ) очередь
     * переполнялась, httpd_ws_send_data_async отвечал отказом, и кадр ПРОПАДАЛ:
     * ядро считало отправку неудавшейся и освобождало буфер. Замерено на стенде:
     * устройство отдало 38 кадров, клиент получил 5 — то есть «терминал теряет
     * ввод» было потерей именно здесь. Ждём место в очереди, а не теряем данные. */
    esp_err_t err = ESP_FAIL;
    for(int attempt = 0; attempt <= WS_TRANSPORT_SEND_RETRY_MAX; attempt++) {
        conn->ulSendsInFlight++;
        err = httpd_ws_send_data_async(conn->hd, conn->fd, &f, wsHttpdSendDone, snd);
        if(err == ESP_OK) return WS_TRANSPORT_OK;
        conn->ulSendsInFlight--;

        if(attempt < WS_TRANSPORT_SEND_RETRY_MAX)
            vTaskDelay(pdMS_TO_TICKS(WS_TRANSPORT_SEND_RETRY_MS));
    }

    /* Очередь так и не освободилась — об этом должно быть видно в логе, иначе
     * «иногда теряется» останется загадкой. Колбэка не будет, буфер освободит
     * ядро. */
    ESP_LOGW(TAG, "Отправка не принята за %d мс (fd=%d, %lu Б): %s",
             WS_TRANSPORT_SEND_RETRY_MAX * WS_TRANSPORT_SEND_RETRY_MS, conn->fd,
             (unsigned long)length, esp_err_to_name(err));
    free(snd);
    if(err == ESP_ERR_NO_MEM) return WS_TRANSPORT_FAULT;
    return WS_TRANSPORT_BAD_CALL;
}

static int32_t wsHttpdPeerId(const void *self) {
    const ws_transport_httpd_conn_t *conn = (const ws_transport_httpd_conn_t *)self;
    return conn ? conn->fd : -1;
}

/* Интерфейс соединения — свойство дескриптора, а не сокета: посчитан при
 * установлении (pxConnOpen). */
static uint32_t wsHttpdPeerIface(const void *self) {
    const ws_transport_httpd_conn_t *conn = (const ws_transport_httpd_conn_t *)self;
    return conn ? conn->ulIface : GW_IFACE_NONE;
}

static const ws_transport_iface_t xIface = {
    .init          = wsHttpdInit,
    .open          = wsHttpdOpen,
    .close         = wsHttpdClose,
    .begin_send    = wsHttpdBeginSend,
    .break_sending = wsHttpdBreakSending,
    .subscribe     = wsHttpdSubscribe,
    .peer_id       = wsHttpdPeerId,
    .peer_iface    = wsHttpdPeerIface,
};

/* --- Приём ---------------------------------------------------------------- */

/* Запас места под ещё len байт собранного сообщения. NULL — сообщение вышло за
 * предел (или нет памяти): тогда соединение закрываем, принимать его негде. */
static uint8_t *pucMsgReserve(ws_transport_httpd_conn_t *conn, uint32_t len) {
    if(len > WS_TRANSPORT_MESSAGE_MAX_LEN) return NULL;
    if(conn->ulMsgCap < conn->ulMsgLen + len + 1) {
        uint32_t cap = conn->ulMsgLen + len + 1;
        uint8_t *buf = realloc(conn->pucMsg, cap);
        if(buf == NULL) return NULL;
        conn->pucMsg   = buf;
        conn->ulMsgCap = cap;
    }
    return conn->pucMsg + conn->ulMsgLen;
}

/* Отбросить недособранное сообщение. */
static void vDropMessage(ws_transport_httpd_conn_t *conn) {
    free(conn->pucMsg);
    conn->pucMsg   = NULL;
    conn->ulMsgLen = 0;
    conn->ulMsgCap = 0;
}

/* Собранное сообщение — ядру одним событием RECEIVED. Уровень сообщений: ядро
 * фрагментов не видит вовсе. Буфер освобождаем после возврата из обработчика:
 * ядро читает его только там. */
static void vDeliverMessage(ws_transport_httpd_conn_t *conn) {
    if(conn->ulMsgLen == 0) {
        /* Пустое сообщение ядру не нужно: разбирать в нём нечего. */
        vDropMessage(conn);
        return;
    }
    conn->pucMsg[conn->ulMsgLen] = '\0';   /* ядру удобно видеть строку в логе */

    ws_transport_event_trigger_t ev = { .event = WS_TRANSPORT_RECEIVED };
    ev.data.ptr    = conn->pucMsg;
    ev.data.length = conn->ulMsgLen;
    ev.data.left   = 0;                    /* сообщение отдано целиком */
    vNotify(conn, &ev);

    free(conn->pucMsg);
    conn->pucMsg   = NULL;
    conn->ulMsgLen = 0;
    conn->ulMsgCap = 0;
}

/* Чтение управляющего кадра. Обрабатывает их транспорт: ядру PING/PONG/CLOSE не
 * отдаются ни при каких условиях. При handle_ws_control_frames = true их забирает
 * обработчик URI (наши PONG и CLOSE никто за нас не отправит), при выключенном
 * флаге PING и CLOSE берёт на себя httpd. */
static esp_err_t wsHttpdControlFrame(httpd_req_t *req, httpd_ws_frame_t *f) {
    int fd = httpd_req_to_sockfd(req);
    uint8_t payload[WS_CONTROL_PAYLOAD_MAX_LEN + 1];

    if(f->len > WS_CONTROL_PAYLOAD_MAX_LEN) {
        ESP_LOGW(TAG, "Управляющий кадр длиной %u — не по RFC 6455, закрываю fd=%d",
                 (unsigned)f->len, fd);
        httpd_sess_trigger_close(req->handle, fd);
        return ESP_OK;
    }
    /* Payload управляющего кадра надо вычитать из сокета даже если он нам не
     * нужен: иначе следующий кадр разберётся не с начала. */
    if(f->len) {
        f->payload = payload;
        esp_err_t ret = httpd_ws_recv_frame(req, f, f->len);
        if(ret != ESP_OK) {
            ESP_LOGW(TAG, "Ошибка чтения управляющего кадра (err %d), fd=%d", ret, fd);
            return ret;
        }
    }

    if((f->type == HTTPD_WS_TYPE_PING) || (f->type == HTTPD_WS_TYPE_CLOSE)) {
        /* На PING отвечаем PONG, на CLOSE — CLOSE (RFC 6455 §5.5.2, §5.5.1).
         * Отправка идёт прямо отсюда, буфер на стеке живёт до её конца. */
        httpd_ws_frame_t reply = {
            .type    = (f->type == HTTPD_WS_TYPE_PING) ? HTTPD_WS_TYPE_PONG : HTTPD_WS_TYPE_CLOSE,
            .final   = true,
            .payload = f->len ? payload : NULL,
            .len     = f->len,
        };
        esp_err_t err = httpd_ws_send_frame_async(req->handle, fd, &reply);
        if(err != ESP_OK) ESP_LOGW(TAG, "Ответ на управляющий кадр не ушёл (err %d), fd=%d", err, fd);
    }
    /* PONG обрабатывать нечего: своих PING транспорт не посылает. */
    return ESP_OK;
}

static esp_err_t wsHttpdHandler(httpd_req_t *req) {
    int fd = httpd_req_to_sockfd(req);

    if (req->method == HTTP_GET) {
#if WS_CONN_FROM_POST_HANDSHAKE_CB
        /* Handshake already answered by esp_http_server; the session was created
         * in ws_httpd_post_handshake() above. */
        return ESP_OK;
#else
        if (!pxConnOpen(req, fd)) return ESP_ERR_NO_MEM;
        return ESP_OK;
#endif
    }

    ws_transport_httpd_conn_t *conn = (ws_transport_httpd_conn_t *)req->sess_ctx;
    if (!conn) {
        /* Without the session object there is no one to dispatch to; dropping
         * the frame is better than dereferencing NULL. */
        ESP_LOGW(TAG, "WS frame without session context, fd=%d", fd);
        return ESP_FAIL;
    }

    httpd_ws_frame_t f = {0};
    esp_err_t ret = httpd_ws_recv_frame(req, &f, 0);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to read ws header (err %d)", ret);
        return ret;
    }

    if((f.type == HTTPD_WS_TYPE_PING) || (f.type == HTTPD_WS_TYPE_PONG) ||
       (f.type == HTTPD_WS_TYPE_CLOSE))
        return wsHttpdControlFrame(req, &f);

    /* Одиночный кадр больше предела: не читаем и не разбираем — закрываем
     * соединение (клиент получает закрытие вместо ответа API, потому что FID и
     * SID этого кадра мы намеренно не разбираем). */
    if (f.len > WS_TRANSPORT_FRAME_MAX_LEN) {
        ESP_LOGW(TAG, "Кадр %u байт больше предела %d — закрываю fd=%d",
                 (unsigned)f.len, WS_TRANSPORT_FRAME_MAX_LEN, fd);
        httpd_sess_trigger_close(req->handle, fd);
        return ESP_OK;
    }

    if(f.type == HTTPD_WS_TYPE_CONTINUE) {
        if(conn->pucMsg == NULL) {
            /* Продолжение без начала: склеивать не с чем, кадр отбрасываем. */
            ESP_LOGW(TAG, "Продолжение без начала сообщения, отброшено fd=%d", fd);
            return ESP_OK;
        }
        uint8_t *dst = pucMsgReserve(conn, f.len);
        if(dst == NULL) {
            ESP_LOGW(TAG, "Сообщение больше %d байт — закрываю fd=%d",
                     WS_TRANSPORT_MESSAGE_MAX_LEN, fd);
            vDropMessage(conn);
            httpd_sess_trigger_close(req->handle, fd);
            return ESP_OK;
        }
        f.payload = dst;
        if(f.len) {
            ret = httpd_ws_recv_frame(req, &f, f.len);
            if(ret != ESP_OK) {
                ESP_LOGW(TAG, "Failed to read ws payload (err %d)", ret);
                vDropMessage(conn);
                return ret;
            }
        }
        conn->ulMsgLen += f.len;
        if(f.final) vDeliverMessage(conn);
        return ESP_OK;
    }

    /* Начало нового сообщения (TEXT/BINARY). Незавершённое предыдущее
     * выбрасываем: целым оно уже не станет, а склеивать его с новым нельзя. */
    if(conn->pucMsg != NULL) {
        ESP_LOGW(TAG, "Незавершённое сообщение отброшено, fd=%d", fd);
        vDropMessage(conn);
    }

    uint8_t *dst = pucMsgReserve(conn, f.len);
    if(dst == NULL) {
        ESP_LOGW(TAG, "Нет памяти под кадр %u байт, закрываю fd=%d", (unsigned)f.len, fd);
        httpd_sess_trigger_close(req->handle, fd);
        return ESP_OK;
    }
    f.payload = dst;
    if(f.len) {
        ret = httpd_ws_recv_frame(req, &f, f.len);
        if(ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to read ws payload (err %d)", ret);
            vDropMessage(conn);
            return ret;
        }
    }
    conn->ulMsgLen += f.len;
    /* Последний кадр — сообщение целиком; иначе ждём продолжений. */
    if(f.final) vDeliverMessage(conn);
    return ESP_OK;
}

/* --- Подъём --------------------------------------------------------------- */

/* Allocates the connection object once per session and hands it to the API core.
 * It is stored in req->sess_ctx: esp_http_server propagates that into the
 * session on request cleanup and copies it back into every later request of the
 * same session, while vSessionFreeCtx releases it when the session goes away. */
static ws_transport_httpd_conn_t *pxConnOpen(httpd_req_t *req, int fd) {
    ws_transport_httpd_conn_t *conn = (ws_transport_httpd_conn_t *)req->sess_ctx;
    if (conn) return conn;

    conn = calloc(1, sizeof(ws_transport_httpd_conn_t));
    if (!conn) return NULL;
    conn->hd = req->handle;
    conn->fd = fd;
    /* Интерфейс выясняем здесь, один раз на соединение, — до того, как ядро
     * заведёт по нему сессию (ESTABLISHED ниже). */
    conn->ulIface = gw_iface_of_socket(fd);
    conn->delegate.handler = link_event_handler;
    conn->delegate.context = conn;
    socket_link_subscribe(fd, &conn->delegate);

    ws_transport_event_trigger_t ev = { .event = WS_TRANSPORT_ESTABLISHED };
    if(xModule.handler == NULL || xModule.handler(conn, &ev) != WS_TRANSPORT_OK) {
        /* Ядру это соединение обслуживать нечем — не заводим сессию вовсе. */
        ESP_LOGE(TAG, "Ядро не приняло соединение, fd=%d", fd);
        event_unsubscribe(&conn->delegate);
        free(conn);
        return NULL;
    }

    req->sess_ctx = conn;
    req->free_ctx = vSessionFreeCtx;
    return conn;
}

#if WS_CONN_FROM_POST_HANDSHAKE_CB
/* The handshake response has already been sent when this runs; the socket is a
 * WebSocket from here on. */
static esp_err_t ws_httpd_post_handshake(httpd_req_t *req) {
    int fd = httpd_req_to_sockfd(req);
    if (!pxConnOpen(req, fd)) {
        ESP_LOGE(TAG, "WS session not created, fd=%d", fd);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "WS session opened, fd=%d", fd);
    return ESP_OK;
}
#endif

httpd_uri_t *ws_transport_httpd_init(char *uri) {
    ws_transport_httpd_config_t cfg = { .uri = uri };

    if(xIface.init(&xModule, &cfg) != WS_TRANSPORT_OK) return NULL;
    if(xIface.open(&xModule, NULL) != WS_TRANSPORT_OK) return NULL;
    if(ws_server_transport_attach(&xIface, &xModule) != WS_TRANSPORT_OK) return NULL;

    return xModule.uri;
}
