/* ws_transport.c — транспорт на WebSocket как надстройка над веб-сервером:
 * соединения и кадры приносит объект веб-сервера интерфейса, транспорт собирает
 * сообщения и поднимает события своему подписчику (ядру API). */

#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "CodeLib.h"
#include "connection.h"
/* api.h транспорту не нужен: он знает только свой объект и подписчика. */
#include "transport.h"
#include "ws_transport.h"
#include "platform.h"

#define GW_LOG_MODULE GW_LOG_WS
#include "gw_log.h"

static const char *TAG = "ws_transport";

/* С ESP-IDF v6 esp_http_server отвечает на рукопожатие сам и не зовёт
 * uri->handler — дескриптор соединения заводится из ws_post_handshake_cb. До v6
 * обработчик зовётся с method == HTTP_GET, и дескриптор заводится там.
 * Рукопожатие приходит через объект веб-сервера, поэтому req->user_ctx здесь тот
 * же контекст экземпляра, что и у кадров. */
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
#define WS_CONN_FROM_POST_HANDSHAKE_CB 1
#else
#define WS_CONN_FROM_POST_HANDSHAKE_CB 0
#endif

#if WS_CONN_FROM_POST_HANDSHAKE_CB && !CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT
#error "ESP-IDF >= 6.0 never calls the URI handler for the WebSocket handshake: enable CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT"
#endif

/* Пределы кадра и собранного сообщения — 16 КБ, одиночный кадр и склейка
 * ограничены одинаково. При превышении закрываем соединение (RFC 6455 §7.4.1,
 * код 1009 — сообщение слишком велико). */
#define TRANSPORT_FRAME_MAX_LEN   16384
#define TRANSPORT_MESSAGE_MAX_LEN 16384

/* Управляющий кадр WebSocket: payload не длиннее 125 байт (RFC 6455 §5.5). */
#define WS_CONTROL_PAYLOAD_MAX_LEN   125

/* Экземпляр транспорта. Объявлен заранее — дескриптор соединения знает, чей он. */
typedef struct ws_transport_module_s ws_transport_module_t;

/* Сколько соединений экземпляр держит в списке — из конфигурации проекта.
 * Больше, чем слотов у WS-порта, смысла нет: сокеты выдаёт он. */
#define WS_CONN_MAX   CONFIG_WS_TRANSPORT_MAX_CONNS
_Static_assert(CONFIG_WS_TRANSPORT_MAX_CONNS <= CONFIG_WEB_SOCKET_MAX_CLIENTS,
               "соединений транспорта больше, чем слотов WS-порта");


/* Дескриптор соединения: ядру непрозрачен — это ключ его сессии. */
typedef struct {
    int fd;
    delegate_t delegate;
    /* Чей это экземпляр: веб-сервер передал его контекстом обработчика. По нему
     * находятся подписчик и веб-сервер, которому принадлежит сессия. */
    ws_transport_module_t *module;
    /* Занят ли слот пула (см. conn_alloc/conn_release) и его поколение: снимок,
     * сделанный тактом до переиспользования слота, узнаётся по номеру и
     * отбрасывается. */
    uint8_t  busy;
    uint32_t gen;
    /* Сборка сообщения из фрагментов (первый кадр с FIN=0, дальше продолжения).
     * Буфер живёт до возврата из обработчика события RECEIVED. */
    uint8_t *msg;
    uint32_t msg_len;
    uint32_t msg_cap;
    /* Сколько отправок в работе: дескриптор нельзя освободить, пока хоть одна не
     * завершилась — колбэк приходит позже закрытия сессии. */
    uint32_t sends_in_flight;
    /* Сессия закрыта, но отправки ещё в работе: освободит последний колбэк. */
    uint8_t  detached;
    /* Колбэк сейчас уведомляет ядро, а ядро вправе закрыть соединение прямо
     * отсюда: тогда обработчик закрытия дескриптор не освобождает. */
    uint8_t  in_callback;
    /* Сколько сообщений этого соединения ждёт доставки: пока счётчик не ноль,
     * дескриптор не освобождается (на него ссылаются записи очереди). */
    uint16_t recv_queued;
    /* Начатый кадр, который клиент ещё не принял целиком: своя копия заголовка и
     * данных. send с MSG_DONTWAIT пишет сколько влезло, а кадр нельзя ни бросить
     * на середине, ни смешать со следующим — остаток досылает такт (poll). */
    uint8_t *out;
    uint32_t out_len;
    uint32_t out_off;
    uint32_t out_since;      /* когда кадр начат — для решения «клиент пропал» */
    uint8_t  out_pending;    /* в буфере есть недописанное */
    uint16_t out_dropped;    /* сколько кадров отброшено за время занятости */
} ws_transport_conn_t;

/* Предел отложенного кадра на соединение: кадр потока — до ~2,7 КБ base64,
 * ответ вызова — сотни байт, 8 КБ хватает. Больше — отбрасываем. */
#define WS_OUT_MAX   8192u
/* Сколько кадр может висеть без прогресса, прежде чем клиент считается пропавшим. */
#define WS_OUT_DEAD_MS   20000u

/* Принятое сообщение, ждущее доставки ядру. Копия, а не указатель на буфер
 * соединения: буфер переиспользуется следующим сообщением. */
typedef struct ws_transport_recv_s {
    struct ws_transport_recv_s *next;
    ws_transport_conn_t *conn;   /* дескриптор из пула; слот мог быть переиспользован */
    uint32_t gen;                /* поколение дескриптора на момент приёма */
    uint32_t len;
    uint8_t  data[];             /* само сообщение + '\0' в конце */
} ws_transport_recv_t;

