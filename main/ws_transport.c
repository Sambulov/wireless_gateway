/* ws_transport.c — транспорт на WebSocket как надстройка над веб-сервером:
 * соединения и кадры приносит объект веб-сервера интерфейса, транспорт собирает
 * сообщения и поднимает события своему подписчику (ядру API). */

#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "esp_http_server.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "CodeLib.h"
#include "connection.h"
/* api.h транспорту не нужен: он знает только свой объект и подписчика. */
#include "transport.h"
#include "ws_transport.h"
#include "ws_msg.h"
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
    ws_msg_t *msg;
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
    /* Отправка — три режима контракта begin_send:
     *   1) data != NULL                 — zero-copy: блок издателя живёт до завершения;
     *   2) data == NULL, length < STREAM — транспорт выделяет буфер с запасом под свой
     *      заголовок и ВЫДАЁТ его событием TRANSFER_BUFFER; издатель заполняет прямо в
     *      обработчике и ставит fill;
     *   3) data == NULL, length == STREAM — поток таких выдач до break_sending.
     * Заголовок кадра пишется в запас перед выданным указателем, поэтому полезная
     * нагрузка не копируется ни в одном режиме. */
    uint8_t  hdr[10];
    uint32_t hdr_len;        /* заголовок кадра WS (режим 1) */
    uint32_t hdr_off;
    const uint8_t *tx_data;  /* данные: чужой блок (1) или наш буфер (2,3) */
    uint32_t tx_len;
    uint32_t tx_off;
    uint8_t  tx_mode;        /* 0 — свободно, 1 — zero-copy, 2 — выдача, 3 — поток */
    uint32_t tx_expected;    /* сколько обещано издателю (2,3) */
    uint32_t tx_given;       /* сколько уже отдано (2,3) */
    uint8_t *tx_buf;         /* наш буфер под выдачу: запас + данные */
    uint32_t out_since;      /* когда кадр начат — для решения «клиент пропал» */
    uint8_t  out_pending;    /* в буфере есть недописанное */
    uint8_t  tx_dead;        /* клиент не принимает: отправку прекратили */
    uint16_t out_dropped;    /* сколько кадров отброшено за время занятости */
    /* Ожидающий управляющий кадр (PONG/CLOSE): строится в задаче httpd из
     * ws_httpd_control_frame, отправляется ТАКТОМ транспорта под ws_frame_lock —
     * вместе с кадрами данных. Прямая запись из задачи httpd другим путём
     * (httpd_ws_send_frame_async) вклинивалась бы в середину идущего кадра данных
     * и рассыпала поток клиента («invalid opcode»). */
    uint8_t  ctrl_pending;
    uint8_t  ctrl_buf[2 + WS_CONTROL_PAYLOAD_MAX_LEN];
    uint32_t ctrl_len;
    uint32_t ctrl_off;           /* сколько байт управляющего кадра уже ушло */
    uint32_t ctrl_since;         /* тик постановки или последнего прогресса */
    uint8_t  close_after;        /* ответ CLOSE ждёт, пока допишется PONG */
    /* Адрес КЛИЕНТА на момент открытия (getpeername): по нему видно, что fd всё
     * ещё наша WS-сессия. Номер fd переиспользуется, и запись/закрытие «по fd» из
     * чужой задачи могли ударить по живому клиенту или по сокету страницы. */
    uint32_t peer_ip;
    uint16_t peer_port;
} ws_transport_conn_t;

/* Предел отложенного кадра на соединение: кадр потока — до ~2,7 КБ base64,
 * ответ вызова — сотни байт, 8 КБ хватает. Больше — отбрасываем. */
#define WS_OUT_MAX   8192u
/* Запас впереди данных под заголовок кадра: когда буфер выдаём МЫ, издатель
 * получает смещённый указатель, а заголовок встаёт в этот запас — кадр уходит
 * одним куском без копирования полезной нагрузки. */
#define WS_FRAME_HEADROOM 14u
/* Сколько кадров отдаём за один такт транспорта: поток мелких кадров не должен
 * упираться в частоту такта. */
#define WS_TX_FRAMES_PER_CYCLE 8
/* Сколько кадр может висеть без прогресса, прежде чем клиент считается пропавшим. */
#define WS_OUT_DEAD_MS   20000u
/* Сколько принятых сообщений разбираем за один такт транспорта. */
#define WS_DELIVER_PER_CYCLE 8

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
 * Дескрипторы не освобождаются: такт работает по снимку указателей, сделанному
 * до закрытия сессии, поэтому «освобождение» — это возврат слота: буферы
 * отдаются, fd становится -1, busy снимается, номер поколения растёт. Записи
 * очереди приёма запоминают поколение и доставляются только своему дескриптору.
 *
 * Пул на два экземпляра (AP и STA). */
static ws_transport_conn_t ws_conn_pool[WS_CONN_MAX * 2];
static platform_mutex_t    ws_pool_lock;
static uint32_t            ws_conn_gen;
/* Замок состояния кадра (hdr_*, tx_*, tx_mode, out_*): begin_send/break_sending
 * зовёт задача ядра API, а досылает кадры ТАКТ транспорта — на двух ядрах это два
 * разных потока, и без замка досылка читает поле, которое ещё только пишется
 * (header ушёл, а tx_len ещё ноль → битый кадр/зависание). */
static platform_mutex_t    ws_frame_lock;
/* Замок событий и учёта жизни слота. event_raise (CL) помечает делегатов в
 * общем списке без своего замка, а события одного экземпляра поднимают две
 * задачи: такт транспорта (RECEIVED, TRANSFER_*) и задача httpd (ESTABLISHED,
 * CLOSED). Без замка параллельный подъём пропускал помеченного делегата —
 * CLOSED не доходил до ядра, и пир со слотом пула утекали. Под этим же замком
 * — счётчики recv_queued/in_callback/detached и решение вернуть слот: их
 * правят те же две задачи. Порядок замков: event → api_lock ядра (его берёт
 * обработчик) → frame; обратного нет — под api_lock ядро зовёт только
 * begin_send zero-copy, который событий не поднимает. Режимы begin_send с
 * TRANSFER_BUFFER (tx_offer) поднимают событие синхронно, и звать их под
 * своим замком подписчику нельзя. */
