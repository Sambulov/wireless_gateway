/* ws_transport.c — транспорт на WebSocket как надстройка над веб-сервером:
 * экземпляр порождает объект веб-сервера интерфейса, он же назначает ему URI и
 * отдаёт соединения и кадры. Транспорт собирает сообщения из кадров и поднимает
 * события своему подписчику (ядру API). */

#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "CodeLib.h"
#include "connection.h"
/* api.h (контракт ядра) транспорту не нужен и не включается: транспорт знает
 * только свой объект (transport.h) и подписчика, которого ему дали в
 * subscribe(). */
#include "transport.h"
/* Свой httpd транспорт не поднимает: точка живёт в веб-сервере интерфейса, и с
 * ним же он работает — сессию закрывает и кадры отправляет через его методы. */
#include "ws_transport.h"
#include "platform.h"

static const char *TAG = "ws_transport";

/* esp_http_server answers the WebSocket handshake itself since ESP-IDF v6 and
 * returns before invoking the URI handler ("If the request is websocket
 * handshake, then do not call the uri->handler", httpd_uri.c). The handshake
 * request is handed to ws_post_handshake_cb instead, so the per-session
 * conn object is allocated from that callback. For older IDF versions the
 * handler is still called with method == HTTP_GET, and the allocation happens
 * there.
 *
 * Рукопожатие приходит через объект веб-сервера (main/web_httpd.c оборачивает
 * этот этап так же, как обычный запрос), поэтому контекст экземпляра в
 * req->user_ctx здесь тот же, что и у кадров. */
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
#define TRANSPORT_FRAME_MAX_LEN   16384
#define TRANSPORT_MESSAGE_MAX_LEN 16384

/* Управляющий кадр WebSocket: payload не длиннее 125 байт (RFC 6455 §5.5). */
#define WS_CONTROL_PAYLOAD_MAX_LEN   125

/* Экземпляр транспорта: свой веб-сервер, свой URI, своё ядро, свои соединения.
 * Объявлен заранее — дескриптор соединения знает, чей он. */
typedef struct ws_transport_module_s ws_transport_module_t;

/* Сколько соединений экземпляр держит в списке (см. WS_CONN_MAX у модуля). */
#define WS_CONN_MAX   8


/* Дескриптор соединения транспорта. Ядру он непрозрачен: ядро использует его
 * как ключ своей сессии и не знает ни этого типа, ни веб-сервера. */
typedef struct {
    int fd;
    delegate_t delegate;
    /* Чей это экземпляр транспорта. Известен с момента установления соединения:
     * его передал сам веб-сервер как контекст обработчика, потому что запрос
     * пришёл в точку ЭТОГО экземпляра. По нему же находятся подписчик (ядро),
     * его обработчик событий и веб-сервер, которому принадлежит сессия. Второго
     * «ядра вообще» у транспорта нет. */
    ws_transport_module_t *module;
    /* Сборка СООБЩЕНИЯ из фрагментов: WebSocket разрешает резать одно сообщение
     * на кадры (первый с FIN=0, дальше продолжения). Буфер принадлежит
     * транспорту и живёт ровно до возврата из обработчика события RECEIVED. */
    uint8_t *msg;
    uint32_t msg_len;
    uint32_t msg_cap;
    /* Сколько отправок сейчас в работе. Дескриптор нельзя освободить, пока
     * хоть одна из них не завершилась: колбэк веб-сервера приходит позже
     * закрытия сессии и получает указатель на него. */
    uint32_t sends_in_flight;
    /* Сессия ядра уже закрыта (CLOSED отправлен), но отправки ещё в работе:
     * память дескриптора освободит последний колбэк. */
    uint8_t  detached;
    /* Колбэк отправки сейчас уведомляет ядро. Ядро вправе закрыть соединение
     * прямо из этого уведомления (отказ отправки), и тогда обработчик закрытия
     * НЕ освобождает дескриптор: он ещё нужен колбэку, который его вызвал. */
    uint8_t  in_callback;
    /* Сколько принятых сообщений этого соединения ждёт доставки (см. очередь
     * recv). Пока счётчик не ноль, дескриптор не освобождается: записи очереди
     * на него ссылаются. */
    uint16_t recv_queued;
    /* Начатый кадр, который клиент ещё не принял целиком: своя копия заголовка и
     * данных. Сокет отдаёт данные порциями (send с MSG_DONTWAIT пишет сколько
     * влезло), и кадр нельзя ни бросить на середине, ни смешать со следующим —
     * поэтому остаток живёт здесь, а досылает его такт транспорта (poll).
     * Пока буфер занят, новые кадры этому клиенту отбрасываются: он не
     * принимает, и копить их незачем. */
    uint8_t *out;
    uint32_t out_len;
    uint32_t out_off;
    uint32_t out_since;      /* когда кадр начат — для решения «клиент пропал» */
    uint8_t  out_pending;    /* в буфере есть недописанное */
    uint16_t out_dropped;    /* сколько кадров отброшено за время занятости */
} ws_transport_conn_t;