/* ── Пул дескрипторов соединений ──────────────────────────────────────────────
 *
 * Дескрипторы НЕ освобождаются: такт ядра (ws_httpd_poll → ws_out_pump) работает
 * по снимку указателей, сделанному до того, как задача веб-сервера закроет
 * сессию, а освобождённый дескриптор в этом снимке — LoadProhibited (поймано
 * штормом переподключений: несколько паник за минуту). Поэтому дескриптор живёт
 * в пуле, а «освобождение» — это возврат слота: буферы отдаются, fd становится -1,
 * busy снимается, номер поколения растёт. Записи очереди приёма запоминают
 * поколение и доставляются только своему дескриптору.
 *
 * Пул на два экземпляра (AP и STA): столько же соединений, сколько у них всего. */
static ws_transport_conn_t ws_conn_pool[WS_CONN_MAX * 2];
static platform_mutex_t    ws_pool_lock;
static uint32_t            ws_conn_gen;

static ws_transport_conn_t *conn_alloc(void) {
    ws_transport_conn_t *conn = NULL;
    if (ws_pool_lock == NULL) return NULL;
    platform_mutex_take(ws_pool_lock, PLATFORM_WAIT_FOREVER);
    for (size_t i = 0; i < sizeof(ws_conn_pool) / sizeof(ws_conn_pool[0]); i++) {
        if (ws_conn_pool[i].busy) continue;
        conn = &ws_conn_pool[i];
        memset(conn, 0, sizeof(*conn));
        conn->busy = 1;
        conn->gen  = ++ws_conn_gen;
        conn->fd   = -1;
        break;
    }
    platform_mutex_give(ws_pool_lock);
    return conn;
}

/* Вернуть слот в пул. Зовётся, когда на дескриптор уже никто не ссылается:
 * отправок в работе нет, очередь приёма пуста. */
static void conn_release(ws_transport_conn_t *conn) {
    if ((conn == NULL) || !conn->busy) return;
    free(conn->out);
    conn->out = NULL;
    free(conn->msg);
    conn->msg = NULL;
    conn->fd  = -1;
    conn->busy = 0;
    conn->detached = 0;
    conn->in_callback = 0;
    conn->sends_in_flight = 0;
    conn->recv_queued = 0;
}

/* ── Экземпляр транспорта ───────────────────────────────────────────────────
 *
 * Экземпляр (в контракте — transport_t.context) — это веб-сервер, который его
 * породил, свой URI, подписка и свои соединения. Номера интерфейса здесь нет: он
 * задан объектом веб-сервера, а «чей это клиент» видно по контексту экземпляра,
 * с которым пришёл запрос, — ни по сокету, ни по таблице не выясняется.
 *
 * Поле on_event стоит ПЕРВЫМ: внутри event_t — элемент связного списка, ему нужен
 * адрес под указатель, а экземпляр выровнен calloc'ом достаточно для любого поля.
 */
struct ws_transport_module_s {
    /* Подписка экземпляра: события event_raise уходят подписчику, который дал
     * делегат в subscribe(). Список подписчиков лежит в самом event_t, поэтому
     * «подписан ли кто-нибудь» видно по результату event_raise. */
    event_t   on_event;
    /* Веб-сервер своего интерфейса и путь WS-точки (дал web_server_open_websocket):
     * через сервер экземпляр активирует путь, закрывает соединения и шлёт кадры. */
    web_server_t *server;
    char         *uri;
    uint8_t   inited;
    uint8_t   opened;
    /* Отправку httpd ставит в очередь датаграммой по UDP: под нагрузкой она
     * теряется, и колбэк не приходит никогда — буферы остаются занятыми. */
    /* Соединения экземпляра — по ним такт транспорта досылает начатые кадры.
     * Список фиксированный: соединений не больше, чем сессий у веб-сервера. */
    platform_mutex_t conn_lock;
    ws_transport_conn_t *conns[WS_CONN_MAX];
    /* Принятые, но ещё не разобранные сообщения: их доставляет poll в задаче
     * приложения, задача веб-сервера только копирует сюда — разбор ей не по стеку. */
    platform_mutex_t recv_lock;
    ws_transport_recv_t *recv_head;
    ws_transport_recv_t *recv_tail;
    uint32_t recv_bytes;
};

/* Настройки экземпляра — то, что передаёт web_server_open_websocket. */
typedef struct {
    const char   *uri;       /* путь WebSocket-точки */
    web_server_t *server;    /* объект веб-сервера своего интерфейса */
} ws_transport_config_t;

static esp_err_t ws_httpd_handler(httpd_req_t *req);
#if WS_CONN_FROM_POST_HANDSHAKE_CB
static esp_err_t ws_httpd_post_handshake(httpd_req_t *req);
#endif
static ws_transport_conn_t *conn_open(httpd_req_t *req, int fd);

/* --- Служебное ------------------------------------------------------------ */

static uint32_t notify_event(ws_transport_conn_t *conn, const transport_event_trigger_t *ev) {
    /* Возврат — сколько подписчиков событие получило: ноль — обслуживать некому. */
    ws_transport_module_t *m = (conn != NULL) ? conn->module : NULL;
    if(m == NULL) return 0;
    return event_raise(&m->on_event, conn, (void *)ev);
}