static platform_mutex_t    ws_event_lock;

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
    /* Под замком пула, как и conn_alloc: иначе alloc мог занять (memset) слот,
     * который здесь ещё дочищается. busy снимается последним. */
    platform_mutex_take(ws_pool_lock, PLATFORM_WAIT_FOREVER);
    free(conn->tx_buf);
    conn->tx_buf = NULL;
    ws_msg_free(&conn->msg);
    conn->fd  = -1;
    conn->detached = 0;
    conn->in_callback = 0;
    conn->sends_in_flight = 0;
    conn->recv_queued = 0;
    conn->busy = 0;
    platform_mutex_give(ws_pool_lock);
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
    event_t   on_event;
    /* Веб-сервер своего интерфейса и путь WS-точки (путь даёт open). */
    web_server_t *server;
    char         *uri;
    uint8_t   inited;
    uint8_t   opened;
    /* Соединения экземпляра — по ним такт транспорта досылает начатые кадры. */
    platform_mutex_t conn_lock;
    ws_transport_conn_t *conns[WS_CONN_MAX];
    /* Принятые, но ещё не разобранные сообщения: их доставляет рабочий такт
     * транспорта, задача веб-сервера только копирует сюда. */
    platform_mutex_t recv_lock;
    ws_transport_recv_t *recv_head;
    ws_transport_recv_t *recv_tail;
    uint32_t recv_bytes;
    TaskHandle_t cycle_task;
    volatile uint8_t cycle_stop;
};

/* Состояние реализаций: по одному на интерфейс (AP и STA). В объекте транспорта
 * его нет — там лежит веб-сервер. */
#define WS_MODULE_COUNT 2
static ws_transport_module_t ws_modules[WS_MODULE_COUNT];

static web_server_t *srv_of(const void *self) {
    const transport_t *tr = (const transport_t *)self;
    return (tr != NULL) ? (web_server_t *)tr->context : NULL;
}

static ws_transport_module_t *mod_of(const void *self) {
    web_server_t *srv = srv_of(self);
    if(srv == NULL) return NULL;
    return &ws_modules[(web_server_iface(srv) == GW_IFACE_STA) ? 1 : 0];
}

static esp_err_t ws_httpd_handler(httpd_req_t *req);
#if WS_CONN_FROM_POST_HANDSHAKE_CB
static esp_err_t ws_httpd_post_handshake(httpd_req_t *req);
#endif
static ws_transport_conn_t *conn_open(httpd_req_t *req, int fd);
static uint8_t conn_is_ours(const ws_transport_conn_t *conn);
static void conn_close_session(ws_transport_conn_t *conn, const char *reason);

/* --- Служебное ------------------------------------------------------------ */

static uint32_t notify_event(ws_transport_conn_t *conn, const transport_event_trigger_t *ev) {
    /* Возврат — сколько подписчиков событие получило: ноль — обслуживать некому. */
    ws_transport_module_t *m = (conn != NULL) ? conn->module : NULL;
    if(m == NULL) return 0;
    platform_mutex_take(ws_event_lock, PLATFORM_WAIT_FOREVER);
    uint32_t n = event_raise(&m->on_event, conn, (void *)ev);
    platform_mutex_give(ws_event_lock);
    return n;
}

/* Событие об отправке и проверка, не пора ли вернуть слот: сессию могли закрыть,
 * пока колбэк шёл (detached), — тогда слот возвращает последний пользователь.
 * Всё — под замком событий, иначе session_free_ctx вклинивался между проверкой
 * и возвратом (утечка слота или двойной возврат). */
static void notify_callback(ws_transport_conn_t *conn, uint32_t gen,
                            const transport_event_trigger_t *ev) {
    platform_mutex_take(ws_event_lock, PLATFORM_WAIT_FOREVER);
    /* Событие считано под замком кадра, а поднимается после: за это время
     * сессия могла закрыться, слот — вернуться в пул и уйти НОВОМУ соединению
     * с тем же указателем. Его ядру чужое событие об отправке снимало бы
     * голову очереди. Поколение слота это отличает. */
    if(!conn->busy || (conn->gen != gen)) {
        platform_mutex_give(ws_event_lock);
        return;
    }
    conn->in_callback = 1;
    notify_event(conn, ev);
    conn->in_callback = 0;
    if(conn->detached && !conn->sends_in_flight && !conn->recv_queued)
        conn_release(conn);
    platform_mutex_give(ws_event_lock);
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
    /* Состояние кадра — под тем же замком, что у такта, который может прямо
     * сейчас досылать кадр из tx_buf (иначе — запись из освобождённого буфера). */
    platform_mutex_take(ws_frame_lock, PLATFORM_WAIT_FOREVER);
    free(conn->tx_buf);
    conn->tx_buf      = NULL;
    conn->tx_data     = NULL;
    conn->tx_len      = 0;
    conn->tx_off      = 0;
    conn->hdr_len     = 0;
    conn->hdr_off     = 0;
    conn->tx_mode     = 0;
    conn->out_pending = 0;
    conn->ctrl_pending = 0;
    conn->ctrl_off     = 0;
    conn->close_after  = 0;
    /* Сокет больше не наш — httpd мог отдать его новому соединению: -1 не даёт
     * позднему закрытию закрыть ЧУЖОЙ сокет. */
    conn->fd = -1;
    platform_mutex_give(ws_frame_lock);
    ws_msg_free(&conn->msg);

    /* Ядру сообщаем о разрыве: свою сессию оно освободит по этому событию.
     * Дескриптор при этом ещё жив — ядро могло закрыть соединение само. Решение
     * о слоте — под замком событий вместе с самим событием. */
    platform_mutex_take(ws_event_lock, PLATFORM_WAIT_FOREVER);
    transport_event_trigger_t ev = { .event = TRANSPORT_CLOSED };
    notify_event(conn, &ev);
    if(conn->sends_in_flight || conn->in_callback || conn->recv_queued) {
        /* Дескриптором ещё пользуются отправки и уведомляющий колбэк — слот
         * вернёт последний из них. */
        conn->detached = 1;
    } else {
        conn_release(conn);
    }
    platform_mutex_give(ws_event_lock);
}