/* Предел отложенного кадра: столько транспорт держит на соединение, пока клиент
 * не принимает. Кадр потока — до ~2,7 КБ base64 (накопитель 2048), ответ вызова —
 * сотни байт; 8 КБ покрывают всё с запасом. Больше — отбрасываем кадр: копить
 * клиенту, который не читает, незачем. */
#define WS_OUT_MAX   8192u
/* Сколько начатый кадр может висеть без прогресса, прежде чем считать клиента
 * пропавшим и закрыть сеанс. */
#define WS_OUT_DEAD_MS   20000u

/* Принятое сообщение, ждущее доставки ядру. Копия, а не указатель на буфер
 * соединения: за время ожидания в то же соединение может прийти следующее
 * сообщение, и буфер будет переиспользован. */
typedef struct ws_transport_recv_s {
    struct ws_transport_recv_s *next;
    ws_transport_conn_t *conn;   /* чьё соединение: дескриптор живёт, пока жива
                                  * запись (см. recv_queued в соединении) */
    uint32_t len;
    uint8_t  data[];             /* само сообщение + '\0' в конце */
} ws_transport_recv_t;

/* ── Экземпляр транспорта ───────────────────────────────────────────────────
 *
 * Экземпляр (в контракте — transport_t.context) — это ровно то, что транспорт
 * знает о себе: веб-сервер, который его породил, свой URI, свою подписку
 * (event_t с делегатом ядра) и свои соединения. Номера сетевого интерфейса здесь
 * нет: интерфейс задан объектом веб-сервера, и если он понадобится (для
 * журнала), его спрашивают у сервера — второго источника правды не заводится.
 *
 * Своего httpd у транспорта нет: WS-точка — надстройка над веб-сервером
 * интерфейса, и работать с соединениями он может только через него. Экземпляр
 * обслуживает ту точку, на которую его породили: соединение приходит в
 * обработчик с контекстом СВОЕГО экземпляра (web_server_activate_ws), поэтому
 * «чей это клиент» не выясняется ни по сокету, ни по таблице.
 *
 * Поле on_event стоит ПЕРВЫМ: внутри event_t — элемент связного списка
 * (указатели), и ему нужен адрес под указатель. Экземпляр создаётся calloc'ом,
 * то есть выровнен достаточно для любого своего поля.
 */
