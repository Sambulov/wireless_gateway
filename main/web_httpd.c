/* web_httpd.c — сокетный слой веб-сервера: httpd на порт, сессии, отправка. */

#include <errno.h>
#include <string.h>
#include "net_iface.h"

#include <sys/socket.h>
#include <lwip/sockets.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "connection.h"
#include "web_httpd.h"
#include "web_server.h"

#define GW_LOG_MODULE GW_LOG_HTTP
#include "gw_log.h"

static const char *TAG = "web_httpd";

/* Сколько путей может быть в одном порту (страницы, файлы, прошивка, WS-точки) */
#define WEB_HTTPD_ROUTES_MAX    16

/* Шаблон пути копируется в запись порта: httpd и объект живут дольше вызова
 * регистрации, а шаблон может прийти из временной структуры. */
#define WEB_URI_MAX             32

/* Запись на сессию порта. */
#define WEB_HTTPD_SESSIONS_MAX  32

#define WEB_SEND_STEP_MS        10

/* Через сколько простоя «последний успех» больше не считается: сессия могла
 * простоять в keep-alive, и окно отсчёта бюджета должно начинаться заново. */
#define WEB_SEND_IDLE_RESET_MS  5000

/* Один зарегистрированный в httpd путь. refs — сколько объектов веб-сервера его
 * предложили: httpd путь хранит один раз, снимается он, когда снят у всех. */
typedef struct {
    web_httpd_t *port;              /* порт, в httpd которого путь зарегистрирован */
    char uri[WEB_URI_MAX];          /* шаблон пути, как его зарегистрировали */
    httpd_method_t method;
    uint16_t refs;
} web_httpd_route_t;

typedef struct {
    int fd;                          /* 0 — слот свободен */
    web_server_t *srv;               /* чей объект обслуживает сессию */
    TickType_t last_ok;              /* когда отправка в последний раз прошла */
} web_httpd_session_t;

struct web_httpd {
    httpd_handle_t handle;
    web_route_kind_t kind;           /* назначение порта: страницы или WebSocket */
    uint16_t port;
    uint32_t send_budget_ms;
    web_httpd_route_t routes[WEB_HTTPD_ROUTES_MAX];
    web_httpd_session_t sessions[WEB_HTTPD_SESSIONS_MAX];
    /* Объекты веб-сервера, привязанные к порту: по одному на интерфейс. */
    web_server_t *servers[GW_IFACE_LAST + 1];
};

/* Порты — объекты модуля: их адрес переживает httpd и передаётся в него как
 * global_user_ctx, поэтому в задачах httpd ничего не разыскивается. */
static web_httpd_t ports[WEB_ROUTE_KINDS];

/* httpd освобождает global_user_ctx на остановке; объект порта статический,
 * освобождать нечего. */
static void port_ctx_release(void *ctx) {
    (void)ctx;
}

/* ── Сессии порта ─────────────────────────────────────────────────────────── */

static web_httpd_session_t *session_find(web_httpd_t *port, int fd) {
    if ((port == NULL) || (fd <= 0)) return NULL;
    for (size_t i = 0; i < WEB_HTTPD_SESSIONS_MAX; i++)
        if (port->sessions[i].fd == fd) return &port->sessions[i];
    return NULL;
}

static web_httpd_session_t *session_take(web_httpd_t *port, int fd) {
    web_httpd_session_t *s = session_find(port, fd);
    if (s != NULL) return s;
    for (size_t i = 0; i < WEB_HTTPD_SESSIONS_MAX; i++) {
        if (port->sessions[i].fd == 0) {
            port->sessions[i].fd      = fd;
            port->sessions[i].srv     = NULL;
            port->sessions[i].last_ok = xTaskGetTickCount();
            return &port->sessions[i];
        }
    }
    return NULL;                     /* таблица полна — работаем без прогресса */
}

static void session_drop(web_httpd_t *port, int fd) {
    web_httpd_session_t *s = session_find(port, fd);
    if (s != NULL) {
        s->fd  = 0;
        s->srv = NULL;
    }
}