static void link_event_handler(void *event_trigger, void *sender, void *context) {
    (void)event_trigger; (void)sender;
    ws_transport_conn_t *conn = (ws_transport_conn_t *)context;
    LOGI(TAG, "Link down, triggering close for fd=%d", conn->fd);
    /* Дескриптор мог быть переиспользован: закрываем только если fd всё ещё наш,
     * иначе просто прекращаем отправку (см. conn_close_session). */
    conn_close_session(conn, "линк клиента пропал");
}

/* --- Операции транспорта как модуля --------------------------------------- */

/* Рабочий такт транспорта: досылка начатых кадров и доставка принятого
 * подписчику (ядру). Очередь FIFO: ядро получает сообщения в том порядке, в каком
 * они пришли.
 *
 * Крутит его САМ транспорт — своей задачей (см. ws_cycle_task): экземпляр
 * порождает слой веба, он же отвечает за рабочие циклы своего транспорта, а ядру
 * API цикл чужого объекта знать незачем. Стек здесь свой (4 КБ), поэтому разбор
 * сообщения идёт в этой задаче, а не в задаче веб-сервера (там оставалось 360
 * байт из 4096 — замерено на приборе). */
static int  out_flush(ws_transport_conn_t *conn);
static void ws_out_pump(ws_transport_module_t *m);
static void tx_offer(ws_transport_conn_t *conn);
static int  ws_frame_header(uint8_t *hdr, uint32_t len, uint8_t opcode);

static void ws_httpd_poll(const void *self) {
    ws_transport_module_t *m = (ws_transport_module_t *)self;
    if(m == NULL || m->recv_lock == NULL) return;

    /* Сначала досылка: она не должна ждать разбора принятого. */
    ws_out_pump(m);

    /* За такт разбираем ОГРАНИЧЕННОЕ число сообщений. Доставка уходит в ядро (там
     * свой мьютекс и работа с вызовами), и если она займёт все сообщения сразу,
     * следующий такт начнётся поздно — а это напрямую задержка отправки. */
    for(int i = 0; i < WS_DELIVER_PER_CYCLE; i++) {
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
        platform_mutex_take(ws_event_lock, PLATFORM_WAIT_FOREVER);
        notify_event(conn, &ev);

        /* Дескриптор ждал этого сообщения: теперь его можно освободить. */
        if(conn->recv_queued) conn->recv_queued--;
        if(conn->detached && !conn->sends_in_flight && !conn->recv_queued && !conn->in_callback)
            conn_release(conn);
        platform_mutex_give(ws_event_lock);
        free(rec);
    }
}

/* Задача рабочего такта транспорта: цикл, пока экземпляр открыт. Приоритет ВЫШЕ
 * задачи такта API (5, см. app.c): досылка кадров — это работа с сокетом, и если
 * её отодвигать, весь поток встаёт за одним недодосланным кадром. */
#define WS_CYCLE_PERIOD_MS  2
#define WS_CYCLE_STACK      4096
#define WS_CYCLE_PRIO       6

static void ws_cycle_task(void *arg) {
    ws_transport_module_t *m = (ws_transport_module_t *)arg;
    while(!m->cycle_stop) {
        ws_httpd_poll(m);
        vTaskDelay(pdMS_TO_TICKS(WS_CYCLE_PERIOD_MS));
    }
    m->cycle_task = NULL;
    vTaskDelete(NULL);
}

static void ws_cycle_start(ws_transport_module_t *m) {
    if(m->cycle_task != NULL) return;
    m->cycle_stop = 0;
    if(xTaskCreate(ws_cycle_task, "ws_cycle", WS_CYCLE_STACK, m,
                   WS_CYCLE_PRIO, &m->cycle_task) != pdPASS) {
        m->cycle_task = NULL;
        ESP_LOGE(TAG, "Не удалось запустить такт транспорта (iface=%lu)",
                 (unsigned long)web_server_iface(m->server));
    }
}

static void ws_cycle_stop(ws_transport_module_t *m) {
    m->cycle_stop = 1;
    /* Даём задаче выйти самой: она не держит ни мьютексов, ни сокетов. */
    for(int i = 0; (i < 50) && (m->cycle_task != NULL); i++)
        vTaskDelay(pdMS_TO_TICKS(1));
}

/* Досылка начатых кадров: клиент, снова начавший читать, получает остаток.
 * Клиента, у которого кадр не двигается дольше WS_OUT_DEAD_MS, считаем
 * пропавшим и закрываем сеанс — иначе буфер и счётчики висят вечно. */
/* Сокет всё ещё наш? Номер fd переиспользуется, поэтому перед записью или
 * закрытием из чужой задачи сверяем АДРЕС КЛИЕНТА. Локальный адрес у всех
 * принятых сокетов одинаков (это адрес слушающего порта), поэтому различаем по
 * удалённому адресу: у нового соединения другой исходящий порт клиента. */
static uint8_t conn_is_ours(const ws_transport_conn_t *conn) {
    if((conn == NULL) || (conn->fd < 0)) return 0;
    struct sockaddr_in sa;
    socklen_t len = sizeof(sa);
    memset(&sa, 0, sizeof(sa));
    if(getpeername(conn->fd, (struct sockaddr *)&sa, &len) != 0) return 0;
    return (sa.sin_addr.s_addr == conn->peer_ip) && (sa.sin_port == conn->peer_port);
}

/* Закрыть сессию, только если она ещё наша; иначе — пометить соединение мёртвым. */
static void conn_close_session(ws_transport_conn_t *conn, const char *reason) {
    platform_mutex_take(ws_frame_lock, PLATFORM_WAIT_FOREVER);
    if(conn->tx_dead) {
        platform_mutex_give(ws_frame_lock);
        return;
    }
    conn->tx_dead = 1;
    platform_mutex_give(ws_frame_lock);

    if(conn_is_ours(conn)) {
        ESP_LOGW(TAG, "Закрываю fd=%d: %s", conn->fd, reason);
        web_server_session_close(conn->module->server, conn->fd);
    } else {
        ESP_LOGW(TAG, "fd=%d уже не наш (%s), только прекращаю отправку", conn->fd, reason);
    }
}