struct ws_transport_module_s {
    /* Подписка экземпляра — делегат в общем с остальной прошивкой смысле
     * (CL/Workflow/Event.h, как у UART и у линии связи): соединения, принятые
     * ЭТИМ экземпляром, уходят его подписчику событиями event_raise. Список
     * подписчиков модуль не ведёт — он лежит в самом event_t, поэтому «подписан
     * ли кто-нибудь» видно по результату event_raise, а отписка (свойство
     * делегата, а не операция транспорта) не требует от модуля ничего. */
    event_t   on_event;
    /* Веб-сервер своего интерфейса и путь WS-точки: его дал тот, кто породил
     * экземпляр (web_server_open_websocket). Через сервер экземпляр
     * активирует путь, закрывает соединения и отправляет кадры. */
    web_server_t *server;
    char         *uri;
    uint8_t   inited;
    uint8_t   opened;
    /* Отправки, которые транспорт принял, но о завершении которых ещё не
     * уведомлён. Список нужен, чтобы находить ЗАВИСШИЕ отправки: httpd ставит
     * задание в свою очередь датаграммой по UDP, и под нагрузкой она теряется —
     * колбэк тогда не приходит НИКОГДА, а буферы (наш контекст отправки и блок
     * ответа ядра) остаются занятыми навсегда. Замерено на стенде: восемьдесят
     * таких отправок = −23 КБ кучи без возврата, через несколько минут прибор
     * падал от нехватки памяти. */
    /* Соединения экземпляра: нужны, чтобы такт транспорта досылал начатые кадры.
     * Список фиксированный — соединений не больше, чем сессий у веб-сервера. */
    platform_mutex_t conn_lock;
    ws_transport_conn_t *conns[WS_CONN_MAX];
    /* Принятые, но ещё не разобранные сообщения: их доставляет poll в задаче
     * приложения (см. ws_transport_poll). Задача веб-сервера только копирует
     * принятое сюда — разбор сообщения ей не по стеку. */
    platform_mutex_t recv_lock;
    ws_transport_recv_t *recv_head;
    ws_transport_recv_t *recv_tail;
    uint32_t recv_bytes;
};

/* Настройки экземпляра: путь WS-точки и объект веб-сервера, который его
 * породил. */
typedef struct {
    const char   *uri;       /* путь WebSocket-точки */
    web_server_t *server;    /* объект веб-сервера своего интерфейса */
} ws_transport_config_t;

/* Обработчик URI и открытие соединения описаны ниже: на них ссылается таблица
 * операций (ws_iface) и колбэки WS-точки. */
static esp_err_t ws_httpd_handler(httpd_req_t *req);
#if WS_CONN_FROM_POST_HANDSHAKE_CB
static esp_err_t ws_httpd_post_handshake(httpd_req_t *req);
#endif
static ws_transport_conn_t *conn_open(httpd_req_t *req, int fd);

/* --- Служебное ------------------------------------------------------------ */

static uint32_t notify_event(ws_transport_conn_t *conn, const transport_event_trigger_t *ev) {
    /* Подписчик — у экземпляра, принявшего ЭТО соединение: делегат, который ядро
     * отдало в subscribe(). Транспорт в контекст делегата не заглядывает, и
     * чужое ядро событие не получает. Возврат — сколько подписчиков событие
     * получило: ноль значит, что соединение обслуживать некому (ядро не
     * подписалось или уже сняло подписку). */
    ws_transport_module_t *m = (conn != NULL) ? conn->module : NULL;
    if(m == NULL) return 0;
    return event_raise(&m->on_event, conn, (void *)ev);
}

/* Список соединений экземпляра: такт транспорта досылает по нему начатые кадры.
 * Список нужен именно для этого — других причин знать все соединения у
 * транспорта нет. */
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

/* Снимок списка: досылка идёт без мьютекса (запись в сокет — не критическая
 * секция), поэтому сначала копируем указатели. */
static int conn_list_snapshot(ws_transport_module_t *m, ws_transport_conn_t **out) {
    int n = 0;
    if((m == NULL) || (m->conn_lock == NULL)) return 0;
    platform_mutex_take(m->conn_lock, PLATFORM_WAIT_FOREVER);
    for(size_t i = 0; i < WS_CONN_MAX; i++)
        if(m->conns[i] != NULL) out[n++] = m->conns[i];
    platform_mutex_give(m->conn_lock);
    return n;
}