/* ── Отправка без бесконечного ожидания ─────────────────────────────────────
 *
 * esp_http_server обслуживает все сессии одного экземпляра ОДНОЙ задачей, а её
 * штатная отправка блокирующая: клиент, переставший читать, залипает в send() и
 * останавливает обслуживание всех остальных. Поэтому здесь своя отправка с
 * бюджетом порта.
 *
 * Ключ сессии — дескриптор, выданный VFS; он не ограничен
 * CONFIG_LWIP_MAX_SOCKETS, поэтому таблица с проверкой ключа, а не массив по fd. */
static int send_bounded(web_httpd_t *port, int fd, const char *buf, size_t len,
                        int flags, uint32_t budget_ms) {
    web_httpd_session_t *s = session_take(port, fd);
    TickType_t call_start = xTaskGetTickCount();
    /* Сессия могла просто простоять (keep-alive у браузера — до 30 с), и тогда
     * «последний успех» у неё старый: окно отсчёта начинается заново, иначе первый
     * же запрос после паузы будет убит как нечитающий. */
    if (s && (call_start - s->last_ok) > pdMS_TO_TICKS(WEB_SEND_IDLE_RESET_MS))
        s->last_ok = call_start;
    for (;;) {
        int ret = send(fd, buf, len, (flags & ~MSG_WAITALL) | MSG_DONTWAIT);
        if (ret > 0) {
            if (s) s->last_ok = xTaskGetTickCount();
            return ret;
        }
        /* ret == 0 при закрытом окне — не ошибка, но и не прогресс */
        if (ret < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
            errno != EINTR && errno != ENOMEM && errno != ENOBUFS)
            return -1;
        TickType_t base = s ? s->last_ok : call_start;
        if ((xTaskGetTickCount() - base) > pdMS_TO_TICKS(budget_ms)) {
            ESP_LOGW(TAG, "fd=%d не принимает данные %lu мс, закрываю сессию",
                     fd, (unsigned long)budget_ms);
            if (s) {
                /* Сессия закрывается — освобождаем и запись прогресса, и учёт в
                 * объекте: повторно его снимет уже нечего. */
                web_server_t *srv = s->srv;
                s->fd  = 0;
                s->srv = NULL;
                if (srv != NULL) web_server_session_release(srv, fd);
            }
            return -1;
        }
        vTaskDelay(pdMS_TO_TICKS(WEB_SEND_STEP_MS));
    }
}

/* Запись куска в сокет сессии: сколько сокет принял — столько и записано.
 *
 * Возврат: >= 0 — записано столько байт (может быть меньше len — остаток вызывающий
 * допишет сам, у него своя копия); -1 — сокет сломан (не EAGAIN, а настоящая
 * ошибка), писать в него больше нельзя.
 *
 * Ждать «пока влезет всё» нельзя: вызывающий — такт сервисов, и один нечитающий
 * клиент не должен его задерживать. */