/* Дослать один кадр соединения. 1 — кадр ушёл целиком (можно взяться за
 * следующий), 0 — кадр ещё идёт или соединение закрыто. */
static uint8_t ws_conn_flush_once(ws_transport_module_t *m, ws_transport_conn_t *conn,
                                  uint32_t now, uint32_t dead_ticks) {
    /* Перед записью убеждаемся, что fd всё ещё эта WS-сессия: номер fd мог быть
     * переиспользован сокетом страницы, и запись в него портила бы HTTP-ответ
     * («Header fields are too long»). */
    if(!conn_is_ours(conn)) {
        /* fd уже не наш (закрыт или переиспользован): отправку прекращаем, а
         * недоданный кадр и режим выдачи гасим — иначе такт снова выдаст буфер
         * мёртвому соединению. tx_buf доедает conn_release/session_free_ctx. */
        platform_mutex_take(ws_frame_lock, PLATFORM_WAIT_FOREVER);
        uint32_t gen = conn->gen;
        uint8_t had = (conn->hdr_len || conn->tx_len || (conn->tx_mode != 0));
        const uint8_t *lost_ptr = (conn->tx_mode == 1) ? conn->tx_data : NULL;
        uint32_t lost_len = (conn->tx_mode == 1) ? conn->tx_len : 0;
        conn->tx_dead = 1;
        conn->hdr_len = 0;
        conn->hdr_off = 0;
        conn->tx_data = NULL;
        conn->tx_len  = 0;
        conn->tx_off  = 0;
        conn->tx_mode = 0;
        conn->out_pending = 0;
        platform_mutex_give(ws_frame_lock);
        if(had) {
            /* Издатель ждёт завершения кадра: без события его очередь стоит
             * (голова «в работе») до разрыва. */
            transport_event_trigger_t ev = { .event = TRANSPORT_TRANSFER_ERR };
            ev.data.ptr    = lost_ptr;
            ev.data.length = lost_len;
            notify_callback(conn, gen, &ev);
        }
        return 0;
    }

    /* Всё, что касается полей кадра, — под замком: begin_send пишет их из задачи
     * ядра API, а этот такт досылает параллельно (на двух ядрах). notify_event и
     * conn_close_session зовём БЕЗ замка — они возвращаются в ядро/веб-сервер и
     * могут заново войти в транспорт (begin_send). */
    const uint8_t *done_ptr;   /* для события завершения */
    uint32_t done_len;
    uint8_t do_complete = 0;    /* кадр ушёл целиком, режим 1 */
    uint8_t do_mode2_done = 0;  /* кадр ушёл целиком, режим 2 закончен */
    uint8_t do_offer = 0;       /* режимы 2/3: выдать следующую часть */
    uint8_t dead = 0;

    platform_mutex_take(ws_frame_lock, PLATFORM_WAIT_FOREVER);
    uint32_t gen = conn->gen;
    done_ptr = conn->tx_data;
    done_len = conn->tx_len;
    {
        uint32_t before = conn->hdr_off + conn->tx_off;
        int rc = out_flush(conn);
        if(rc < 0 || (((conn->hdr_off + conn->tx_off) == before) &&
                      ((uint32_t)(now - conn->out_since) > dead_ticks))) {
            dead = 1;
        } else if((conn->hdr_off + conn->tx_off) > before) {
            conn->out_since = now;          /* прогресс есть — отсчёт заново */
        }
    }
    if(!dead && (conn->hdr_len == 0) && (conn->tx_len == 0)) {
        /* Кадр ушёл целиком. В режиме zero-copy сообщаем издателю, что его блок
         * больше не нужен (по этому указателю он находит свой кадр). */
        if(conn->tx_mode == 1) {
            conn->tx_mode = 0;
            do_complete = 1;
        } else if((conn->tx_mode == 2) && (conn->tx_given >= conn->tx_expected)) {
            conn->tx_mode = 0;
            free(conn->tx_buf);
            conn->tx_buf = NULL;
            do_mode2_done = 1;
        } else if((conn->tx_mode == 2) || (conn->tx_mode == 3)) {
            do_offer = 1;                   /* следующая часть */
        }
    }
    platform_mutex_give(ws_frame_lock);

    if(dead) {
        conn_close_session(conn, "клиент не принимает кадр");
        return 0;
    }
    if(do_complete) {
        transport_event_trigger_t ev = { .event = TRANSPORT_TRANSFER_COMPLETE };
        ev.data.ptr    = done_ptr;
        ev.data.length = done_len;
        ev.data.left   = 0;
        notify_callback(conn, gen, &ev);
        return 1;
    }
    if(do_mode2_done) {
        transport_event_trigger_t ev = { .event = TRANSPORT_TRANSFER_COMPLETE };
        notify_callback(conn, gen, &ev);
        return 0;
    }
    if(do_offer) {
        tx_offer(conn);                     /* выдаёт буфер и сам notify_event'ит */
    }
    return 0;
}

/* Отправить ожидающий управляющий кадр (PONG/CLOSE), если он стоит в дескрипторе.
 * Только на границе кадров данных: текущий кадр ещё не начат (ни байта заголовка
 * и тела не ушло) — он может быть уже поставлен begin_send, это не мешает. Запись
 * частичная (сокет полон) — досылается со смещения на следующих тактах, а данные
 * этого соединения до конца управляющего кадра не стартуют (см. ws_out_pump):
 * иначе байты данных легли бы внутрь PONG/CLOSE («invalid opcode» на клиенте).
 * Возврат: CTRL_IDLE — данные можно слать, CTRL_BLOCKS — управляющий кадр ещё
 * не дописан и держит данные, CTRL_DEAD — сокет сломан или клиент не принимает
 * дольше dead_ticks (тот же детектор, что у данных: пока кадр держит данные,
 * ws_conn_flush_once не зовётся). */