static void conn_list_add(ws_transport_module_t *m, ws_transport_conn_t *conn) {
    if((m == NULL) || (m->conn_lock == NULL)) return;
    platform_mutex_take(m->conn_lock, PLATFORM_WAIT_FOREVER);
    for(size_t i = 0; i < WS_CONN_MAX; i++) {
        if(m->conns[i] == NULL) { m->conns[i] = conn; break; }
    }
    platform_mutex_give(m->conn_lock);
}

static void conn_list_remove(ws_transport_module_t *m, ws_transport_conn_t *conn) {
    if((m == NULL) || (m->conn_lock == NULL)) return;
    platform_mutex_take(m->conn_lock, PLATFORM_WAIT_FOREVER);
    for(size_t i = 0; i < WS_CONN_MAX; i++)
        if(m->conns[i] == conn) m->conns[i] = NULL;
    platform_mutex_give(m->conn_lock);
}

/* Снимок списка: досылка идёт без мьютекса — запись в сокет не критическая
 * секция, поэтому указатели копируются заранее. */
static int conn_list_snapshot(ws_transport_module_t *m, ws_transport_conn_t **out) {
    int n = 0;
    if((m == NULL) || (m->conn_lock == NULL)) return 0;
    platform_mutex_take(m->conn_lock, PLATFORM_WAIT_FOREVER);
    for(size_t i = 0; i < WS_CONN_MAX; i++)
        if(m->conns[i] != NULL) out[n++] = m->conns[i];
    platform_mutex_give(m->conn_lock);
    return n;
}

/* Сколько соединений держит экземпляр: по нему решается вежливый отказ. */
static int conn_list_count(ws_transport_module_t *m) {
    int n = 0;
    if((m == NULL) || (m->conn_lock == NULL)) return 0;
    platform_mutex_take(m->conn_lock, PLATFORM_WAIT_FOREVER);
    for(size_t i = 0; i < WS_CONN_MAX; i++)
        if(m->conns[i] != NULL) n++;
    platform_mutex_give(m->conn_lock);
    return n;
}

/* free_ctx у httpd принимает void*, поэтому обёртка над типизированным дескриптором. */
static void session_free_ctx(void *ctx) {
    ws_transport_conn_t *conn = (ws_transport_conn_t *)ctx;
    if(conn == NULL) return;
    event_unsubscribe(&conn->delegate);
    conn_list_remove(conn->module, conn);
    free(conn->out);
    conn->out     = NULL;
    conn->out_len = 0;
    conn->out_off = 0;
    conn->out_pending = 0;
    /* Сокет больше не наш — httpd мог отдать его новому соединению: -1 не даёт
     * позднему закрытию закрыть ЧУЖОЙ сокет. */
    conn->fd = -1;
    free(conn->msg);
    conn->msg    = NULL;
    conn->msg_len  = 0;
    conn->msg_cap  = 0;

    /* Ядру сообщаем о разрыве: свою сессию оно освободит по этому событию.
     * Дескриптор при этом ещё жив — ядро могло закрыть соединение само. */
    transport_event_trigger_t ev = { .event = TRANSPORT_CLOSED };
    notify_event(conn, &ev);

    if(conn->sends_in_flight || conn->in_callback || conn->recv_queued) {
        /* Дескриптором ещё пользуются отправки и уведомляющий колбэк — слот
         * вернёт последний из них. */
        conn->detached = 1;
        return;
    }
    conn_release(conn);
}

static void link_event_handler(void *event_trigger, void *sender, void *context) {
    (void)event_trigger; (void)sender;
    ws_transport_conn_t *conn = (ws_transport_conn_t *)context;
    LOGI(TAG, "Link down, triggering close for fd=%d", conn->fd);
    web_server_session_close(conn->module->server, conn->fd);
}

/* --- Операции транспорта как модуля --------------------------------------- */

/* Такт транспорта (transport_iface_t poll): доставка принятого подписчику.
 * Зовётся из задачи приложения — там стек 8 КБ, а не 4, как у веб-сервера.
 * Очередь FIFO: ядро получает сообщения в том порядке, в каком они пришли. */
static int  out_flush(ws_transport_conn_t *conn);
static void ws_out_pump(ws_transport_module_t *m);

static void ws_httpd_poll(const void *self) {
    ws_transport_module_t *m = (ws_transport_module_t *)self;
    if(m == NULL || m->recv_lock == NULL) return;

    ws_out_pump(m);

    for(;;) {
        platform_mutex_take(m->recv_lock, PLATFORM_WAIT_FOREVER);
        ws_transport_recv_t *rec = m->recv_head;
        if(rec != NULL) {
            m->recv_head = rec->next;
            if(m->recv_head == NULL) m->recv_tail = NULL;
            m->recv_bytes -= (rec->len + 1);
        }
        platform_mutex_give(m->recv_lock);
        if(rec == NULL) return;

        ws_transport_conn_t *conn = rec->conn;
        /* Запись могла пережить своё соединение: слот из пула уже переиспользован
         * под другого клиента. Доставляем только своему дескриптору. */
        if ((conn == NULL) || !conn->busy || (conn->gen != rec->gen)) {
            free(rec);
            continue;
        }
        transport_event_trigger_t ev = { .event = TRANSPORT_RECEIVED };
        ev.data.ptr    = rec->data;
        ev.data.length = rec->len;
        ev.data.left   = 0;                   /* сообщение отдано целиком */
        notify_event(conn, &ev);

        /* Дескриптор ждал этого сообщения: теперь его можно освободить. */
        if(conn != NULL && conn->recv_queued) conn->recv_queued--;
        uint8_t release = (conn != NULL) && conn->detached && !conn->sends_in_flight
                          && !conn->recv_queued && !conn->in_callback;
        free(rec);
        if(release) conn_release(conn);
    }
}

