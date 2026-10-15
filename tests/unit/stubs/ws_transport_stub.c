#include "ws_transport_stub.h"
#include "web_api.h"

#include <stdlib.h>
#include <string.h>

/* Модуль транспорта: обработчик событий ядра и признак подъёма. */
typedef struct {
    const char *uri;
    uint8_t inited;
    uint8_t opened;
    ws_transport_event_handler_t handler;
    void *context;
} stub_module_t;

/* Одна отложенная отправка: что ядро отдало в begin_send. */
typedef struct {
    const void *data;
    uint32_t len;
} stub_send_t;

static stub_module_t xModule;
static ws_transport_stub_conn_t *pxConns;

struct ws_transport_stub_conn {
    struct ws_transport_stub_conn *next;
    int fd;
    /* Что возвращает begin_send (WS_TRANSPORT_OK по умолчанию). */
    ws_transport_result_t send_result;
    int deferred;
    int send_attempts;
    int delivered;
    int close_calls;
    int reply_calls;
    /* close() позвало ядро (соединения больше нет, но CLOSED ещё не отдан) */
    int down;
    /* Транспорт уже отдал ядру CLOSED */
    int closed;
    /* Последнее отправленное сообщение: тесты проверяют сам конверт ответа. */
    char last_sent[512];
    /* Сборка сообщения из фрагментов — как в настоящем транспорте. */
    uint8_t *msg;
    uint32_t msg_len;
    uint32_t msg_cap;
    /* Отложенные завершения отправок. */
    stub_send_t *pending;
    int pending_count;
    int pending_cap;
};

static void vNotify(ws_transport_stub_conn_t *conn, const ws_transport_event_trigger_t *ev) {
    if(xModule.handler != NULL) xModule.handler(conn, ev);
}

/* --- Операции транспорта как модуля --------------------------------------- */

static ws_transport_result_t stubInit(const void *self, const void *config) {
    stub_module_t *m = (stub_module_t *)self;
    if((m == NULL) || (config == NULL)) return WS_TRANSPORT_BAD_CALL;
    if(m->inited) return WS_TRANSPORT_BUSY;
    m->uri    = (const char *)config;
    m->inited = 1;
    return WS_TRANSPORT_OK;
}

static ws_transport_result_t stubOpen(const void *self, const void *config) {
    (void)config;
    stub_module_t *m = (stub_module_t *)self;
    if((m == NULL) || !m->inited) return WS_TRANSPORT_BAD_CALL;
    m->opened = 1;
    return WS_TRANSPORT_OK;
}

static ws_transport_result_t stubSubscribe(const void *self, ws_transport_event_handler_t handler,
                                           void *context) {
    stub_module_t *m = (stub_module_t *)self;
    if((m == NULL) || (handler == NULL)) return WS_TRANSPORT_BAD_CALL;
    m->handler = handler;
    m->context = context;
    return WS_TRANSPORT_OK;
}

/* --- Операции соединения -------------------------------------------------- */

/* Ядро просит закрыть соединение. Настоящий транспорт отдаёт это httpd, а CLOSED
 * приходит позже — когда httpd освободит сессию; в тестах это делает
 * ws_transport_stub_disconnect(). Здесь же соединение просто становится мёртвым. */
static ws_transport_result_t stubClose(const void *self) {
    ws_transport_stub_conn_t *conn = (ws_transport_stub_conn_t *)self;
    if(conn == NULL) return WS_TRANSPORT_BAD_CALL;
    conn->close_calls++;
    conn->down = 1;
    return WS_TRANSPORT_OK;
}

static ws_transport_result_t stubBreakSending(const void *self) {
    (void)self;
    return WS_TRANSPORT_OK;
}

/* Отправка идёт одним куском: прерывать нечего (как у httpd). */
static ws_transport_result_t stubBeginSend(const void *self, const void *data, uint32_t length) {
    ws_transport_stub_conn_t *conn = (ws_transport_stub_conn_t *)self;
    if((conn == NULL) || (data == NULL) || (length == 0)) return WS_TRANSPORT_BAD_CALL;
    conn->send_attempts++;

    /* Транспорт мог отказаться от отправки (нет памяти, соединение мертво) —
     * тогда колбэка не будет, и буфер освобождает ядро. */
    if(conn->send_result != WS_TRANSPORT_OK) return conn->send_result;
    if(conn->down) return WS_TRANSPORT_BAD_CALL;

    size_t n = length < sizeof(conn->last_sent) - 1 ? length : sizeof(conn->last_sent) - 1;
    memcpy(conn->last_sent, data, n);
    conn->last_sent[n] = '\0';
    conn->delivered++;

    if(conn->deferred) {
        if(conn->pending_count == conn->pending_cap) {
            int cap = conn->pending_cap ? conn->pending_cap * 2 : 8;
            stub_send_t *p = realloc(conn->pending, (size_t)cap * sizeof(*p));
            if(p == NULL) return WS_TRANSPORT_FAULT;
            conn->pending     = p;
            conn->pending_cap = cap;
        }
        conn->pending[conn->pending_count].data = data;
        conn->pending[conn->pending_count].len  = length;
        conn->pending_count++;
        return WS_TRANSPORT_OK;
    }

    ws_transport_event_trigger_t ev = { .event = WS_TRANSPORT_TRANSFER_COMPLETE };
    ev.data.ptr    = data;
    ev.data.length = length;
    ev.data.left   = 0;
    vNotify(conn, &ev);
    return WS_TRANSPORT_OK;
}