#define CTRL_IDLE   0
#define CTRL_BLOCKS 1
#define CTRL_DEAD   2
static uint8_t ws_conn_ctrl_flush(ws_transport_conn_t *conn, uint32_t now, uint32_t dead_ticks) {
    if((conn == NULL) || (conn->fd < 0)) return CTRL_IDLE;
    uint8_t ours = conn_is_ours(conn);            /* fd мог уйти другому сокету */
    uint8_t dead = 0;
    platform_mutex_take(ws_frame_lock, PLATFORM_WAIT_FOREVER);
    uint8_t at_boundary = (conn->hdr_off == 0) && (conn->tx_off == 0);
    if(conn->ctrl_pending && !ours) {
        conn->ctrl_pending = 0;                   /* писать некуда */
        conn->ctrl_off     = 0;
        conn->close_after  = 0;
    } else if(conn->ctrl_pending && at_boundary) {
        int rc = web_server_write(conn->module->server, conn->fd,
                                  (const char *)conn->ctrl_buf + conn->ctrl_off,
                                  conn->ctrl_len - conn->ctrl_off, 0);
        if(rc < 0) {
            dead = 1;
        } else if(rc > 0) {
            conn->ctrl_off  += (uint32_t)rc;
            conn->ctrl_since = now;
        } else if((uint32_t)(now - conn->ctrl_since) > dead_ticks) {
            dead = 1;                             /* клиент не принимает */
        }
        if(!dead && (conn->ctrl_off >= conn->ctrl_len)) {
            conn->ctrl_pending = 0;
            conn->ctrl_off     = 0;
            if(conn->close_after) {               /* CLOSE ждал конца PONG */
                conn->close_after  = 0;
                conn->ctrl_len     = (uint32_t)ws_frame_header(conn->ctrl_buf, 0, 0x8);
                conn->ctrl_pending = 1;
                conn->ctrl_since   = now;
            }
        }
    }
    uint8_t blocks = conn->ctrl_pending && at_boundary;
    platform_mutex_give(ws_frame_lock);
    if(dead) return CTRL_DEAD;
    return blocks ? CTRL_BLOCKS : CTRL_IDLE;
}

static void ws_out_pump(ws_transport_module_t *m) {
    ws_transport_conn_t *list[WS_CONN_MAX];
    int n = conn_list_snapshot(m, list);
    uint32_t now = platform_tick();
    uint32_t dead_ticks = platform_ms_to_ticks(WS_OUT_DEAD_MS);

    for(int i = 0; i < n; i++) {
        ws_transport_conn_t *conn = list[i];
        if(!conn->busy || (conn->fd < 0)) continue;

        /* Управляющий кадр идёт впереди данных, но на границе кадров; пока он
         * не дописан, данные этого соединения ждут. */
        uint8_t ctrl = ws_conn_ctrl_flush(conn, platform_tick(), dead_ticks);
        if(ctrl == CTRL_DEAD) {
            conn_close_session(conn, "клиент не принимает управляющий кадр");
            continue;
        }
        if(ctrl == CTRL_BLOCKS) continue;

        for(int round = 0; round < WS_TX_FRAMES_PER_CYCLE; round++) {
            if(!conn->hdr_len && !conn->tx_len) {
                /* Нечего досылать: если издатель ждёт буфер — выдаём его. */
                if((conn->tx_mode == 2) || (conn->tx_mode == 3)) tx_offer(conn);
                break;
            }
            /* now — на каждый кадр заново: после TRANSFER_COMPLETE издатель может
             * тут же начать следующий кадр (begin_send), и его out_since окажется
             * свежее захваченного ранее now — беззнаковая разность переполнится и
             * «мёртвый» таймер сработает сразу (см. WS_TX_FRAMES_PER_CYCLE). */
            now = platform_tick();
            if(!ws_conn_flush_once(m, conn, now, dead_ticks)) break;
        }
    }
}

/* Операции соединения адресуются объекту, чей контекст — соединение. */
static ws_transport_conn_t *conn_of(const void *self) {
    const transport_t *tr = (const transport_t *)self;
    return (tr != NULL) ? (ws_transport_conn_t *)tr->context : NULL;
}

/* Подготовить экземпляр: веб-сервер уже в контексте объекта, путь придёт в open. */
static transport_result_t ws_httpd_init(const void *self, const void *config) {
    (void)config;
    ws_transport_module_t *m = mod_of(self);
    if(m == NULL) return TRANSPORT_BAD_CALL;
    if(m->inited) return TRANSPORT_BUSY;

    m->conn_lock = platform_mutex_create();
    if(m->conn_lock == NULL) return TRANSPORT_FAULT;
    for(size_t ci = 0; ci < WS_CONN_MAX; ci++) m->conns[ci] = NULL;
    m->recv_lock = platform_mutex_create();
    if(m->recv_lock == NULL) return TRANSPORT_FAULT;
    m->recv_head = m->recv_tail = NULL;
    m->recv_bytes = 0;
    m->server = srv_of(self);
    m->inited = 1;
    return TRANSPORT_OK;
}

/* Открыть экземпляр: config — путь WS-точки. Занять путь, отдать httpd колбэки,
 * запустить свой цикл. До этого вызова точка не работает. */
static transport_result_t ws_httpd_open(const void *self, const void *config) {
    ws_transport_module_t *m = mod_of(self);
    const char *uri = (const char *)config;
    if((m == NULL) || (uri == NULL) || !m->inited) return TRANSPORT_BAD_CALL;
    if(m->opened) return TRANSPORT_BUSY;

    m->uri = strdup(uri);
    if(m->uri == NULL) return TRANSPORT_FAULT;

    if(!web_server_claim_ws(m->server, m->uri)) {
        ESP_LOGE(TAG, "Путь '%s' не закреплён на интерфейсе %lu",
                 m->uri, (unsigned long)web_server_iface(m->server));
        return TRANSPORT_BAD_CALL;
    }
    web_server_transport_attach(m->server, (transport_t *)self);

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
    ws_cycle_start(m);
    LOGI(TAG, "Open, iface=%lu: WS-точка '%s' активна",
             (unsigned long)web_server_iface(m->server), m->uri);
    return TRANSPORT_OK;
}