/* Досылка начатых кадров: клиент, снова начавший читать, получает остаток.
 * Клиента, у которого кадр не двигается дольше WS_OUT_DEAD_MS, считаем
 * пропавшим и закрываем сеанс — иначе буфер и счётчики висят вечно. */
static void ws_out_pump(ws_transport_module_t *m) {
    ws_transport_conn_t *list[WS_CONN_MAX];
    int n = conn_list_snapshot(m, list);
    uint32_t now = platform_tick();
    uint32_t dead_ticks = platform_ms_to_ticks(WS_OUT_DEAD_MS);

    for(int i = 0; i < n; i++) {
        ws_transport_conn_t *conn = list[i];
        if(!conn->busy || (conn->fd < 0) || (conn->out == NULL)) continue;
        uint32_t before = conn->out_off;
        int rc = out_flush(conn);
        if(rc < 0 || ((conn->out != NULL) && (conn->out_off == before) &&
                      ((uint32_t)(now - conn->out_since) > dead_ticks))) {
            ESP_LOGW(TAG, "Клиент fd=%d не принимает кадр (%lu из %lu Б в буфере) — закрываю",
                     conn->fd, (unsigned long)conn->out_off, (unsigned long)conn->out_len);
            if(conn->fd >= 0) web_server_session_close(m->server, conn->fd);
            continue;
        }
        if(conn->out != NULL && (conn->out_off > before))
            conn->out_since = now;          /* прогресс есть — отсчёт заново */
    }
}

static transport_result_t ws_httpd_init(const void *self, const void *config) {
    ws_transport_module_t *m = (ws_transport_module_t *)self;
    const ws_transport_config_t *cfg = (const ws_transport_config_t *)config;
    if((m == NULL) || (cfg == NULL) || (cfg->uri == NULL) || (cfg->server == NULL))
        return TRANSPORT_BAD_CALL;
    if(m->inited) return TRANSPORT_BUSY;   /* экземпляр уже инициализирован */

    /* Путь — своя копия: он живёт столько же, сколько экземпляр. */
    m->uri = strdup(cfg->uri);
    if(m->uri == NULL) return TRANSPORT_FAULT;
    m->conn_lock = platform_mutex_create();
    for(size_t ci = 0; ci < WS_CONN_MAX; ci++) m->conns[ci] = NULL;
    m->recv_lock = platform_mutex_create();
    m->recv_head = m->recv_tail = NULL;
    m->recv_bytes = 0;
    m->server = cfg->server;
    m->inited = 1;
    LOGI(TAG, "Init, iface=%lu, uri=%s",
             (unsigned long)web_server_iface(m->server), m->uri);
    return TRANSPORT_OK;
}

/* «Начать слушать»: активировать назначенный экземпляру URI. Раньше этого вызова
 * точка не работает, поэтому соединение, пришедшее до подписки ядра, не попадает в
 * «висящую» сессию, а не находит пути. */
static transport_result_t ws_httpd_open(const void *self, const void *config) {
    (void)config;
    ws_transport_module_t *m = (ws_transport_module_t *)self;
    if((m == NULL) || !m->inited) return TRANSPORT_BAD_CALL;
    if(m->opened) return TRANSPORT_BUSY;

    const web_ws_callbacks_t callbacks = {
        .handler          = ws_httpd_handler,
#if WS_CONN_FROM_POST_HANDSHAKE_CB
        .post_handshake   = ws_httpd_post_handshake,
#endif
        /* Управляющие кадры (PING/PONG/CLOSE) забирает обработчик транспорта: с
         * этим флагом веб-сервер отдаёт их нам, а не разбирает сам. */
        .handle_control_frames = 1,
    };
    /* Контекст колбэков — сам экземпляр: по нему обработчик узнаёт, чей это
     * вызов, таблицы «какой URI какому транспорту» не существует. */
    if(!web_server_activate_ws(m->server, m->uri, &callbacks, m)) {
        ESP_LOGE(TAG, "WS-точка '%s' не активирована на интерфейсе %lu",
                 m->uri, (unsigned long)web_server_iface(m->server));
        return TRANSPORT_BAD_CALL;
    }

    m->opened = 1;
    LOGI(TAG, "Open, iface=%lu: WS-точка '%s' активна",
             (unsigned long)web_server_iface(m->server), m->uri);
    return TRANSPORT_OK;
}

static transport_result_t ws_httpd_subscribe(const void *self, delegate_t *delegate) {
    ws_transport_module_t *m = (ws_transport_module_t *)self;
    if((m == NULL) || (delegate == NULL) || (delegate->handler == NULL))
        return TRANSPORT_BAD_CALL;
    /* Подписчик у каждого экземпляра свой. Модуль лишь кладёт делегат в свой
     * event_t; отписки здесь нет — её делает сам подписчик (event_unsubscribe). */
    event_subscribe(&m->on_event, delegate);
    LOGI(TAG, "Subscribed");
    return TRANSPORT_OK;
}

/* --- Операции соединения -------------------------------------------------- */

static transport_result_t ws_httpd_close(const void *self) {
    ws_transport_conn_t *conn = (ws_transport_conn_t *)self;
    if(conn == NULL) return TRANSPORT_BAD_CALL;
    /* Сессия уже закрыта (ядро снимает её раньше, чем узнаёт о завершении
     * отправок): закрывать нечего, а дескриптор мог достаться другому соединению. */
    if(conn->fd < 0) return TRANSPORT_OK;
    if(!web_server_session_close(conn->module->server, conn->fd)) {
        ESP_LOGW(TAG, "Close fd=%d failed", conn->fd);
        return TRANSPORT_BAD_CALL;   /* сессии уже нет */
    }
    return TRANSPORT_OK;
}

