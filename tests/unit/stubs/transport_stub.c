#include "transport_stub.h"
#include "api.h"
#include "system_stub.h"
#include "net_iface.h"

#include <stdlib.h>
#include <string.h>

/* Модуль транспорта — контекст экземпляра (как в main/ws_transport.c):
 * экземпляр, порождённый веб-сервером одного интерфейса. События экземпляра —
 * свой event_t, подписчик — делегат своего ядра. Интерфейс — знание самого
 * экземпляра (у настоящего транспорта его знает породивший веб-сервер), в
 * общем объекте transport_t его нет.
 *
 * Поле on_event стоит ПЕРВЫМ: внутри event_t — элемент связного списка
 * (указатели), и ему нужен адрес под указатель; у структуры есть и другие
 * указатели, поэтому сам экземпляр выровнен как надо. */
typedef struct {
    event_t on_event;
    uint32_t iface;
    const char *uri;
    uint8_t inited;
    uint8_t opened;
} stub_module_t;

/* Два экземпляра — по одному на веб-сервер интерфейса: точка доступа и станция. */
static stub_module_t modules[] = {
    { .iface = GW_IFACE_AP  },
    { .iface = GW_IFACE_STA },
};
#define STUB_MODULE_COUNT (sizeof(modules) / sizeof(modules[0]))

static stub_module_t *module_for_iface(uint32_t iface) {
    for(size_t i = 0; i < STUB_MODULE_COUNT; i++)
        if(modules[i].iface == iface) return &modules[i];
    return NULL;
}

/* Одна отложенная отправка: что ядро отдало в begin_send. */
typedef struct {
    const void *data;
    uint32_t len;
} stub_send_t;

static ws_transport_stub_conn_t *conns;
/* Интерфейс, который стаб выдаёт следующим соединениям (как настоящий
 * транспорт — по локальному адресу сокета). По умолчанию точка доступа: тестам
 * без политики нужен конкретный интерфейс, а не «неизвестно». */
static uint32_t next_iface = GW_IFACE_AP;

struct ws_transport_stub_conn {
    struct ws_transport_stub_conn *next;
    int fd;
    /* Через какой интерфейс «пришло» соединение и чей оно экземпляр: выбрано
     * при установлении, как у настоящего транспорта. Ядро об этом не
     * спрашивает — оно подписано на этот экземпляр. */
    uint32_t iface;
    stub_module_t *module;
    /* Что возвращает begin_send (TRANSPORT_OK по умолчанию). */
    transport_result_t send_result;
    int deferred;
    int send_attempts;
    int delivered;
    int close_calls;
    int reply_calls;
    /* close() позвало ядро (соединения больше нет, но CLOSED ещё не отдан) */
    int down;
    /* Транспорт уже отдал ядру CLOSED */
    int closed;
    /* Последнее отправленное сообщение: тесты проверяют сам конверт ответа.
     * Размер — под самый длинный ответ сервиса: кадр приёма UART собирается из
     * накопителя (до 4 КБ данных, то есть ~5.5 КБ base64) плюс конверт, поэтому
     * 512 байт обрезали бы его и тест «сколько пришло» проверял бы обрезок. */
    char last_sent[8192];
    /* Сборка сообщения из фрагментов — как в настоящем транспорте. */
    uint8_t *msg;
    uint32_t msg_len;
    uint32_t msg_cap;
    /* Отложенные завершения отправок. */
    stub_send_t *pending;
    int pending_count;
    int pending_cap;
};

/* Событие, поднятое, пока подписчик занят другим событием: отдаётся, когда
 * внешний подъём закончился (см. notify_event). */
typedef struct {
    ws_transport_stub_conn_t *conn;
    transport_event_trigger_t ev;
} stub_deferred_event_t;

#define STUB_NOTIFY_DEPTH 8
static stub_deferred_event_t notify_queue[STUB_NOTIFY_DEPTH];
static int notify_queued;
/* Глубина подъёма: 0 — подписчик ничего не обрабатывает. */
static int notify_depth;

static uint32_t raise_event(ws_transport_stub_conn_t *conn, const transport_event_trigger_t *ev) {
    stub_module_t *m = (conn != NULL) ? conn->module : NULL;
    if(m == NULL) return 0;
    return event_raise(&m->on_event, conn, (void *)ev);
}