static transport_result_t ws_httpd_subscribe(const void *self, delegate_t *delegate) {
    ws_transport_module_t *m = mod_of(self);
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
    ws_transport_conn_t *conn = conn_of(self);
    if(conn == NULL) return TRANSPORT_BAD_CALL;
    conn_close_session(conn, "закрытие из ядра");
    return TRANSPORT_OK;
}

/* Прервать начатую отправку: начатый кадр и выдача буфера бросаются, издатель
 * получает событие ошибки (может сгенерироваться TRANSFER_ERR). */
static transport_result_t ws_httpd_break_sending(const void *self) {
    ws_transport_conn_t *conn = conn_of(self);
    if(conn == NULL) return TRANSPORT_BAD_CALL;

    uint8_t had;
    const uint8_t *done_ptr;
    uint32_t done_len;

    platform_mutex_take(ws_frame_lock, PLATFORM_WAIT_FOREVER);
    uint32_t gen = conn->gen;
    had = (conn->hdr_len || conn->tx_len || (conn->tx_mode != 0));
    done_ptr = (conn->tx_mode == 1) ? conn->tx_data : NULL;
    done_len = (conn->tx_mode == 1) ? conn->tx_len : 0;

    conn->hdr_len = 0;
    conn->hdr_off = 0;
    conn->tx_data = NULL;
    conn->tx_len  = 0;
    conn->tx_off  = 0;
    conn->tx_mode = 0;
    conn->tx_given = 0;
    conn->out_pending = 0;
    free(conn->tx_buf);
    conn->tx_buf = NULL;
    platform_mutex_give(ws_frame_lock);

    if(had) {
        transport_event_trigger_t ev = { .event = TRANSPORT_TRANSFER_ERR };
        ev.data.ptr    = done_ptr;
        ev.data.length = done_len;
        notify_callback(conn, gen, &ev);
    }
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

/* Досылка начатого кадра. Возврат: 1 — кадр ушёл целиком, 0 — остаток ждёт места
 * в сокете, -1 — сокет сломан. Заголовок (если он отдельный) уходит первым. */
static int out_flush(ws_transport_conn_t *conn) {
    if((conn->hdr_off >= conn->hdr_len) && (conn->tx_off >= conn->tx_len)) return 1;
    /* Ожидания нет: остаток досылает такт транспорта, а ждать в такте нельзя —
     * задержка одного нечитающего клиента останавливает приём от всех. */
    for (int i = 0; i < 32; i++) {
        const char *p;
        uint32_t left;
        if(conn->hdr_off < conn->hdr_len) {
            p    = (const char *)conn->hdr + conn->hdr_off;
            left = conn->hdr_len - conn->hdr_off;
        } else {
            p    = (const char *)conn->tx_data + conn->tx_off;
            left = conn->tx_len - conn->tx_off;
        }
        int rc = web_server_write(conn->module->server, conn->fd, p, left, 0);
        if(rc < 0) return -1;
        if(rc == 0) break;                  /* места в сокете больше нет */
        if(conn->hdr_off < conn->hdr_len) conn->hdr_off += (uint32_t)rc;
        else                              conn->tx_off  += (uint32_t)rc;
        if((conn->hdr_off >= conn->hdr_len) && (conn->tx_off >= conn->tx_len)) break;
    }
    if((conn->hdr_off < conn->hdr_len) || (conn->tx_off < conn->tx_len)) return 0;
    conn->hdr_len = 0;
    conn->hdr_off = 0;
    conn->tx_data = NULL;
    conn->tx_len  = 0;
    conn->tx_off  = 0;
    conn->out_pending = 0;
    return 1;
}

/* Начать кадр из ЧУЖОГО блока (режим zero-copy): данные не копируются, а живут у
 * издателя до TRANSFER_COMPLETE. Заголовок — свой маленький буфер. */
static void tx_start_borrowed(ws_transport_conn_t *conn, const uint8_t *data, uint32_t length) {
    conn->hdr_len = (uint32_t)ws_frame_header(conn->hdr, length, 0x1);   /* 0x1 — text */
    conn->hdr_off = 0;
    conn->tx_data = data;
    conn->tx_len  = length;
    conn->tx_off  = 0;
    conn->out_since   = platform_tick();
    conn->out_pending = 1;
}

/* Закончить кадр, который издатель заполнил в нашем буфере: заголовок встаёт в
 * запас перед данными, кадр уходит ОДНИМ непрерывным блоком. */
static void tx_start_own(ws_transport_conn_t *conn, uint32_t fill) {
    int hlen = ws_frame_header(conn->tx_buf, fill, 0x1);
    conn->hdr_len = 0;
    conn->hdr_off = 0;
    conn->tx_data = conn->tx_buf;
    conn->tx_len  = (uint32_t)hlen + fill;
    conn->tx_off  = 0;
    conn->out_since   = platform_tick();
    conn->out_pending = 1;
}

/* Выдать издателю буфер под заполнение (режимы 2 и 3). Размер — сколько он ещё
 * обещал (в потоке — сколько влезает). */
static void tx_offer(ws_transport_conn_t *conn) {
    uint32_t cap = WS_OUT_MAX;
    if(conn->tx_mode == 2) {
        uint32_t left = conn->tx_expected - conn->tx_given;
        if(left == 0) {
            conn->tx_mode = 0;
            return;
        }
        if(left < cap) cap = left;
    }
    if(conn->tx_buf == NULL) {
        conn->tx_buf = malloc(WS_FRAME_HEADROOM + WS_OUT_MAX);
        if(conn->tx_buf == NULL) {
            ESP_LOGW(TAG, "Нет памяти под буфер отправки, fd=%d", conn->fd);
            conn->tx_mode = 0;
            return;
        }
    }

    transport_event_trigger_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.event        = TRANSPORT_TRANSFER_BUFFER;
    ev.buffer.ptr   = conn->tx_buf + WS_FRAME_HEADROOM;   /* смещённый указатель */
    ev.buffer.size  = cap;
    ev.buffer.fill  = 0;
    /* Издатель заполняет буфер ПРЯМО ЗДЕСЬ и выставляет fill — по этому полю
     * транспорт и узнаёт, сколько данных он положил. */
    notify_event(conn, &ev);

    uint32_t fill = ev.buffer.fill;
    if(fill > cap) fill = cap;
    if(fill == 0) {                        /* заполнять больше нечего — конец */
        free(conn->tx_buf);
        conn->tx_buf  = NULL;
        conn->tx_mode = 0;
        transport_event_trigger_t done = { .event = TRANSPORT_TRANSFER_COMPLETE };
        notify_event(conn, &done);
        return;
    }
    conn->tx_given += fill;
    tx_start_own(conn, fill);
}

/* Три режима контракта (см. transport.h):
 *   data != NULL            — zero-copy: блок издателя живёт до TRANSFER_COMPLETE;
 *   data == NULL, length    — транспорт выдаёт буфер под заполнение событием
 *                             TRANSFER_BUFFER (может быть несколько частей);
 *   data == NULL, STREAM_LENGTH — поток таких выдач до break_sending. */
static transport_result_t ws_httpd_begin_send(const void *self, const void *data, uint32_t length) {
    ws_transport_conn_t *conn = conn_of(self);
    if(conn == NULL) return TRANSPORT_BAD_CALL;
    /* Соединение закрыто: писать некуда. */
    if(conn->fd < 0) return TRANSPORT_BAD_CALL;

    if(data != NULL) {
        if(length == 0 || length > WS_OUT_MAX) return TRANSPORT_BAD_CALL;
        /* Предыдущий кадр ещё не ушёл или клиент не принимает — пусть издатель
         * повторит тактом. Проверка и старт кадра — под замком: иначе такт
         * транспорта прочитает кадр наполовину записанным. */
        platform_mutex_take(ws_frame_lock, PLATFORM_WAIT_FOREVER);
        if(conn->tx_dead || conn->hdr_len || conn->tx_len || (conn->tx_mode != 0)) {
            conn->out_dropped++;
            platform_mutex_give(ws_frame_lock);
            return TRANSPORT_BUSY;
        }
        conn->tx_mode = 1;
        tx_start_borrowed(conn, (const uint8_t *)data, length);
        platform_mutex_give(ws_frame_lock);
        return TRANSPORT_OK;
    }

    platform_mutex_take(ws_frame_lock, PLATFORM_WAIT_FOREVER);
    if(conn->hdr_len || conn->tx_len || (conn->tx_mode != 0)) {
        platform_mutex_give(ws_frame_lock);
        return TRANSPORT_BUSY;
    }
    conn->tx_mode     = (length == TRANSPORT_STREAM_LENGTH) ? 3 : 2;
    conn->tx_expected = (length == TRANSPORT_STREAM_LENGTH) ? 0 : length;
    conn->tx_given    = 0;
    platform_mutex_give(ws_frame_lock);
    /* Буфер выдаём сразу: издатель обязан быть готов заполнить его прямо в
     * обработчике события TRANSFER_BUFFER. */
    tx_offer(conn);
    return TRANSPORT_OK;
}

/* Таблица операций одна на все экземпляры: реализация одна, а адресат и путь
 * приходят в объекте. */
static const transport_iface_t ws_iface = {
    .init          = ws_httpd_init,
    .open          = ws_httpd_open,
    .close         = ws_httpd_close,
    .begin_send    = ws_httpd_begin_send,
    .break_sending = ws_httpd_break_sending,
    .subscribe     = ws_httpd_subscribe,
};

/* ── Объект транспорта ──────────────────────────────────────────────────────
 *
 * Наружу транспорт отдаёт ОБЪЕКТ: методы и свой контекст (transport_t). Объект
 * порождается вместе с экземпляром и освобождается вместе с ним
 * (ws_transport_destroy): экземпляр существует только потому, что его породил
 * объект веб-сервера интерфейса. */

/* --- Приём ---------------------------------------------------------------- */

/* Запас места под ещё len байт. NULL — сообщение ЦЕЛИКОМ вышло бы за предел
 * (не один кадр: поток продолжений иначе растит буфер до исчерпания кучи) или
 * нет памяти: тогда соединение закрываем, принимать его негде. */
static uint8_t *msg_reserve(ws_transport_conn_t *conn, uint32_t len) {
    return ws_msg_reserve(&conn->msg, len, TRANSPORT_MESSAGE_MAX_LEN);
}

/* Отбросить недособранное сообщение. */
static void drop_message(ws_transport_conn_t *conn) {
    ws_msg_free(&conn->msg);
}

/* Предел того, что копится непринятым: очередь разбирают на каждом такте, но
 * одно сообщение бывает крупным — порция заливки прошивки приходит base64. */
#define WS_RECV_QUEUE_MAX     16
#define WS_RECV_QUEUE_BYTES    (32u * 1024u)

/* Собранное сообщение — в очередь, а не сразу подписчику: разбор идёт в задаче
 * приложения, и ядро фрагментов не видит. Копия нужна, потому что буфер
 * соединения переиспользуется следующим кадром. */
static void deliver_message(ws_transport_conn_t *conn) {
    uint32_t msg_len = ws_msg_len(conn->msg);
    if(msg_len == 0) {
        /* Пустое сообщение ядру не нужно. */
        drop_message(conn);
        return;
    }

    ws_transport_module_t *m = conn->module;
    /* recv_queued правит и такт (уменьшает, под замком событий): проверка и
     * резерв места — под тем же замком. Резерв снимается, если записи не будет. */
    platform_mutex_take(ws_event_lock, PLATFORM_WAIT_FOREVER);
    uint8_t full = (m->recv_lock == NULL) ||
                   (conn->recv_queued >= WS_RECV_QUEUE_MAX) ||
                   (m->recv_bytes + msg_len > WS_RECV_QUEUE_BYTES);
    if(!full) conn->recv_queued++;
    platform_mutex_give(ws_event_lock);
    if(full) {
        /* Разбирать негде или копится больше предела: закрываем соединение —
         * молча терять сообщения клиента нельзя, он ждёт ответа. */
        ESP_LOGW(TAG, "Очередь приёма заполнена (%lu Б в ней), закрываю fd=%d",
                 (unsigned long)m->recv_bytes, conn->fd);
        drop_message(conn);
        web_server_session_close(m->server, conn->fd);
        return;
    }

    ws_transport_recv_t *rec = malloc(sizeof(*rec) + msg_len + 1);
    if(rec == NULL) {
        ESP_LOGW(TAG, "Нет памяти под принятое сообщение (%lu Б), закрываю fd=%d",
                 (unsigned long)msg_len, conn->fd);
        platform_mutex_take(ws_event_lock, PLATFORM_WAIT_FOREVER);
        conn->recv_queued--;                      /* резерв не понадобился */
        platform_mutex_give(ws_event_lock);
        drop_message(conn);
        web_server_session_close(m->server, conn->fd);
        return;
    }
    rec->next = NULL;
    rec->conn = conn;
    rec->gen  = conn->gen;
    rec->len  = msg_len;
    memcpy(rec->data, ws_msg_data(conn->msg), msg_len);
    rec->data[msg_len] = '\0';   /* ядру удобно видеть строку в логе */

    platform_mutex_take(m->recv_lock, PLATFORM_WAIT_FOREVER);
    if(m->recv_tail != NULL) m->recv_tail->next = rec;
    else                     m->recv_head = rec;
    m->recv_tail   = rec;
    m->recv_bytes += rec->len + 1;
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
        /* На PING отвечаем PONG, на CLOSE — CLOSE (RFC 6455 §5.5.2, §5.5.1).
         *
         * Ответ НЕ пишем отсюда: это задача httpd, а кадры данных досылает такт
         * транспорта, и параллельная запись штатным путём httpd (send-оверрайд,
         * httpd_ws_send_frame_async) вклинивается между уже записанным заголовком
         * и телом идущего кадра данных — поток на клиенте рассыпается
         * («invalid opcode»). Поэтому кадр кладём в дескриптор, а отправит его
         * ТАКТ транспорта под ws_frame_lock, в очередь с кадрами данных. */
        uint8_t opcode = (f->type == HTTPD_WS_TYPE_PING) ? 0xA : 0x8;   /* PONG/CLOSE */
        uint8_t frame[sizeof(conn->ctrl_buf)];
        int hlen = ws_frame_header(frame, f->len, opcode);
        if(f->len) memcpy(frame + hlen, payload, f->len);

        /* В дескриптор — под замком и только если предыдущий управляющий кадр
         * не пишется прямо сейчас (такт досылает его из ctrl_buf): начатый кадр
         * дописывается, а ответ на новый PING можно и не давать (RFC 6455
         * §5.5.3 — достаточно ответа на последний). */
        platform_mutex_take(ws_frame_lock, PLATFORM_WAIT_FOREVER);
        if((opcode == 0x8) && !conn->ctrl_pending &&
           (conn->hdr_off == 0) && (conn->tx_off == 0)) {
            /* CLOSE: httpd сам помечает сессию закрытой (ws_close) и снимет её
             * сразу после этого обработчика — ответ из такта не успевал бы.
             * Пишем здесь: под замком кадра такт не пишет, а кадр данных не
             * начат — вклиниться не во что. Не ушёл целиком — не беда, сессия
             * всё равно закрывается. */
            web_server_write(conn->module->server, fd, (const char *)frame,
                             (size_t)hlen + f->len, 0);
        } else if(!(conn->ctrl_pending && conn->ctrl_off)) {
            memcpy(conn->ctrl_buf, frame, (size_t)hlen + f->len);
            conn->ctrl_len     = (uint32_t)hlen + f->len;
            conn->ctrl_off     = 0;
            conn->ctrl_pending = 1;
            conn->ctrl_since   = platform_tick();
        } else if(opcode == 0x8) {
            /* CLOSE отвечать обязательно (§5.5.1): уйдёт сразу за начатым
             * кадром — без тела, кода статуса в ответе может и не быть. */
            conn->close_after = 1;
        }
        platform_mutex_give(ws_frame_lock);
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
        ws_msg_commit(conn->msg, f.len);
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
    ws_msg_commit(conn->msg, f.len);
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

    /* Запоминаем адрес КЛИЕНТА: по нему потом видно, что fd всё ещё наш. */
    {
        struct sockaddr_in sa;
        socklen_t len = sizeof(sa);
        memset(&sa, 0, sizeof(sa));
        if(getpeername(fd, (struct sockaddr *)&sa, &len) == 0) {
            conn->peer_ip   = sa.sin_addr.s_addr;
            conn->peer_port = sa.sin_port;
        }
    }

    /* TCP_NODELAY ставит сокетный слой на приёме сессии (web_httpd.c,
     * port_session_open): повторять здесь не нужно. */

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

/* Заполнить объект транспорта: методы и контекст — веб-сервер интерфейса.
 * Всё остальное (путь, активация, цикл) — open. */
transport_t *ws_transport_create(web_server_t *server) {
    if(server == NULL) return NULL;

    /* Пул дескрипторов общий на все экземпляры: замок заводится один раз. */
    if(ws_pool_lock == NULL) ws_pool_lock = platform_mutex_create();
    if(ws_pool_lock == NULL) return NULL;
    if(ws_frame_lock == NULL) ws_frame_lock = platform_mutex_create();
    if(ws_frame_lock == NULL) return NULL;
    if(ws_event_lock == NULL) ws_event_lock = platform_mutex_create();
    if(ws_event_lock == NULL) return NULL;

    transport_t *transport = malloc(sizeof(*transport));
    if(transport == NULL) return NULL;
    transport->iface   = &ws_iface;
    transport->context = server;

    if(ws_iface.init(transport, NULL) != TRANSPORT_OK) {
        free(transport);
        return NULL;
    }
    return transport;
}

void ws_transport_destroy(transport_t *transport) {
    if(transport == NULL) return;
    ws_transport_module_t *m = mod_of(transport);
    if(m != NULL) {
        /* Сначала останавливаем свой такт: иначе он трогает освобождённый
         * экземпляр. */
        ws_cycle_stop(m);
        free(m->uri);
        m->uri     = NULL;
        m->server  = NULL;
        m->inited  = 0;
        m->opened  = 0;
    }
    free(transport);
}