/* Отправка идёт одним куском: кадр и нарезку на TCP-сегменты делает веб-сервер,
 * прерывать нечего. */
static transport_result_t ws_httpd_break_sending(const void *self) {
    (void)self;
    return TRANSPORT_OK;
}

/* Заголовок кадра WebSocket (сервер не маскирует, RFC 6455 §5.2).
 * Возврат — длина заголовка. */
static int ws_frame_header(uint8_t *hdr, uint32_t len, uint8_t opcode) {
    int n = 0;
    hdr[n++] = (uint8_t)(0x80 | opcode);          /* FIN + тип */
    if(len <= 125) {
        hdr[n++] = (uint8_t)len;
    } else if(len <= 0xFFFF) {
        hdr[n++] = 126;
        hdr[n++] = (uint8_t)(len >> 8);
        hdr[n++] = (uint8_t)(len & 0xFF);
    } else {
        hdr[n++] = 127;
        for(int shift = 56; shift >= 0; shift -= 8)
            hdr[n++] = (uint8_t)(((uint64_t)len >> shift) & 0xFF);
    }
    return n;
}

/* Досылка начатого кадра. Возврат: 1 — буфер свободен (кадр ушёл целиком),
 * 0 — остаток ждёт места в сокете, -1 — сокет сломан. */
static int out_flush(ws_transport_conn_t *conn) {
    if(conn->out == NULL) return 1;
    /* Ожидания нет: остаток досылает такт транспорта, а ждать в такте нельзя —
     * задержка одного нечитающего клиента останавливает приём от всех. */
    int rc = web_server_write(conn->module->server, conn->fd,
                              (const char *)conn->out + conn->out_off,
                              conn->out_len - conn->out_off, 0);
    if(rc < 0) return -1;
    conn->out_off += (uint32_t)rc;
    if(conn->out_off < conn->out_len) return 0;
    free(conn->out);
    conn->out       = NULL;
    conn->out_len   = 0;
    conn->out_off   = 0;
    conn->out_pending = 0;
    return 1;
}

/* Кадр целиком: голова и данные в ОДНОМ буфере, поэтому кадр либо есть у клиента
 * весь, либо его нет. Возврат: 1 — принят, 0 — отброшен, -1 — сокет сломан,
 * -2 — у соединения ещё не ушёл предыдущий кадр. */
static int frame_queue(ws_transport_conn_t *conn, const void *data, uint32_t length) {
    if(conn->out != NULL) {
        /* Предыдущий кадр ещё в буфере: отдаём BUSY, сервис держит кадр у себя и
         * повторит его следующим тактом. */
        conn->out_dropped++;
        return -2;
    }

    uint8_t header[10];
    int hlen = ws_frame_header(header, length, 0x1);       /* 0x1 — text */
    uint32_t total = (uint32_t)hlen + length;
    if(total > WS_OUT_MAX) {
        ESP_LOGW(TAG, "Кадр %lu Б больше предела %u — отброшен, fd=%d",
                 (unsigned long)total, (unsigned)WS_OUT_MAX, conn->fd);
        return 0;
    }

    uint8_t *buf = malloc(total);
    if(buf == NULL) return 0;
    memcpy(buf, header, (size_t)hlen);
    memcpy(buf + hlen, data, length);
    conn->out        = buf;
    conn->out_len    = total;
    conn->out_off    = 0;
    conn->out_since  = platform_tick();
    conn->out_pending = 1;

    int sent = out_flush(conn);
    if(sent >= 0) return 1;
    ESP_LOGW(TAG, "out_flush: сокет сломан (fd=%d, %lu Б)", conn->fd, (unsigned long)total);
    /* Сокет сломан: ядро узнает отказом ниже. */
    return -1;
}

static transport_result_t ws_httpd_begin_send(const void *self, const void *data, uint32_t length) {
    ws_transport_conn_t *conn = (ws_transport_conn_t *)self;
    if((conn == NULL) || (data == NULL) || (length == 0)) return TRANSPORT_BAD_CALL;
    /* Соединение закрыто: писать некуда. */
    if(conn->fd < 0) return TRANSPORT_BAD_CALL;

    /* Дескриптор нужен уведомлению: обработчик закрытия его не освободит
     * (in_callback), поэтому освобождаем сами, если сессия уже закрыта. */
    conn->sends_in_flight++;
    int queued = frame_queue(conn, data, length);
    conn->sends_in_flight--;

    if(queued == -2) {
        /* Данные остаются у вызывающего, поэтому объявлять нечего — только BUSY,
         * по нему отправка повторится. */
        if(conn->detached && !conn->sends_in_flight && !conn->recv_queued) conn_release(conn);
        return TRANSPORT_BUSY;
    }

    /* Завершение объявляем сразу и всегда: данные у нас в СВОЕЙ копии (или
     * отброшены), буфер ядра больше не нужен. Отказ — только когда поток
     * испорчен и соединение надо закрыть. */
    transport_event_trigger_t ev = {
        .event = (queued < 0) ? TRANSPORT_TRANSFER_ERR : TRANSPORT_TRANSFER_COMPLETE,
    };
    ev.data.ptr    = data;
    ev.data.length = length;
    ev.data.left   = 0;

    conn->in_callback = 1;
    notify_event(conn, &ev);
    conn->in_callback = 0;
    if(conn->detached && !conn->sends_in_flight && !conn->recv_queued) conn_release(conn);
    return TRANSPORT_OK;
}