/* Отдать накопленное, сохранив порядок: ядро ждёт завершения отправок в том
 * порядке, в каком транспорт их принял. */
static void notify_flush(void) {
    while(notify_queued > 0) {
        stub_deferred_event_t e = notify_queue[0];
        notify_queued--;
        memmove(&notify_queue[0], &notify_queue[1],
                (size_t)notify_queued * sizeof(notify_queue[0]));
        notify_depth++;
        raise_event(e.conn, &e.ev);
        notify_depth--;
        /* Подъём мог накопить ещё события — цикл отдаст и их. */
    }
}

static uint32_t notify_event(ws_transport_stub_conn_t *conn, const transport_event_trigger_t *ev) {
    /* Событие уходит подписчику ЭКЗЕМПЛЯРА, принявшего соединение: обработчик и
     * контекст лежат в делегате, который ядро отдало в subscribe(). Возврат —
     * сколько подписчиков событие получило (ноль — обслуживать некому;
     * отложенное событие получит его позже). */
    if(notify_depth > 0) {
        /* Подъём события ВНУТРИ обработки другого события. event_raise помечает
         * уже обойдённых делегатов, и вложенный подъём по тому же event_t их
         * пропустил бы — событие потерялось бы целиком (а вместе с ним ядро не
         * освободило бы буфер ответа). Настоящий транспорт в такое положение не
         * попадает: завершение отправки приходит из задачи httpd, то есть позже
         * вызова begin_send. Стаб отправляет сразу, поэтому такие события
         * копятся и отдаются, когда внешний подъём закончился. */
        if(notify_queued < STUB_NOTIFY_DEPTH) {
            notify_queue[notify_queued].conn = conn;
            notify_queue[notify_queued].ev   = *ev;
            notify_queued++;
            return 0;
        }
        return raise_event(conn, ev);   /* очередь полна — отдаём сразу */
    }

    notify_depth++;
    uint32_t n = raise_event(conn, ev);
    notify_depth--;
    notify_flush();
    return n;
}

/* --- Операции транспорта как модуля --------------------------------------- */

static transport_result_t stub_init(const void *self, const void *config) {
    stub_module_t *m = (stub_module_t *)self;
    if((m == NULL) || (config == NULL)) return TRANSPORT_BAD_CALL;
    if(m->inited) return TRANSPORT_BUSY;
    m->uri    = (const char *)config;
    m->inited = 1;
    return TRANSPORT_OK;
}

static transport_result_t stub_open(const void *self, const void *config) {
    (void)config;
    stub_module_t *m = (stub_module_t *)self;
    if((m == NULL) || !m->inited) return TRANSPORT_BAD_CALL;
    m->opened = 1;
    return TRANSPORT_OK;
}

static transport_result_t stub_subscribe(const void *self, delegate_t *delegate) {
    stub_module_t *m = (stub_module_t *)self;
    if((m == NULL) || (delegate == NULL) || (delegate->handler == NULL))
        return TRANSPORT_BAD_CALL;
    /* Как настоящий транспорт (main/ws_transport.c): подписчик кладётся в event_t
     * экземпляра, обработчик и контекст — в самом делегате. Отдельного
     * unsubscribe у стаба тоже нет: отписывается делегат. */
    event_subscribe(&m->on_event, delegate);
    return TRANSPORT_OK;
}

/* --- Операции соединения -------------------------------------------------- */

/* Ядро просит закрыть соединение. Настоящий транспорт отдаёт это httpd, а CLOSED
 * приходит позже — когда httpd освободит сессию; в тестах это делает
 * ws_transport_stub_disconnect(). Здесь же соединение просто становится мёртвым. */
static transport_result_t stub_close(const void *self) {
    ws_transport_stub_conn_t *conn = (ws_transport_stub_conn_t *)self;
    if(conn == NULL) return TRANSPORT_BAD_CALL;
    conn->close_calls++;
    conn->down = 1;
    return TRANSPORT_OK;
}

static transport_result_t stub_break_sending(const void *self) {
    (void)self;
    return TRANSPORT_OK;
}