int web_httpd_write(web_httpd_t *port, int fd, const char *buf, size_t len,
                    uint32_t stall_ms) {
    if ((port == NULL) || (fd <= 0) || (buf == NULL) || (len == 0)) return -1;
    web_httpd_session_t *s = session_take(port, fd);
    size_t done = 0;
    TickType_t base = xTaskGetTickCount();
    for (;;) {
        int ret = send(fd, buf + done, len - done, MSG_DONTWAIT);
        if (ret > 0) {
            done += (size_t)ret;
            if (s) s->last_ok = xTaskGetTickCount();
            if (done == len) return (int)done;
            continue;
        }
        if (ret < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR
            && errno != ENOMEM && errno != ENOBUFS) {
            ESP_LOGW(TAG, "write fd=%d len=%u errno=%d (%s), записано %u",
                     fd, (unsigned)len, errno, strerror(errno), (unsigned)done);
            return (done == 0) ? -1 : (int)done;
        }
        /* Места нет. Что-то записано — ждём недолго и добираем; не записано
         * ничего — сразу отдаём 0, пусть вызывающий решает сам. */
        if ((done == 0) || ((xTaskGetTickCount() - base) > pdMS_TO_TICKS(stall_ms)))
            return (int)done;
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

/* Своя отправка сессии порта: бюджет берётся из настроек порта. */
static int port_send(httpd_handle_t hd, int fd, const char *buf, size_t len, int flags) {
    web_httpd_t *port = (web_httpd_t *)httpd_get_global_user_ctx(hd);
    if (port == NULL) return -1;
    return send_bounded(port, fd, buf, len, flags, port->send_budget_ms);
}

/* ── Сессия: открытие и закрытие ────────────────────────────────────────────
 *
 * Соединение разводится по интерфейсу сокета: порт находит объект веб-сервера
 * своего интерфейса и отдаёт сессию ему. С интерфейса, для которого объект не
 * поднят, соединение закрывается: обслуживать его нечем. */
static esp_err_t port_session_open(httpd_handle_t hd, int sockfd) {
    web_httpd_t *port = (web_httpd_t *)httpd_get_global_user_ctx(hd);
    if (port == NULL) return ESP_FAIL;

    const uint32_t iface = gw_iface_of_socket(sockfd);
    web_server_t *srv = web_httpd_server(port, iface);
    if (srv == NULL) {
        ESP_LOGW(TAG, "Интерфейс %lu не обслуживается веб-сервером, закрываю fd=%d",
                 (unsigned long)iface, sockfd);
        return ESP_FAIL;             /* httpd закроет сокет сам */
    }

    web_httpd_session_t *s = session_take(port, sockfd);
    if (s != NULL) s->srv = srv;
    if (!web_server_session_open(srv, sockfd))
        ESP_LOGW(TAG, "Учёт сессий интерфейса %lu: мест нет, fd=%d",
                 (unsigned long)iface, sockfd);

    /* TCP_NODELAY. Без него мелкие ответы (JSON API и кадры WebSocket) ждут
     * подтверждения по Нейглу вместе с задержанным ACK, и любой запрос стоит
     * 100-250 мс независимо от объёма. esp_http_server включает TCP_NODELAY только
     * для сообщений об ошибках, обычные ответы и кадры WS идут через Нейгла.
     * Ставим один раз на сессию. */
    int one = 1;
    if (setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) < 0)
        ESP_LOGW(TAG, "TCP_NODELAY для fd=%d не включился (%d)", sockfd, errno);

    return httpd_sess_set_send_override(hd, sockfd, port_send);
}

static void port_session_close(httpd_handle_t hd, int sockfd) {
    web_httpd_t *port = (web_httpd_t *)httpd_get_global_user_ctx(hd);
    web_httpd_session_t *s = (port != NULL) ? session_find(port, sockfd) : NULL;
    if ((s != NULL) && (s->srv != NULL))
        web_server_session_release(s->srv, sockfd);
    if (port != NULL) session_drop(port, sockfd);

    /* ВАЖНО: при заданном close_fn httpd НЕ закрывает сокет сам — он просто
     * вызывает этот обработчик вместо close() (см. httpd_sess_delete в
     * httpd_sess.c). Если здесь не закрыть, каждый завершённый сеанс теряет
     * сокет: пул LWIP исчерпывается, accept() возвращает ENFILE, и сервер
     * начинает отвечать сбросом на все запросы. */
    close(sockfd);
}

/* ── Развод запроса по объекту ──────────────────────────────────────────────
 *
 * httpd различает только «путь → обработчик»: масок интерфейсов у него нет.
 * Поэтому каждый путь зарегистрирован в httpd через эту обёртку: она узнаёт
 * интерфейс соединения (gw_iface_of_socket) и передаёт запрос объекту веб-сервера
 * этого интерфейса. user_ctx обёртки — запись пути на порту. */
static esp_err_t port_guard(httpd_req_t *req) {
    web_httpd_route_t *reg = (web_httpd_route_t *)req->user_ctx;
    web_httpd_t *port = (reg != NULL) ? reg->port : NULL;
    if (port == NULL) return ESP_FAIL;

    const int fd = httpd_req_to_sockfd(req);
    const uint32_t iface = gw_iface_of_socket(fd);
    web_server_t *srv = web_httpd_server(port, iface);
    if (srv == NULL) {
        ESP_LOGW(TAG, "URI '%s' с интерфейса %lu: объекта нет, закрываю fd=%d",
                 reg->uri, (unsigned long)iface, fd);
        return ESP_FAIL;             /* httpd закроет сессию */
    }
    return web_server_handle(srv, port->kind, reg->uri, req);
}

/* Рукопожатие WebSocket httpd зовёт сам, минуя port_guard: req->user_ctx в этот
 * момент — запись пути на порту, а не контекст обработчика, поэтому объект
 * находится по интерфейсу соединения и контекст подставляет уже он. */
static esp_err_t port_guard_handshake(httpd_req_t *req) {
    web_httpd_route_t *reg = (web_httpd_route_t *)req->user_ctx;
    web_httpd_t *port = (reg != NULL) ? reg->port : NULL;
    if (port == NULL) return ESP_FAIL;

    const int fd = httpd_req_to_sockfd(req);
    const uint32_t iface = gw_iface_of_socket(fd);
    web_server_t *srv = web_httpd_server(port, iface);
    if (srv == NULL) {
        ESP_LOGW(TAG, "Рукопожатие '%s' с интерфейса %lu: объекта нет, закрываю fd=%d",
                 reg->uri, (unsigned long)iface, fd);
        return ESP_FAIL;
    }
    return web_server_handshake(srv, port->kind, reg->uri, req);
}

/* ── События httpd ──────────────────────────────────────────────────────────
 *
 * Регистрация одна на все экземпляры: esp_event_handler_register не идемпотентен,
 * а событие приходит и от порта страниц, и от порта WebSocket. */
static void port_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
    switch (event_id) {
        case HTTP_SERVER_EVENT_ERROR:      /* httpd_err_code_t */
            ESP_LOGW(TAG, "error: '%08lx'", (unsigned long)(uintptr_t)event_data);
            break;
        case HTTP_SERVER_EVENT_START:
            LOGI(TAG, "started");
            break;
        case HTTP_SERVER_EVENT_ON_CONNECTED: /* int (fd) */
            LOGD(TAG, "new connection: fd=%d", *(int *)event_data);
            break;
        case HTTP_SERVER_EVENT_ON_HEADER:
            LOGD(TAG, "on header: '%08lx'", (unsigned long)(uintptr_t)event_data);
            break;
        case HTTP_SERVER_EVENT_HEADERS_SENT:
            LOGD(TAG, "headers sent: '%08lx'", (unsigned long)(uintptr_t)event_data);
            break;
        /* Эти два события приходят НА КАЖДУЮ порцию данных в обе стороны.
         * Логировать их на уровне WARN нельзя: вывод в консоль синхронный и
         * тормозит задачу httpd прямо посреди отдачи файла. */
        case HTTP_SERVER_EVENT_ON_DATA:
        case HTTP_SERVER_EVENT_SENT_DATA:
            LOGD(TAG, "%s: fd=%d len=%d",
                     (event_id == HTTP_SERVER_EVENT_ON_DATA) ? "on data" : "data sent",
                     ((esp_http_server_event_data *)event_data)->fd,
                     ((esp_http_server_event_data *)event_data)->data_len);
            break;
        case HTTP_SERVER_EVENT_DISCONNECTED: /* int (fd) */
            LOGD(TAG, "disconnected: fd=%d", *(int *)event_data);
            break;
        case HTTP_SERVER_EVENT_STOP:
            LOGI(TAG, "stopped");
            break;
        default:
            break;
    }
}