static int32_t ws_httpd_peer_id(const void *self) {
    const ws_transport_conn_t *conn = (const ws_transport_conn_t *)self;
    return conn ? conn->fd : -1;
}

/* Таблица операций одна на все экземпляры: разница только в том, ЧЕЙ контекст
 * им передали (transport_t.context). */
static const transport_iface_t ws_iface = {
    .init          = ws_httpd_init,
    .open          = ws_httpd_open,
    .close         = ws_httpd_close,
    .begin_send    = ws_httpd_begin_send,
    .break_sending = ws_httpd_break_sending,
    .subscribe     = ws_httpd_subscribe,
    .poll          = ws_httpd_poll,
    .peer_id       = ws_httpd_peer_id,
};

/* ── Объект транспорта ──────────────────────────────────────────────────────
 *
 * Наружу транспорт отдаёт ОБЪЕКТ: методы и свой контекст (transport_t). Объект
 * порождается вместе с экземпляром и освобождается вместе с ним
 * (ws_transport_destroy): экземпляр существует только потому, что его породил
 * объект веб-сервера интерфейса. */

/* --- Приём ---------------------------------------------------------------- */

/* Запас места под ещё len байт. NULL — сообщение вышло за предел или нет памяти:
 * тогда соединение закрываем, принимать его негде. */
static uint8_t *msg_reserve(ws_transport_conn_t *conn, uint32_t len) {
    if(len > TRANSPORT_MESSAGE_MAX_LEN) return NULL;
    if(conn->msg_cap < conn->msg_len + len + 1) {
        uint32_t cap = conn->msg_len + len + 1;
        uint8_t *buf = realloc(conn->msg, cap);
        if(buf == NULL) return NULL;
        conn->msg   = buf;
        conn->msg_cap = cap;
    }
    return conn->msg + conn->msg_len;
}

/* Отбросить недособранное сообщение. */
static void drop_message(ws_transport_conn_t *conn) {
    free(conn->msg);
    conn->msg   = NULL;
    conn->msg_len = 0;
    conn->msg_cap = 0;
}

/* Предел того, что копится непринятым: очередь разбирают на каждом такте, но
 * одно сообщение бывает крупным — порция заливки прошивки приходит base64. */
#define WS_RECV_QUEUE_MAX     16
#define WS_RECV_QUEUE_BYTES    (32u * 1024u)

/* Собранное сообщение — в очередь, а не сразу подписчику: разбор идёт в задаче
 * приложения, и ядро фрагментов не видит. Копия нужна, потому что буфер
 * соединения переиспользуется следующим кадром. */
static void deliver_message(ws_transport_conn_t *conn) {
    if(conn->msg_len == 0) {
        /* Пустое сообщение ядру не нужно. */
        drop_message(conn);
        return;
    }

    ws_transport_module_t *m = conn->module;
    if((m->recv_lock == NULL) ||
       (conn->recv_queued >= WS_RECV_QUEUE_MAX) ||
       (m->recv_bytes + conn->msg_len > WS_RECV_QUEUE_BYTES)) {
        /* Разбирать негде или копится больше предела: закрываем соединение —
         * молча терять сообщения клиента нельзя, он ждёт ответа. */
        ESP_LOGW(TAG, "Очередь приёма заполнена (%lu Б в ней), закрываю fd=%d",
                 (unsigned long)m->recv_bytes, conn->fd);
        drop_message(conn);
        web_server_session_close(m->server, conn->fd);
        return;
    }

    ws_transport_recv_t *rec = malloc(sizeof(*rec) + conn->msg_len + 1);
    if(rec == NULL) {
        ESP_LOGW(TAG, "Нет памяти под принятое сообщение (%lu Б), закрываю fd=%d",
                 (unsigned long)conn->msg_len, conn->fd);
        drop_message(conn);
        web_server_session_close(m->server, conn->fd);
        return;
    }
    rec->next = NULL;
    rec->conn = conn;
    rec->gen  = conn->gen;
    rec->len  = conn->msg_len;
    memcpy(rec->data, conn->msg, conn->msg_len);
    rec->data[conn->msg_len] = '\0';   /* ядру удобно видеть строку в логе */

    platform_mutex_take(m->recv_lock, PLATFORM_WAIT_FOREVER);
    if(m->recv_tail != NULL) m->recv_tail->next = rec;
    else                     m->recv_head = rec;
    m->recv_tail   = rec;
    m->recv_bytes += rec->len + 1;
    conn->recv_queued++;
    platform_mutex_give(m->recv_lock);

    drop_message(conn);
}

/* Управляющие кадры обрабатывает транспорт: ядру PING/PONG/CLOSE не отдаются ни
 * при каких условиях, а PONG и CLOSE за нас никто не отправит. */