/* Отправка идёт одним куском: прерывать нечего (как у httpd). */
static transport_result_t stub_begin_send(const void *self, const void *data, uint32_t length) {
    ws_transport_stub_conn_t *conn = (ws_transport_stub_conn_t *)self;
    if((conn == NULL) || (data == NULL) || (length == 0)) return TRANSPORT_BAD_CALL;
    conn->send_attempts++;

    /* Транспорт мог отказаться от отправки (нет памяти, соединение мертво) —
     * тогда колбэка не будет, и буфер освобождает ядро. */
    if(conn->send_result != TRANSPORT_OK) return conn->send_result;
    if(conn->down) return TRANSPORT_BAD_CALL;

    size_t n = length < sizeof(conn->last_sent) - 1 ? length : sizeof(conn->last_sent) - 1;
    memcpy(conn->last_sent, data, n);
    conn->last_sent[n] = '\0';
    conn->delivered++;

    if(conn->deferred) {
        if(conn->pending_count == conn->pending_cap) {
            int cap = conn->pending_cap ? conn->pending_cap * 2 : 8;
            stub_send_t *p = realloc(conn->pending, (size_t)cap * sizeof(*p));
            if(p == NULL) return TRANSPORT_FAULT;
            conn->pending     = p;
            conn->pending_cap = cap;
        }
        conn->pending[conn->pending_count].data = data;
        conn->pending[conn->pending_count].len  = length;
        conn->pending_count++;
        return TRANSPORT_OK;
    }

    transport_event_trigger_t ev = { .event = TRANSPORT_TRANSFER_COMPLETE };
    ev.data.ptr    = data;
    ev.data.length = length;
    ev.data.left   = 0;
    notify_event(conn, &ev);
    return TRANSPORT_OK;
}

static int32_t stub_peer_id(const void *self) {
    const ws_transport_stub_conn_t *conn = (const ws_transport_stub_conn_t *)self;
    return conn ? conn->fd : -1;
}

static const transport_iface_t iface_table = {
    .init          = stub_init,
    .open          = stub_open,
    .close         = stub_close,
    .begin_send    = stub_begin_send,
    .break_sending = stub_break_sending,
    .subscribe     = stub_subscribe,
    .peer_id       = stub_peer_id,
};

/* Объекты транспорта — то, что получает api_init. Их ровно столько, сколько
 * экземпляров: методы общие, контекст реализации свой. Номера интерфейса в
 * объекте нет — экземпляр отличается от другого только контекстом. */
static transport_t transports[] = {
    { .iface = &iface_table, .context = &modules[0] },
    { .iface = &iface_table, .context = &modules[1] },
};
#define STUB_TRANSPORT_COUNT (sizeof(transports) / sizeof(transports[0]))

const transport_iface_t *ws_transport_stub_iface(void) {
    return &iface_table;
}

void *ws_transport_stub_module(uint32_t iface) {
    return module_for_iface(iface);
}

const transport_t *ws_transport_stub_object(uint32_t iface) {
    /* Экземпляр выбирается по интерфейсу, который помнит ЕГО контекст (как
     * настоящий: интерфейс задан породившим его веб-сервером), а не по полю
     * объекта: такого поля у transport_t больше нет. */
    const stub_module_t *module = module_for_iface(iface);
    if(module == NULL) return NULL;
    for(size_t i = 0; i < STUB_TRANSPORT_COUNT; i++)
        if(transports[i].context == module) return &transports[i];
    return NULL;
}

/* --- Приём ---------------------------------------------------------------- */

static void stub_drop(ws_transport_stub_conn_t *conn) {
    free(conn->msg);
    conn->msg     = NULL;
    conn->msg_len = 0;
    conn->msg_cap = 0;
}

static void stub_deliver(ws_transport_stub_conn_t *conn) {
    if(conn->msg_len == 0) {   /* пустое сообщение ядру не нужно */
        stub_drop(conn);
        return;
    }
    transport_event_trigger_t ev = { .event = TRANSPORT_RECEIVED };
    ev.data.ptr    = conn->msg;
    ev.data.length = conn->msg_len;
    ev.data.left   = 0;        /* фрагменты уже склеены — это сообщение целиком */
    notify_event(conn, &ev);
    /* Буфер наш: ядро читает его только во время обработчика события. */
    stub_drop(conn);
}