/* ── Подъём портов ────────────────────────────────────────────────────────── */

web_httpd_t *web_httpd_port(web_route_kind_t kind) {
    if ((uint32_t)kind >= WEB_ROUTE_KINDS) return NULL;
    return (ports[kind].handle != NULL) ? &ports[kind] : NULL;
}

static uint8_t port_start(web_httpd_t *port, const web_httpd_config_t *cfg) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    port->port           = cfg->port;
    port->send_budget_ms = cfg->send_budget_ms;
    /* Шаблоны путей — свои: httpd выбирает обработчик по шаблону (см. ниже), а
     * что именно оказано соединению, решает объект веб-сервера. */
    config.uri_match_fn      = httpd_uri_match_wildcard;
    config.server_port       = cfg->port;
    config.max_uri_handlers  = cfg->max_uri_handlers;
    config.max_open_sockets  = cfg->max_open_sockets;
    config.keep_alive_enable = true;
    /* Брошенное соединение держит сокет из общего пула, а сокетов всего
     * CONFIG_LWIP_MAX_SOCKETS: когда пул кончался, httpd отвечал сбросом на всё
     * подряд (ENFILE). 10 секунд — достаточно для догрузки ресурсов страницы. */
    config.keep_alive_idle     = 10;
    config.keep_alive_interval = 5;
    config.keep_alive_count    = 3;

    /* Вытеснение по LRU ОТКЛЮЧЕНО: оно закрывает уже принятые сессии, чтобы
     * впустить новую, и при быстром клиенте с keep-alive рвёт живые соединения.
     * Вместо него пул сокетов увеличен. */
    config.lru_purge_enable = false;

    /* Своя отправка с бюджетом и закрытие сессии с освобождением её слота. */
    config.open_fn  = port_session_open;
    config.close_fn = port_session_close;

    /* Порт передаётся в задачи httpd объектом: у open_fn/close_fn и у своей
     * отправки нет других аргументов, кроме дескриптора. */
    config.global_user_ctx          = port;
    config.global_user_ctx_free_fn  = port_ctx_release;

    /* Все экземпляры httpd по умолчанию делят ОДИН ctrl_port (внутренний UDP-канал
     * межзадачной сигнализации), поэтому порту WebSocket нужен свой. */
    if (port->kind == WEB_ROUTE_WS)
        config.ctrl_port = ESP_HTTPD_DEF_CTRL_PORT + 1;

    LOGI(TAG, "Starting server on port: '%d'", config.server_port);
    if (httpd_start(&port->handle, &config) != ESP_OK) {
        port->handle = NULL;
        ESP_LOGE(TAG, "Error starting server on port '%d'", config.server_port);
        return 0;
    }
    return 1;
}