static esp_err_t ws_httpd_control_frame(ws_transport_conn_t *conn, httpd_req_t *req,
                                        httpd_ws_frame_t *f) {
    int fd = httpd_req_to_sockfd(req);
    uint8_t payload[WS_CONTROL_PAYLOAD_MAX_LEN + 1];

    if(f->len > WS_CONTROL_PAYLOAD_MAX_LEN) {
        ESP_LOGW(TAG, "Управляющий кадр длиной %u — не по RFC 6455, закрываю fd=%d",
                 (unsigned)f->len, fd);
        web_server_session_close(conn->module->server, fd);
        return ESP_OK;
    }
    /* Payload надо вычитать из сокета, даже если он не нужен: иначе следующий
     * кадр разберётся не с начала. */
    if(f->len) {
        f->payload = payload;
        esp_err_t ret = httpd_ws_recv_frame(req, f, f->len);
        if(ret != ESP_OK) {
            ESP_LOGW(TAG, "Ошибка чтения управляющего кадра (err %d), fd=%d", ret, fd);
            return ret;
        }
    }

    if((f->type == HTTPD_WS_TYPE_PING) || (f->type == HTTPD_WS_TYPE_CLOSE)) {
        /* На PING отвечаем PONG, на CLOSE — CLOSE (RFC 6455 §5.5.2, §5.5.1):
         * буфер на стеке живёт до конца отправки. */
        httpd_ws_frame_t reply = {
            .type    = (f->type == HTTPD_WS_TYPE_PING) ? HTTPD_WS_TYPE_PONG : HTTPD_WS_TYPE_CLOSE,
            .final   = true,
            .payload = f->len ? payload : NULL,
            .len     = f->len,
        };
        esp_err_t err = web_server_send_ws(conn->module->server, fd, &reply, NULL, NULL);
        if(err != ESP_OK) ESP_LOGW(TAG, "Ответ на управляющий кадр не ушёл (err %d), fd=%d", err, fd);
    }
    return ESP_OK;
}