/* Место под ещё len байт сообщения (плюс терминатор). NULL — предел или память. */
static uint8_t *stub_reserve(ws_transport_stub_conn_t *conn, uint32_t len) {
    if(conn->msg_len + len > TRANSPORT_STUB_MESSAGE_MAX_LEN) return NULL;
    if(conn->msg_cap < conn->msg_len + len + 1) {
        uint32_t cap = conn->msg_len + len + 1;
        uint8_t *buf = realloc(conn->msg, cap);
        if(buf == NULL) return NULL;
        conn->msg     = buf;
        conn->msg_cap = cap;
    }
    return conn->msg + conn->msg_len;
}

static void stub_append(ws_transport_stub_conn_t *conn, const void *data, uint32_t len, int final) {
    uint8_t *dst = stub_reserve(conn, len);
    if(dst == NULL) {
        /* Сообщение вышло за предел — принимать его негде: закрываем соединение. */
        stub_drop(conn);
        if(!conn->closed) {
            conn->closed = 1;
            conn->down    = 1;
            conn->close_calls++;
            transport_event_trigger_t ev = { .event = TRANSPORT_CLOSED };
            notify_event(conn, &ev);
        }
        return;
    }
    if(len) memcpy(dst, data, len);
    conn->msg_len += len;
    if(final) stub_deliver(conn);
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
    if(len > TRANSPORT_STUB_FRAME_MAX_LEN) {
        stub_drop(conn);
        if(!conn->closed) {
            conn->closed = 1;
            conn->down    = 1;
            conn->close_calls++;
            transport_event_trigger_t ev = { .event = TRANSPORT_CLOSED };
            notify_event(conn, &ev);
        }
        return;
    }

    if(type == WS_STUB_FRAME_CONT) {
        if(conn->msg == NULL) return;   /* продолжение без начала — отбрасываем */
        stub_append(conn, data, (uint32_t)len, final);
        return;
    }

    /* Новое сообщение: недособранное предыдущее выбрасываем, оно уже не станет
     * целым, а склеивать его с новым нельзя. */
    stub_drop(conn);
    stub_append(conn, data, (uint32_t)len, final);
}

/* --- Управление соединениями --------------------------------------------- */

ws_transport_stub_conn_t *ws_transport_stub_connect_silent(int fd) {
    ws_transport_stub_conn_t *conn = calloc(1, sizeof(*conn));
    if(conn == NULL) return NULL;
    conn->fd = fd;
    /* Интерфейс фиксируем здесь, как настоящий транспорт при установлении
     * соединения: смена значения потом на уже принятую сессию не влияет. */
    conn->iface = next_iface;
    /* Экземпляр выбирается по интерфейсу — как в прошивке. Интерфейса, которого
     * у заглушки нет, не бывает: неподнятый экземпляр всё равно выбран, и ядро
     * его просто не обслуживает (см. connect). */
    conn->module = module_for_iface(conn->iface);
    /* По умолчанию транспорт отправку принимает: TRANSPORT_INVALID — это 0,
     * то есть «по умолчанию» отказ, если поле не заполнить. */
    conn->send_result = TRANSPORT_OK;
    conn->next = conns;
    conns = conn;
    return conn;
}