uint8_t web_httpd_start(const web_httpd_config_t config[WEB_ROUTE_KINDS]) {
    uint8_t started = 0;

    if (config == NULL) return 0;
    memset(ports, 0, sizeof(ports));

    /* Регистрация одна на оба порта: обработчик события не знает, чей httpd
     * прислал событие, и повторная регистрация на второй порт не нужна. */
    esp_event_handler_register(ESP_HTTP_SERVER_EVENT, ESP_EVENT_ANY_ID,
                               &port_event_handler, NULL);

    for (uint32_t kind = 0; kind < WEB_ROUTE_KINDS; kind++) {
        ports[kind].kind = (web_route_kind_t)kind;
        if (port_start(&ports[kind], &config[kind])) {
            started++;
        } else {
            ESP_LOGE(TAG, "Порт назначения %lu не поднялся", (unsigned long)kind);
        }
    }
    return (started == WEB_ROUTE_KINDS);
}

void web_httpd_stop(void) {
    for (uint32_t kind = 0; kind < WEB_ROUTE_KINDS; kind++) {
        if (ports[kind].handle != NULL) {
            httpd_stop(ports[kind].handle);
            ports[kind].handle = NULL;
        }
    }
    esp_event_handler_unregister(ESP_HTTP_SERVER_EVENT, ESP_EVENT_ANY_ID,
                                 &port_event_handler);
    memset(ports, 0, sizeof(ports));
}

/* ── Связь с объектами веб-сервера ────────────────────────────────────────── */

uint8_t web_httpd_attach(web_httpd_t *port, web_server_t *srv) {
    if ((port == NULL) || (srv == NULL)) return 0;
    const uint32_t iface = web_server_iface(srv);
    if (iface > GW_IFACE_LAST) return 0;
    if (port->servers[iface] != NULL) {
        ESP_LOGW(TAG, "Интерфейс %lu уже обслуживается другим объектом",
                 (unsigned long)iface);
        return 0;
    }
    port->servers[iface] = srv;
    return 1;
}

void web_httpd_detach(web_httpd_t *port, web_server_t *srv) {
    if ((port == NULL) || (srv == NULL)) return;
    const uint32_t iface = web_server_iface(srv);
    if ((iface <= GW_IFACE_LAST) && (port->servers[iface] == srv))
        port->servers[iface] = NULL;

    /* Записи сессий ссылаются на объект: отвязанный объект может быть
     * освобождён, поэтому ссылку снимаем здесь, до его освобождения. */
    for (size_t i = 0; i < WEB_HTTPD_SESSIONS_MAX; i++)
        if (port->sessions[i].srv == srv) port->sessions[i].srv = NULL;
}

web_server_t *web_httpd_server(web_httpd_t *port, uint32_t iface) {
    if ((port == NULL) || (iface > GW_IFACE_LAST)) return NULL;
    return port->servers[iface];
}

/* ── Регистрация путей ────────────────────────────────────────────────────── */