static esp_err_t ws_httpd_handler(httpd_req_t *req) {
    int fd = httpd_req_to_sockfd(req);

    if (req->method == HTTP_GET) {
#if WS_CONN_FROM_POST_HANDSHAKE_CB
        /* Рукопожатие уже отвечено esp_http_server, сессия заведена в
         * ws_httpd_post_handshake(). */
        return ESP_OK;
#else
        /* conn_open сам решает, обслуживать ли соединение: если ядро не подписано
         * или нет памяти, оно уже закрыто. */
        return conn_open(req, fd) ? ESP_OK : ESP_FAIL;
#endif
    }

    ws_transport_conn_t *conn = (ws_transport_conn_t *)req->sess_ctx;
    if (!conn) {
        /* Без объекта сессии передавать кадр некому — лучше отбросить, чем
         * разыменовать NULL. */
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
        return ws_httpd_control_frame(conn, req, &f);

    /* Кадр больше предела: не читаем и не разбираем — закрываем соединение,
     * клиент получает закрытие вместо ответа API. */
    if (f.len > TRANSPORT_FRAME_MAX_LEN) {
        ESP_LOGW(TAG, "Кадр %u байт больше предела %d — закрываю fd=%d",
                 (unsigned)f.len, TRANSPORT_FRAME_MAX_LEN, fd);
        web_server_session_close(conn->module->server, fd);
        return ESP_OK;
    }

    if(f.type == HTTPD_WS_TYPE_CONTINUE) {
        if(conn->msg == NULL) {
            /* Продолжение без начала: склеивать не с чем. */
            ESP_LOGW(TAG, "Продолжение без начала сообщения, отброшено fd=%d", fd);
            return ESP_OK;
        }
        uint8_t *dst = msg_reserve(conn, f.len);
        if(dst == NULL) {
            ESP_LOGW(TAG, "Сообщение больше %d байт — закрываю fd=%d",
                     TRANSPORT_MESSAGE_MAX_LEN, fd);
            drop_message(conn);
            web_server_session_close(conn->module->server, fd);
            return ESP_OK;
        }
        f.payload = dst;
        if(f.len) {
            ret = httpd_ws_recv_frame(req, &f, f.len);
            if(ret != ESP_OK) {
                ESP_LOGW(TAG, "Failed to read ws payload (err %d)", ret);
                drop_message(conn);
                return ret;
            }
        }
        conn->msg_len += f.len;
        if(f.final) deliver_message(conn);
        return ESP_OK;
    }

    /* Начало нового сообщения (TEXT/BINARY). Незавершённое предыдущее
     * выбрасываем: целым оно уже не станет, а склеивать его с новым нельзя. */
    if(conn->msg != NULL) {
        ESP_LOGW(TAG, "Незавершённое сообщение отброшено, fd=%d", fd);
        drop_message(conn);
    }

    uint8_t *dst = msg_reserve(conn, f.len);
    if(dst == NULL) {
        ESP_LOGW(TAG, "Нет памяти под кадр %u байт, закрываю fd=%d", (unsigned)f.len, fd);
        web_server_session_close(conn->module->server, fd);
        return ESP_OK;
    }
    f.payload = dst;
    if(f.len) {
        ret = httpd_ws_recv_frame(req, &f, f.len);
        if(ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to read ws payload (err %d)", ret);
            drop_message(conn);
            return ret;
        }
    }
    conn->msg_len += f.len;
    if(f.final) deliver_message(conn);
    return ESP_OK;
}

/* --- Соединение ----------------------------------------------------------- */

/* Дескриптор соединения заводится один раз на сессию и живёт в req->sess_ctx:
 * esp_http_server переносит его в сессию и отдаёт в каждый следующий запрос той
 * же сессии, а session_free_ctx освобождает при её завершении.
 *
 * Экземпляр транспорта приходит контекстом обработчика (req->user_ctx) — запрос
 * дошёл сюда потому, что его обслуживает ЭТОТ объект (web_server_activate_ws).
 * Чей это клиент, здесь не выясняется; номер сокета известен только тут. */
static ws_transport_conn_t *conn_open(httpd_req_t *req, int fd) {
    ws_transport_conn_t *conn = (ws_transport_conn_t *)req->sess_ctx;
    if (conn) return conn;

    ws_transport_module_t *module = (ws_transport_module_t *)req->user_ctx;
    if(module == NULL) {
        /* Контекста экземпляра нет — отдать соединение некому, возврат NULL
         * закрывает сессию: вызывающий отвечает веб-серверу ошибкой. */
        ESP_LOGW(TAG, "WS-точка без экземпляра транспорта, закрываю fd=%d", fd);
        return NULL;
    }

    /* Слотов у экземпляра столько, сколько задано конфигурацией проекта. Порт
     * держит на один сокет больше, поэтому лишний клиент доходит сюда — и
     * получает вежливый отказ (кадр CLOSE 1013), а не молчание. */
    if (conn_list_count(module) >= (int)CONFIG_WEB_SOCKET_MAX_CLIENTS) {
        ESP_LOGW(TAG, "WS-клиентов уже %d, отказываю fd=%d",
                 conn_list_count(module), fd);
        return NULL;
    }

    conn = conn_alloc();
    if (!conn) {
        ESP_LOGW(TAG, "Пул соединений исчерпан — закрываю fd=%d", fd);
        return NULL;
    }
    conn->fd = fd;
    conn->module = module;
    conn->delegate.handler = link_event_handler;
    conn->delegate.context = conn;
    net_link_subscribe(fd, &conn->delegate);
    conn_list_add(module, conn);

    transport_event_trigger_t ev = { .event = TRANSPORT_ESTABLISHED };
    if(notify_event(conn, &ev) == 0) {
        /* Событие не получил никто: на экземпляр не подписано ядро или оно уже
         * сняло подписку. Обслуживать соединение нечем — сессию не заводим. */
        ESP_LOGE(TAG, "Ядро не подписано на интерфейсе %lu, закрываю fd=%d",
                 (unsigned long)web_server_iface(module->server), fd);
        event_unsubscribe(&conn->delegate);
        conn_list_remove(module, conn);
        conn_release(conn);
        return NULL;
    }

    LOGI(TAG, "WS connection accepted, fd=%d, iface=%lu",
             fd, (unsigned long)web_server_iface(module->server));

    req->sess_ctx = conn;
    req->free_ctx = session_free_ctx;
    return conn;
}

#if WS_CONN_FROM_POST_HANDSHAKE_CB
/* Вежливый отказ: слотов нет. Рукопожатие httpd уже подтвердил, поэтому
 * HTTP-ошибку отдать нечем — отправляем штатный кадр CLOSE с кодом 1013
 * (Try Again Later, RFC 6455 §7.4.1) прямо в сокет: клиент видит причину сразу,
 * а не висит до своего таймаута. */
static void ws_refuse_busy(httpd_req_t *req) {
    ws_transport_module_t *m = (ws_transport_module_t *)req->user_ctx;
    static const char reason[] = "busy: too many WebSocket clients";
    uint8_t frame[2 + 2 + sizeof(reason) - 1];
    frame[0] = 0x88;                        /* FIN + CLOSE */
    frame[1] = (uint8_t)(2 + sizeof(reason) - 1);
    frame[2] = 0x03;                        /* код 1013 */
    frame[3] = 0xF5;
    memcpy(&frame[4], reason, sizeof(reason) - 1);

    if ((m != NULL) && (m->server != NULL))
        web_server_write(m->server, httpd_req_to_sockfd(req),
                         (const char *)frame, sizeof(frame), 0);
}

/* Ответ рукопожатия уже отправлен: сокет с этого момента — WebSocket. */
static esp_err_t ws_httpd_post_handshake(httpd_req_t *req) {
    int fd = httpd_req_to_sockfd(req);
    if (!conn_open(req, fd)) {
        ESP_LOGW(TAG, "WS session not created, fd=%d", fd);
        ws_refuse_busy(req);
        return ESP_FAIL;
    }
    LOGI(TAG, "WS session opened, fd=%d", fd);
    return ESP_OK;
}
#endif

/* --- Порождение и освобождение экземпляра --------------------------------- */

/* Экземпляр порождает веб-сервер интерфейса (web_server_open_websocket): интерфейс
 * уже задан объектом, поэтому сюда он не передаётся. URI ещё не активирован —
 * активацию делает open, после того как назначен обработчик событий. */
transport_t *ws_transport_create(web_server_t *server, const char *uri) {
    if((server == NULL) || (uri == NULL)) return NULL;

    /* Пул дескрипторов общий на все экземпляры: замок заводится один раз. */
    if(ws_pool_lock == NULL) ws_pool_lock = platform_mutex_create();
    if(ws_pool_lock == NULL) return NULL;

    ws_transport_module_t *m = calloc(1, sizeof(*m));
    if(m == NULL) return NULL;

    const ws_transport_config_t cfg = { .uri = uri, .server = server };
    if(ws_iface.init(m, &cfg) != TRANSPORT_OK) {
        free(m);
        return NULL;
    }

    /* Объект появляется вместе с экземпляром: без контекста транспорта не бывает. */
    transport_t *transport = malloc(sizeof(*transport));
    if(transport == NULL) {
        while(m->recv_head != NULL) {
            ws_transport_recv_t *rec = m->recv_head;
            m->recv_head = rec->next;
            free(rec);
        }
        free(m->uri);
        free(m);
        return NULL;
    }
    transport->iface   = &ws_iface;
    transport->context = m;
    return transport;
}

void ws_transport_destroy(transport_t *transport) {
    if(transport == NULL) return;
    ws_transport_module_t *m = (ws_transport_module_t *)transport->context;
    if(m != NULL) {
        free(m->uri);
        free(m);
    }
    free(transport);
}