static int32_t stubPeerId(const void *self) {
    const ws_transport_stub_conn_t *conn = (const ws_transport_stub_conn_t *)self;
    return conn ? conn->fd : -1;
}

static const ws_transport_iface_t xIface = {
    .init          = stubInit,
    .open          = stubOpen,
    .close         = stubClose,
    .begin_send    = stubBeginSend,
    .break_sending = stubBreakSending,
    .subscribe     = stubSubscribe,
    .peer_id       = stubPeerId,
};

const ws_transport_iface_t *ws_transport_stub_iface(void) {
    return &xIface;
}

void *ws_transport_stub_module(void) {
    return &xModule;
}

/* --- Приём ---------------------------------------------------------------- */

static void vStubDrop(ws_transport_stub_conn_t *conn) {
    free(conn->msg);
    conn->msg     = NULL;
    conn->msg_len = 0;
    conn->msg_cap = 0;
}

static void vStubDeliver(ws_transport_stub_conn_t *conn) {
    if(conn->msg_len == 0) {   /* пустое сообщение ядру не нужно */
        vStubDrop(conn);
        return;
    }
    ws_transport_event_trigger_t ev = { .event = WS_TRANSPORT_RECEIVED };
    ev.data.ptr    = conn->msg;
    ev.data.length = conn->msg_len;
    ev.data.left   = 0;        /* фрагменты уже склеены — это сообщение целиком */
    vNotify(conn, &ev);
    /* Буфер наш: ядро читает его только во время обработчика события. */
    vStubDrop(conn);
}

/* Место под ещё len байт сообщения (плюс терминатор). NULL — предел или память. */
static uint8_t *pucStubReserve(ws_transport_stub_conn_t *conn, uint32_t len) {
    if(conn->msg_len + len > WS_TRANSPORT_STUB_MESSAGE_MAX_LEN) return NULL;
    if(conn->msg_cap < conn->msg_len + len + 1) {
        uint32_t cap = conn->msg_len + len + 1;
        uint8_t *buf = realloc(conn->msg, cap);
        if(buf == NULL) return NULL;
        conn->msg     = buf;
        conn->msg_cap = cap;
    }
    return conn->msg + conn->msg_len;
}

static void vStubAppend(ws_transport_stub_conn_t *conn, const void *data, uint32_t len, int final) {
    uint8_t *dst = pucStubReserve(conn, len);
    if(dst == NULL) {
        /* Сообщение вышло за предел — принимать его негде: закрываем соединение. */
        vStubDrop(conn);
        if(!conn->closed) {
            conn->closed = 1;
            conn->down    = 1;
            conn->close_calls++;
            ws_transport_event_trigger_t ev = { .event = WS_TRANSPORT_CLOSED };
            vNotify(conn, &ev);
        }
        return;
    }
    if(len) memcpy(dst, data, len);
    conn->msg_len += len;
    if(final) vStubDeliver(conn);
}

void ws_transport_stub_frame(ws_transport_stub_conn_t *conn, ws_transport_stub_frame_t type,
                             const void *data, size_t len, int final) {
    if((conn == NULL) || conn->down) return;

    switch(type) {
    case WS_STUB_FRAME_PING:
        /* Управляющие кадры обрабатывает транспорт: ядру они не отдаются. */
        conn->reply_calls++;
        return;
    case WS_STUB_FRAME_CLOSE:
        conn->reply_calls++;
        return;
    case WS_STUB_FRAME_PONG:
        return;
    default:
        break;
    }

    /* Кадр данных больше предела: не читаем и не разбираем, закрываем сессию. */
    if(len > WS_TRANSPORT_STUB_FRAME_MAX_LEN) {
        vStubDrop(conn);
        if(!conn->closed) {
            conn->closed = 1;
            conn->down    = 1;
            conn->close_calls++;
            ws_transport_event_trigger_t ev = { .event = WS_TRANSPORT_CLOSED };
            vNotify(conn, &ev);
        }
        return;
    }

    if(type == WS_STUB_FRAME_CONT) {
        if(conn->msg == NULL) return;   /* продолжение без начала — отбрасываем */
        vStubAppend(conn, data, (uint32_t)len, final);
        return;
    }

    /* Новое сообщение: недособранное предыдущее выбрасываем, оно уже не станет
     * целым, а склеивать его с новым нельзя. */
    vStubDrop(conn);
    vStubAppend(conn, data, (uint32_t)len, final);
}