uint8_t web_httpd_register(web_httpd_t *port, const web_route_t *route) {
    if ((port == NULL) || (port->handle == NULL) || (route == NULL) ||
        (route->uri == NULL)) return 0;
    if (strlen(route->uri) >= WEB_URI_MAX) {
        ESP_LOGE(TAG, "Путь '%s' длиннее %d символов", route->uri, WEB_URI_MAX - 1);
        return 0;
    }

    web_httpd_route_t *free_slot = NULL;
    for (size_t i = 0; i < WEB_HTTPD_ROUTES_MAX; i++) {
        web_httpd_route_t *reg = &port->routes[i];
        if (reg->port == NULL) {
            if (free_slot == NULL) free_slot = reg;
            continue;
        }
        if ((reg->method == route->method) && (strcmp(reg->uri, route->uri) == 0)) {
            /* Путь уже в httpd: его предложил другой объект. Обработчик у порта
             * один на всех (port_guard), поэтому регистрировать нечего — считаем
             * ссылки. */
            reg->refs++;
            return 1;
        }
    }
    if (free_slot == NULL) {
        ESP_LOGE(TAG, "В порту нет места под путь '%s'", route->uri);
        return 0;
    }

    /* В httpd путь попадает обёрткой: её user_ctx — эта запись, по ней обёртка
     * находит порт и шаблон, которым httpd выбрал обработчик. */
    httpd_uri_t guarded = {
        .uri       = free_slot->uri,
        .method    = route->method,
        .handler   = port_guard,
        .user_ctx  = free_slot,
    };
#if CONFIG_HTTPD_WS_SUPPORT
    guarded.is_websocket             = route->is_websocket;
    guarded.handle_ws_control_frames = route->handle_ws_control_frames;
#endif
#if CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT
    /* Обёртка, а не обработчик объекта: рукопожатие httpd зовёт сам, и контекст
     * объекта подставляет порт (см. port_guard_handshake). */
    guarded.ws_post_handshake_cb     = (route->post_handshake != NULL)
                                     ? port_guard_handshake : NULL;
#endif

    strlcpy(free_slot->uri, route->uri, sizeof(free_slot->uri));
    free_slot->method = route->method;
    free_slot->refs   = 1;
    /* Запись готова до регистрации: обёртка может прийти на запрос сразу после
     * того, как httpd узнал путь. */
    free_slot->port   = port;

    if (httpd_register_uri_handler(port->handle, &guarded) != ESP_OK) {
        free_slot->port   = NULL;
        free_slot->refs   = 0;
        free_slot->uri[0] = '\0';
        ESP_LOGE(TAG, "Путь '%s' не зарегистрирован в httpd", route->uri);
        return 0;
    }
    return 1;
}

uint8_t web_httpd_unregister(web_httpd_t *port, const char *uri,
                             httpd_method_t method) {
    if ((port == NULL) || (port->handle == NULL) || (uri == NULL)) return 0;

    for (size_t i = 0; i < WEB_HTTPD_ROUTES_MAX; i++) {
        web_httpd_route_t *reg = &port->routes[i];
        if ((reg->port == NULL) || (reg->method != method)) continue;
        if (strcmp(reg->uri, uri) != 0) continue;

        if (reg->refs > 1) {          /* путь ещё нужен другому объекту */
            reg->refs--;
            return 1;
        }
        if (httpd_unregister_uri_handler(port->handle, uri, method) != ESP_OK)
            ESP_LOGW(TAG, "Путь '%s' не снят с httpd", uri);
        reg->port   = NULL;
        reg->refs   = 0;
        reg->uri[0] = '\0';
        return 1;
    }
    return 0;
}

/* ── Сессии и отправка для объектов ───────────────────────────────────────── */

uint8_t web_httpd_kick(web_httpd_t *port, int fd) {
    if ((port == NULL) || (port->handle == NULL) || (fd < 0)) return 0;
    return httpd_sess_trigger_close(port->handle, fd) == ESP_OK;
}

esp_err_t web_httpd_send_ws(web_httpd_t *port, int fd, httpd_ws_frame_t *frame,
                            transfer_complete_cb done, void *arg) {
    if ((port == NULL) || (port->handle == NULL) || (frame == NULL))
        return ESP_ERR_INVALID_ARG;
    if (done == NULL)
        return httpd_ws_send_frame_async(port->handle, fd, frame);
    return httpd_ws_send_data_async(port->handle, fd, frame, done, arg);
}