/* free_ctx у httpd принимает void*, поэтому обёртка: внутри — своя логика на
 * типизированном дескрипторе. */
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
    /* Дескриптор сокета больше не наш: httpd мог отдать его новому соединению.
     * Запоминаем это в дескрипторе, чтобы позднее закрытие (отказ отправки или
     * ядро, закрывающее мёртвое соединение) не закрыло ЧУЖОЙ сокет. */
    conn->fd = -1;
    free(conn->msg);
    conn->msg    = NULL;
    conn->msg_len  = 0;
    conn->msg_cap  = 0;

    /* Ядру сообщаем о разрыве: свою сессию оно освободит по этому событию.
     * Дескриптор при этом ещё жив: ядро могло закрыть соединение само. */
    transport_event_trigger_t ev = { .event = TRANSPORT_CLOSED };
    notify_event(conn, &ev);

    if(conn->sends_in_flight || conn->in_callback || conn->recv_queued) {
        /* Отправки ещё в работе и получат этот указатель в колбэке — освободит
         * память последний из них (см. ws_httpd_send_done). Колбэк, уведомляющий
         * ядро прямо сейчас, тоже ещё пользуется дескриптором. */
        conn->detached = 1;
        return;
    }
    free(conn);
}

static void link_event_handler(void *event_trigger, void *sender, void *context) {
    (void)event_trigger; (void)sender;
    ws_transport_conn_t *conn = (ws_transport_conn_t *)context;
    ESP_LOGI(TAG, "Link down, triggering close for fd=%d", conn->fd);
    web_server_session_close(conn->module->server, conn->fd);
}

/* --- Операции транспорта как модуля --------------------------------------- */

/* Подготовка экземпляра: запомнить, куда он пойдёт — путь WS-точки и объект
 * веб-сервера, который его породил. Зовётся один раз, из ws_transport_create;
 * URI активирует open, и только после того, как подписчик назначен (см.
 * conn_open). */

/* Доставка принятого подписчику: это и есть «такт транспорта» (transport_iface_t
 * poll). Зовётся из задачи приложения на каждом её такте — там, где уже
 * работают обработчики сервисов и где стек 8 КБ, а не 4, как у веб-сервера.
 *
 * Порядок сохраняется (очередь FIFO): ядро получает сообщения клиента в том
 * порядке, в каком они пришли. */
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
        transport_event_trigger_t ev = { .event = TRANSPORT_RECEIVED };
        ev.data.ptr    = rec->data;
        ev.data.length = rec->len;
        ev.data.left   = 0;                   /* сообщение отдано целиком */
        notify_event(conn, &ev);

        /* Дескриптор ждал этого сообщения: теперь его можно освободить, если
         * сессия уже закрыта и отправок не осталось. */
        if(conn != NULL && conn->recv_queued) conn->recv_queued--;
        uint8_t release = (conn != NULL) && conn->detached && !conn->sends_in_flight
                          && !conn->recv_queued && !conn->in_callback;
        free(rec);
        if(release) free(conn);
    }
}

/* Досылка начатых кадров: клиент, который снова начал читать, получает остаток, а
 * не копит потери. Клиента, у которого кадр не двигается дольше WS_OUT_DEAD_MS,
 * считаем пропавшим и закрываем сеанс — иначе буфер и счётчики висят вечно. */