/* --- Управление соединениями --------------------------------------------- */

ws_transport_stub_conn_t *ws_transport_stub_connect_silent(int fd) {
    ws_transport_stub_conn_t *conn = calloc(1, sizeof(*conn));
    if(conn == NULL) return NULL;
    conn->fd = fd;
    /* По умолчанию транспорт отправку принимает: WS_TRANSPORT_INVALID — это 0,
     * то есть «по умолчанию» отказ, если поле не заполнить. */
    conn->send_result = WS_TRANSPORT_OK;
    conn->next = pxConns;
    pxConns = conn;
    return conn;
}

ws_transport_stub_conn_t *ws_transport_stub_connect(int fd) {
    ws_transport_stub_conn_t *conn = ws_transport_stub_connect_silent(fd);
    if(conn == NULL) return NULL;

    ws_transport_event_trigger_t ev = { .event = WS_TRANSPORT_ESTABLISHED };
    if((xModule.handler == NULL) || (xModule.handler(conn, &ev) != WS_TRANSPORT_OK)) {
        /* Ядру соединение обслуживать нечем — как и настоящий транспорт,
         * закрываем его. */
        conn->closed = 1;
        conn->down    = 1;
        conn->close_calls++;
    }
    return conn;
}

/* Транспорт закрыл сессию (у httpd — free_ctx): соединения больше нет. */
void ws_transport_stub_disconnect(ws_transport_stub_conn_t *conn) {
    if((conn == NULL) || conn->closed) return;
    conn->closed = 1;
    conn->down   = 1;
    ws_transport_event_trigger_t ev = { .event = WS_TRANSPORT_CLOSED };
    vNotify(conn, &ev);
}

/* --- Наблюдение ----------------------------------------------------------- */

void ws_transport_stub_set_send_result(ws_transport_stub_conn_t *conn,
                                       ws_transport_result_t res) {
    if(conn) conn->send_result = res;
}

void ws_transport_stub_emit_closed(ws_transport_stub_conn_t *conn) {
    if(conn == NULL) return;
    conn->closed = 1;
    conn->down   = 1;
    ws_transport_event_trigger_t ev = { .event = WS_TRANSPORT_CLOSED };
    vNotify(conn, &ev);
}

int ws_transport_stub_send_attempts(ws_transport_stub_conn_t *conn) {
    return conn ? conn->send_attempts : 0;
}

const char *ws_transport_stub_last_sent(ws_transport_stub_conn_t *conn) {
    return conn ? conn->last_sent : "";
}

int ws_transport_stub_delivered(ws_transport_stub_conn_t *conn) {
    return conn ? conn->delivered : 0;
}

int ws_transport_stub_close_calls(ws_transport_stub_conn_t *conn) {
    return conn ? conn->close_calls : 0;
}

int ws_transport_stub_reply_calls(ws_transport_stub_conn_t *conn) {
    return conn ? conn->reply_calls : 0;
}

int ws_transport_stub_closed(ws_transport_stub_conn_t *conn) {
    return conn ? conn->closed : 0;
}

void ws_transport_stub_set_deferred(ws_transport_stub_conn_t *conn, int on) {
    if(conn) conn->deferred = on;
}

int ws_transport_stub_pending_count(ws_transport_stub_conn_t *conn) {
    return conn ? conn->pending_count : 0;
}

void ws_transport_stub_complete_pending(ws_transport_stub_conn_t *conn, ws_transport_result_t res) {
    if(conn == NULL) return;
    /* Обработчик может поставить новые отправки: завершаем только те, что уже
     * накопились. */
    int n = conn->pending_count;
    stub_send_t *batch = conn->pending;
    conn->pending       = NULL;
    conn->pending_count = conn->pending_cap = 0;

    for(int i = 0; i < n; i++) {
        ws_transport_event_trigger_t ev = {
            .event = (res == WS_TRANSPORT_OK) ? WS_TRANSPORT_TRANSFER_COMPLETE : WS_TRANSPORT_TRANSFER_ERR,
        };
        ev.data.ptr    = batch[i].data;
        ev.data.length = batch[i].len;
        ev.data.left   = 0;
        vNotify(conn, &ev);
    }
    free(batch);
}

void ws_transport_stub_reset(void) {
    /* Отложенные отправки: сообщаем ядру об ошибке, чтобы оно освободило свои
     * буферы ответов (иначе их нашёл бы LeakSanitizer). */
    ws_transport_stub_conn_t *conn = pxConns;
    while(conn != NULL) {
        ws_transport_stub_conn_t *next = conn->next;
        if(conn->pending_count) ws_transport_stub_complete_pending(conn, WS_TRANSPORT_FAULT);
        free(conn->msg);
        free(conn->pending);
        free(conn);
        conn = next;
    }
    pxConns = NULL;
    memset(&xModule, 0, sizeof(xModule));
}

ws_transport_result_t ws_transport_stub_attach(void) {
    return ws_server_transport_attach(&xIface, &xModule);
}