ws_transport_stub_conn_t *ws_transport_stub_connect(int fd) {
    ws_transport_stub_conn_t *conn = ws_transport_stub_connect_silent(fd);
    if(conn == NULL) return NULL;

    transport_event_trigger_t ev = { .event = TRANSPORT_ESTABLISHED };
    if(notify_event(conn, &ev) == 0) {
        /* Ядру соединение обслуживать нечем (на этот интерфейс никто не
         * подписан) — как и настоящий транспорт, закрываем его. */
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
    transport_event_trigger_t ev = { .event = TRANSPORT_CLOSED };
    notify_event(conn, &ev);
}

/* --- Наблюдение ----------------------------------------------------------- */

void ws_transport_stub_set_send_result(ws_transport_stub_conn_t *conn,
                                       transport_result_t res) {
    if(conn) conn->send_result = res;
}

void ws_transport_stub_emit_closed(ws_transport_stub_conn_t *conn) {
    if(conn == NULL) return;
    conn->closed = 1;
    conn->down   = 1;
    transport_event_trigger_t ev = { .event = TRANSPORT_CLOSED };
    notify_event(conn, &ev);
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

/* Интерфейс, который получат СЛЕДУЮЩИЕ соединения (как настоящий транспорт
 * определяет его при установлении). Вызывать до ws_transport_stub_connect(). */
void ws_transport_stub_set_iface(uint32_t iface) {
    next_iface = iface;
}

uint32_t ws_transport_stub_conn_iface(ws_transport_stub_conn_t *conn) {
    return conn ? conn->iface : GW_IFACE_NONE;
}

void ws_transport_stub_set_deferred(ws_transport_stub_conn_t *conn, int on) {
    if(conn) conn->deferred = on;
}

int ws_transport_stub_pending_count(ws_transport_stub_conn_t *conn) {
    return conn ? conn->pending_count : 0;
}

const void *ws_transport_stub_pending_data(ws_transport_stub_conn_t *conn, int idx) {
    if((conn == NULL) || (idx < 0) || (idx >= conn->pending_count)) return NULL;
    return conn->pending[idx].data;
}

uint32_t ws_transport_stub_pending_len(ws_transport_stub_conn_t *conn, int idx) {
    if((conn == NULL) || (idx < 0) || (idx >= conn->pending_count)) return 0;
    return conn->pending[idx].len;
}

void ws_transport_stub_complete_pending(ws_transport_stub_conn_t *conn, transport_result_t res) {
    if(conn == NULL) return;
    /* Обработчик может поставить новые отправки: завершаем только те, что уже
     * накопились. */
    int n = conn->pending_count;
    stub_send_t *batch = conn->pending;
    conn->pending       = NULL;
    conn->pending_count = conn->pending_cap = 0;

    for(int i = 0; i < n; i++) {
        transport_event_trigger_t ev = {
            .event = (res == TRANSPORT_OK) ? TRANSPORT_TRANSFER_COMPLETE : TRANSPORT_TRANSFER_ERR,
        };
        ev.data.ptr    = batch[i].data;
        ev.data.length = batch[i].len;
        ev.data.left   = 0;
        notify_event(conn, &ev);
    }
    free(batch);
}

void ws_transport_stub_reset(void) {
    /* Отложенные отправки: сообщаем ядру об ошибке, чтобы оно освободило свои
     * буферы ответов (иначе их нашёл бы LeakSanitizer). */
    ws_transport_stub_conn_t *conn = conns;
    while(conn != NULL) {
        ws_transport_stub_conn_t *next = conn->next;
        if(conn->pending_count) ws_transport_stub_complete_pending(conn, TRANSPORT_FAULT);
        free(conn->msg);
        free(conn->pending);
        free(conn);
        conn = next;
    }
    conns = NULL;
    next_iface = GW_IFACE_AP;
    /* Отложенные события принадлежат прошлому тесту: их получатели (соединения и
     * ядро) уже не существуют. */
    notify_queued = 0;
    notify_depth  = 0;
    /* Подписку снимаем у всех экземпляров: следующему тесту нужно чистое
     * состояние, а не ядро предыдущего. Снимаем через event_clear, а не
     * event_unsubscribe: делегат нам неизвестен (он лежит в дескрипторе ядра, и
     * к моменту сброса ядро прошлого теста уже может быть освобождено), а
     * event_clear снимает из списка КАЖДОГО подписчика и помечает их
     * отписанными — повторная отписка в api_deinit после этого безопасна. */
    for(size_t i = 0; i < STUB_MODULE_COUNT; i++) {
        event_clear(&modules[i].on_event);
        modules[i].inited = 0;
        modules[i].opened = 0;
        modules[i].uri = NULL;
    }
}

/* Ядро поднимается над объектом транспорта с системной таблицей стаба
 * (см. system_stub_api_system): своего потока и своей очереди у ядра нет, а всё,
 * что ему нужно от системы (мьютекс, время, журнал), оно берёт оттуда. */
static api_t *stub_core(uint32_t which) {
    return api_init(ws_transport_stub_object(which), system_stub_api_system());
}

api_t *ws_transport_stub_init(void) {
    return stub_core(GW_IFACE_AP);
}

api_t *ws_transport_stub_init_only(uint32_t iface) {
    return stub_core(iface);
}