static void ws_out_pump(ws_transport_module_t *m) {
    ws_transport_conn_t *list[WS_CONN_MAX];
    int n = conn_list_snapshot(m, list);
    uint32_t now = platform_tick();
    uint32_t dead_ticks = platform_ms_to_ticks(WS_OUT_DEAD_MS);

    for(int i = 0; i < n; i++) {
        ws_transport_conn_t *conn = list[i];
        if(conn->out == NULL) continue;
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

    /* Путь — своя копия: он живёт столько же, сколько экземпляр, а не столько,
     * сколько строка у вызывающего. */
    m->uri = strdup(cfg->uri);
    if(m->uri == NULL) return TRANSPORT_FAULT;
    m->conn_lock = platform_mutex_create();
    for(size_t ci = 0; ci < WS_CONN_MAX; ci++) m->conns[ci] = NULL;
    m->recv_lock = platform_mutex_create();
    m->recv_head = m->recv_tail = NULL;
    m->recv_bytes = 0;
    m->server = cfg->server;
    m->inited = 1;
    ESP_LOGI(TAG, "Init, iface=%lu, uri=%s",
             (unsigned long)web_server_iface(m->server), m->uri);
    return TRANSPORT_OK;
}

/* «Начать слушать». Своего httpd у транспорта нет: соединения принимает
 * веб-сервер, поэтому open означает «активировать назначенный экземпляру URI»:
 * путь попадает в httpd, и с этого момента веб-сервер отдаёт экземпляру
 * соединения и кадры. Раньше этого вызова точка не работает — так соединение,
 * пришедшее до подписки ядра, не попадает в «висящую» сессию, а не находит
 * пути. */
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
        /* Управляющие кадры (PING/PONG/CLOSE) забирает обработчик транспорта — с
         * этим флагом веб-сервер отдаёт их нам, а не разбирает сам. Ядру API они
         * не приходят вовсе (см. ws_httpd_control_frame). */
        .handle_control_frames = 1,
    };
    /* Контекст колбэков — сам экземпляр: по нему обработчик узнаёт, чей это
     * вызов. Другого способа отличить экземпляры нет и не нужно: таблицы
     * «какой URI какому транспорту» не существует. */
    if(!web_server_activate_ws(m->server, m->uri, &callbacks, m)) {
        ESP_LOGE(TAG, "WS-точка '%s' не активирована на интерфейсе %lu",
                 m->uri, (unsigned long)web_server_iface(m->server));
        return TRANSPORT_BAD_CALL;
    }

    m->opened = 1;
    ESP_LOGI(TAG, "Open, iface=%lu: WS-точка '%s' активна",
             (unsigned long)web_server_iface(m->server), m->uri);
    return TRANSPORT_OK;
}

static transport_result_t ws_httpd_subscribe(const void *self, delegate_t *delegate) {
    ws_transport_module_t *m = (ws_transport_module_t *)self;
    if((m == NULL) || (delegate == NULL) || (delegate->handler == NULL))
        return TRANSPORT_BAD_CALL;
    /* Подписчик у каждого экземпляра свой — своё ядро API. Обработчик и его
     * контекст живут в делегате: модуль лишь кладёт его в свой event_t и больше
     * про подписчика ничего не знает. Отписки здесь нет и не будет: снимает
     * подписку сам подписчик (event_unsubscribe), а транспорт просто перестаёт
     * поднимать для него события. */
    event_subscribe(&m->on_event, delegate);
    ESP_LOGI(TAG, "Subscribed");
    return TRANSPORT_OK;
}

/* --- Операции соединения -------------------------------------------------- */

static transport_result_t ws_httpd_close(const void *self) {
    ws_transport_conn_t *conn = (ws_transport_conn_t *)self;
    if(conn == NULL) return TRANSPORT_BAD_CALL;
    /* Сессия уже закрыта (сессия ядра снимается раньше, чем ядро узнаёт о
     * завершении отправок): закрывать нечего, и дескриптора у нас нет — он мог
     * достаться другому соединению. */
    if(conn->fd < 0) return TRANSPORT_OK;
    if(!web_server_session_close(conn->module->server, conn->fd)) {
        ESP_LOGW(TAG, "Close fd=%d failed", conn->fd);
        return TRANSPORT_BAD_CALL;   /* сессии уже нет */
    }
    return TRANSPORT_OK;
}

/* Отправка идёт одним куском: сообщение целиком уходит в веб-сервер, он сам
 * строит WS-кадр и сам режет его на TCP-сегменты. Прерывать нечего. */
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
    /* Ожидания здесь нет: остаток живёт в нашем буфере, и его досылает такт
     * транспорта. Ждать в такте нельзя — задержка одного нечитающего клиента
     * останавливает приём сообщений от всех. */
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

/* Кадр целиком: голова и данные кладутся в ОДИН буфер, поэтому кадр либо есть у
 * клиента весь, либо его нет вовсе — смешать половину с чужим кадром нельзя.
 * Возврат: 1 — принят (ушёл или ждёт в буфере), 0 — отброшен, -1 — сокет сломан. */
static int frame_queue(ws_transport_conn_t *conn, const void *data, uint32_t length) {
    if(conn->out != NULL) {
        /* Предыдущий кадр ещё не ушёл: этот клиент не принимает — свежий кадр
         * отбрасываем, а не копим (поток с линии lossy по своей природе). */
        conn->out_dropped++;
        return 0;
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
    /* Сокет сломан: закрываем соединение (ядро узнает отказом ниже). */
    return -1;
}

static transport_result_t ws_httpd_begin_send(const void *self, const void *data, uint32_t length) {
    ws_transport_conn_t *conn = (ws_transport_conn_t *)self;
    if((conn == NULL) || (data == NULL) || (length == 0)) return TRANSPORT_BAD_CALL;
    /* Соединение уже закрыто: писать некуда, дескриптора у нас нет. */
    if(conn->fd < 0) return TRANSPORT_BAD_CALL;

    /* Дескриптор нужен уведомлению: обработчик закрытия сессии его не освободит
     * (in_callback), а мы освободим сами, если сессия уже закрыта. */
    conn->sends_in_flight++;
    int queued = frame_queue(conn, data, length);
    conn->sends_in_flight--;

    /* Завершение объявляем сразу и всегда: данные у нас в СВОЕЙ копии (или
     * отброшены), поэтому буфер ядра больше не нужен — оно его освободит.
     * Отказ объявляем только тогда, когда поток испорчен и соединение надо
     * закрыть. */
    transport_event_trigger_t ev = {
        .event = (queued < 0) ? TRANSPORT_TRANSFER_ERR : TRANSPORT_TRANSFER_COMPLETE,
    };
    ev.data.ptr    = data;
    ev.data.length = length;
    ev.data.left   = 0;

    conn->in_callback = 1;
    notify_event(conn, &ev);
    conn->in_callback = 0;
    if(conn->detached && !conn->sends_in_flight && !conn->recv_queued) free(conn);
    return TRANSPORT_OK;
}

static int32_t ws_httpd_peer_id(const void *self) {
    const ws_transport_conn_t *conn = (const ws_transport_conn_t *)self;
    return conn ? conn->fd : -1;
}

/* Таблица операций одна на все экземпляры: поведение у них одинаковое, разница
 * только в том, ЧЕЙ контекст им передали (transport_t.context). */
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
 * Наружу транспорт отдаёт не таблицу методов и не «модуль» по отдельности, а
 * ОБЪЕКТ: методы и свой контекст (transport_t в transport.h). Ядро API получает
 * его в api_init, подписывается через него и больше к транспорту никак не
 * обращается.
 *
 * Объект порождается вместе с экземпляром, а не лежит в готовом виде: экземпляр
 * живёт, пока жив его веб-сервер, и освобождает его тот же сервер
 * (ws_transport_destroy). Готового набора «для AP и для STA» здесь нет и быть не
 * может: экземпляр существует только потому, что его породил объект веб-сервера
 * интерфейса, а идентификатор экземпляра — указатель на его контекст. */

/* --- Приём ---------------------------------------------------------------- */

/* Запас места под ещё len байт собранного сообщения. NULL — сообщение вышло за
 * предел (или нет памяти): тогда соединение закрываем, принимать его негде. */
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

/* Собранное сообщение — ядру одним событием RECEIVED. Уровень сообщений: ядро
 * фрагментов не видит вовсе. Буфер освобождаем после возврата из обработчика:
 * ядро читает его только там. */
/* Предел того, что копится непринятым: очередь короткая (её разбирают на
 * каждом такте, раз в 1 мс), но одно сообщение бывает крупным — порция заливки
 * прошивки приходит base64-строкой в десяток килобайт. */
#define WS_RECV_QUEUE_MAX     16
#define WS_RECV_QUEUE_BYTES    (32u * 1024u)

/* Принятое сообщение — В ОЧЕРЕДЬ, а не сразу подписчику: разбор идёт в задаче
 * приложения (см. ws_transport_poll). Здесь только копия: буфер соединения
 * переиспользуется следующим кадром. */
static void deliver_message(ws_transport_conn_t *conn) {
    if(conn->msg_len == 0) {
        /* Пустое сообщение ядру не нужно: разбирать в нём нечего. */
        drop_message(conn);
        return;
    }

    ws_transport_module_t *m = conn->module;
    if((m->recv_lock == NULL) ||
       (conn->recv_queued >= WS_RECV_QUEUE_MAX) ||
       (m->recv_bytes + conn->msg_len > WS_RECV_QUEUE_BYTES)) {
        /* Разбирать негде или копится больше предела: соединение закрываем —
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

/* Чтение управляющего кадра. Обрабатывает их транспорт: ядру PING/PONG/CLOSE не
 * отдаются ни при каких условиях. При handle_control_frames веб-сервера их
 * забирает обработчик транспорта (наши PONG и CLOSE никто за нас не отправит),
 * при выключенном флаге PING и CLOSE берёт на себя веб-сервер. */
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
        esp_err_t err = web_server_send_ws(conn->module->server, fd, &reply, NULL, NULL);
        if(err != ESP_OK) ESP_LOGW(TAG, "Ответ на управляющий кадр не ушёл (err %d), fd=%d", err, fd);
    }
    /* PONG обрабатывать нечего: своих PING транспорт не посылает. */
    return ESP_OK;
}

static esp_err_t ws_httpd_handler(httpd_req_t *req) {
    int fd = httpd_req_to_sockfd(req);

    if (req->method == HTTP_GET) {
#if WS_CONN_FROM_POST_HANDSHAKE_CB
        /* Handshake already answered by esp_http_server; the session was created
         * in ws_httpd_post_handshake() above. */
        return ESP_OK;
#else
        /* conn_open сам решает, обслуживать ли соединение: если его интерфейс
         * никому не нужен или нет памяти, оно уже закрыто. */
        return conn_open(req, fd) ? ESP_OK : ESP_FAIL;
#endif
    }

    ws_transport_conn_t *conn = (ws_transport_conn_t *)req->sess_ctx;
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
        return ws_httpd_control_frame(conn, req, &f);

    /* Одиночный кадр больше предела: не читаем и не разбираем — закрываем
     * соединение (клиент получает закрытие вместо ответа API, потому что FID и
     * SID этого кадра мы намеренно не разбираем). */
    if (f.len > TRANSPORT_FRAME_MAX_LEN) {
        ESP_LOGW(TAG, "Кадр %u байт больше предела %d — закрываю fd=%d",
                 (unsigned)f.len, TRANSPORT_FRAME_MAX_LEN, fd);
        web_server_session_close(conn->module->server, fd);
        return ESP_OK;
    }

    if(f.type == HTTPD_WS_TYPE_CONTINUE) {
        if(conn->msg == NULL) {
            /* Продолжение без начала: склеивать не с чем, кадр отбрасываем. */
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
    /* Последний кадр — сообщение целиком; иначе ждём продолжений. */
    if(f.final) deliver_message(conn);
    return ESP_OK;
}

/* --- Соединение ----------------------------------------------------------- */

/* Allocates the connection object once per session and hands it to the API core.
 * It is stored in req->sess_ctx: esp_http_server propagates that into the
 * session on request cleanup and copies it back into every later request of the
 * same session, while session_free_ctx releases it when the session goes away.
 *
 * Экземпляр транспорта приходит контекстом обработчика (req->user_ctx): точка
 * живёт в объекте своего интерфейса, и запрос дошёл сюда именно потому, что его
 * обслуживает ЭТОТ объект (см. web_server_activate_ws). Поэтому чей это клиент,
 * здесь не выясняется — в отличие от номера сокета, который известен только тут. */
static ws_transport_conn_t *conn_open(httpd_req_t *req, int fd) {
    ws_transport_conn_t *conn = (ws_transport_conn_t *)req->sess_ctx;
    if (conn) return conn;

    ws_transport_module_t *module = (ws_transport_module_t *)req->user_ctx;
    if(module == NULL) {
        /* Обработчик вызван без контекста экземпляра: отдать соединение некому.
         * Возврат NULL закрывает сессию: вызывающий отвечает веб-серверу
         * ошибкой, и тот рвёт соединение (см. ws_httpd_post_handshake). */
        ESP_LOGW(TAG, "WS-точка без экземпляра транспорта, закрываю fd=%d", fd);
        return NULL;
    }

    conn = calloc(1, sizeof(ws_transport_conn_t));
    if (!conn) return NULL;
    conn->fd = fd;
    conn->module = module;
    conn->delegate.handler = link_event_handler;
    conn->delegate.context = conn;
    net_link_subscribe(fd, &conn->delegate);
    conn_list_add(module, conn);

    transport_event_trigger_t ev = { .event = TRANSPORT_ESTABLISHED };
    if(notify_event(conn, &ev) == 0) {
        /* Событие не получил никто: на этот экземпляр не подписано ядро (или оно
         * уже сняло подписку). Соединение обслуживать нечем — не заводим сессию
         * вовсе. Именно так выглядит и «ядро умерло»: делегат снят, а транспорт
         * об этом отдельно не спрашивают. */
        ESP_LOGE(TAG, "Ядро не подписано на интерфейсе %lu, закрываю fd=%d",
                 (unsigned long)web_server_iface(module->server), fd);
        event_unsubscribe(&conn->delegate);
        free(conn);
        return NULL;
    }

    ESP_LOGI(TAG, "WS connection accepted, fd=%d, iface=%lu",
             fd, (unsigned long)web_server_iface(module->server));

    req->sess_ctx = conn;
    req->free_ctx = session_free_ctx;
    return conn;
}

#if WS_CONN_FROM_POST_HANDSHAKE_CB
/* The handshake response has already been sent when this runs; the socket is a
 * WebSocket from here on. */
static esp_err_t ws_httpd_post_handshake(httpd_req_t *req) {
    int fd = httpd_req_to_sockfd(req);
    if (!conn_open(req, fd)) {
        ESP_LOGE(TAG, "WS session not created, fd=%d", fd);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "WS session opened, fd=%d", fd);
    return ESP_OK;
}
#endif

/* --- Порождение и освобождение экземпляра --------------------------------- */

/* Экземпляр порождает веб-сервер интерфейса (web_server_open_websocket): он уже
 * знает, на каком интерфейсе живёт точка, поэтому интерфейс сюда не передаётся и
 * не разыскивается. Возвращается объект транспорта с ещё не активированным
 * URI — активацию делает open, после того как вызывающий назначил обработчик. */
transport_t *ws_transport_create(web_server_t *server, const char *uri) {
    if((server == NULL) || (uri == NULL)) return NULL;

    ws_transport_module_t *m = calloc(1, sizeof(*m));
    if(m == NULL) return NULL;

    const ws_transport_config_t cfg = { .uri = uri, .server = server };
    if(ws_iface.init(m, &cfg) != TRANSPORT_OK) {
        free(m);
        return NULL;
    }

    /* Объект появляется вместе с экземпляром: без контекста транспорта не
     * бывает, а половины от разных экземпляров разойтись не могут. */
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
