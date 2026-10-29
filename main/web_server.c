/* web_server.c — объект веб-сервера на сетевой интерфейс: пути, политика, сессии. */

#include <stdlib.h>
#include <string.h>
#include "net_iface.h"

#include "esp_log.h"

#include "web_httpd.h"
#include "web_server.h"
#include "web_iface_policy.h"
#include "ws_transport.h"

#define GW_LOG_MODULE GW_LOG_HTTP
#include "gw_log.h"

static const char *TAG = "web_server";

/* Сколько путей может предложить один объект: страницы, файлы, прошивка и
 * WebSocket-точка своего интерфейса. */
#define WEB_SERVER_ROUTES_MAX    12

/* Сколько транспортов может породить один объект (в прошивке — одна точка).
 * Список нужен для владения: порождённое объектом освобождает объект. */
#define WEB_SERVER_TRANSPORTS_MAX 4

/* Сколько соединений интерфейса объект может учитывать одновременно (страницы и
 * WebSocket вместе) — из конфигурации проекта, с запасом на слоты обоих портов:
 * «нет места» не должно наступать раньше, чем у httpd кончатся сокеты. */
#define WEB_SERVER_SESSIONS_MAX \
    (CONFIG_WEB_SERVER_MAX_CLIENTS + CONFIG_WEB_SOCKET_MAX_CLIENTS)

struct web_server {
    uint32_t iface;                  /* какой интерфейс обслуживает объект */
    web_route_t routes[WEB_SERVER_ROUTES_MAX];  /* uri принадлежит объекту */
    uint16_t route_count;
    /* Порты сокетного слоя, в которые объект регистрирует свои пути: страницы и
     * WebSocket. NULL — порт не поднят, пути этого назначения не регистрируются. */
    web_httpd_t *ports[WEB_ROUTE_KINDS];
    /* Транспорты, привязанные к объекту (web_server_transport_attach). */
    transport_t *transports[WEB_SERVER_TRANSPORTS_MAX];
    uint8_t transport_count;
    /* Учтённые соединения интерфейса: дескриптор 0 — слот свободен. */
    int sessions[WEB_SERVER_SESSIONS_MAX];
    uint16_t session_count;
};

uint32_t web_server_iface(const web_server_t *srv) {
    return (srv != NULL) ? srv->iface : GW_IFACE_NONE;
}

/* ── Таблица путей ────────────────────────────────────────────────────────── */

/* Запись уже занята этим путём назначения (активная или заявленная). NULL —
 * такой записи нет. */
static web_route_t *route_reserved(web_server_t *srv, web_route_kind_t kind,
                                   const char *uri) {
    if ((srv == NULL) || (uri == NULL)) return NULL;
    for (uint16_t i = 0; i < srv->route_count; i++) {
        web_route_t *route = &srv->routes[i];
        if ((route->kind != kind) || (route->uri == NULL)) continue;
        if (strcmp(route->uri, uri) == 0) return route;
    }
    return NULL;
}

/* Занять место под путь в таблице объекта. Путь при этом ещё НЕ отдан httpd:
 * публикует его route_publish — регистрация страницы или активация
 * WebSocket-точки. Политика интерфейса применяется сразу, она следует за путём. */
static web_route_t *route_claim(web_server_t *srv, const char *uri,
                                web_route_kind_t kind) {
    if ((srv == NULL) || (uri == NULL)) return NULL;

    if (srv->ports[kind] == NULL) {
        ESP_LOGE(TAG, "Порт назначения %lu не поднят, путь '%s' не оказан",
                 (unsigned long)kind, uri);
        return NULL;
    }
    /* Одна запись на путь: вторая заявка на тот же URI означала бы два
     * обработчика для одного пути, и запросы достались бы только первому. */
    if (route_reserved(srv, kind, uri) != NULL) {
        ESP_LOGW(TAG, "Путь '%s' уже закреплён за интерфейсом %lu", uri,
                 (unsigned long)srv->iface);
        return NULL;
    }
    if (srv->route_count >= WEB_SERVER_ROUTES_MAX) {
        ESP_LOGE(TAG, "Таблица путей интерфейса %lu полна", (unsigned long)srv->iface);
        return NULL;
    }

    web_route_t *route = &srv->routes[srv->route_count];
    memset(route, 0, sizeof(*route));
    route->uri = strdup(uri);  /* шаблон может прийти из временной структуры */
    if (route->uri == NULL) return NULL;
    route->kind       = kind;
    route->method     = HTTP_GET;
    route->iface_mask = web_iface_service_mask(uri);
    srv->route_count++;
    return route;
}

/* Снять заявку: путь уходит из httpd (если был там) и из таблицы объекта. */
static void route_drop(web_server_t *srv, web_route_t *route) {
    if ((srv == NULL) || (route == NULL)) return;

    if ((route->handler != NULL) && (srv->ports[route->kind] != NULL))
        web_httpd_unregister(srv->ports[route->kind], route->uri, route->method);
    free((char *)route->uri);

    /* Таблица плоская: место снимается переносом последней записи. */
    const uint16_t idx = (uint16_t)(route - srv->routes);
    if (idx < srv->route_count) {
        srv->route_count--;
        if (idx != srv->route_count) srv->routes[idx] = srv->routes[srv->route_count];
        memset(&srv->routes[srv->route_count], 0, sizeof(srv->routes[0]));
    }
}

/* Отдать путь httpd. Пишется в запись до регистрации: httpd может прислать
 * запрос сразу после того, как узнал путь. */
static uint8_t route_publish(web_server_t *srv, web_route_t *route) {
    if ((route == NULL) || (srv->ports[route->kind] == NULL)) return 0;
    if (!web_httpd_register(srv->ports[route->kind], route)) return 0;
    return 1;
}

/* Путь, которым httpd выбрал этот запрос. Шаблон приходит от сокетного слоя:
 * сопоставление с путём запроса уже сделал httpd.
 *
 * Метод сверяется только у обычных маршрутов: рукопожатие WebSocket httpd
 * отвечает сам, а каждый следующий кадр приходит запросом с method == 0 (не GET).
 *
 * Заявка без обработчика (WS-точка, ещё не активированная) не находится: клиенту
 * нечего на неё ответить. */
static web_route_t *route_find(web_server_t *srv, web_route_kind_t kind,
                               const char *uri, httpd_method_t method) {
    if ((srv == NULL) || (uri == NULL)) return NULL;
    for (uint16_t i = 0; i < srv->route_count; i++) {
        web_route_t *route = &srv->routes[i];
        if (route->kind != kind) continue;
        if (route->handler == NULL) continue;
        if ((kind != WEB_ROUTE_WS) && (route->method != method)) continue;
        if (strcmp(route->uri, uri) == 0) return route;
    }
    return NULL;
}

/* ── Регистрация сервисов ───────────────────────────────────────────────────
 *
 * Политика «какой сервис каким интерфейсам оказан» живёт в одной таблице
 * (web_iface_policy.c): здесь она лишь применяется к маршруту. Отказ — 404, как у
 * несуществующего пути: клиенту нечего знать о том, что путь на устройстве есть. */
uint8_t web_server_register_uri(web_server_t *srv, const httpd_uri_t *uri) {
    if ((srv == NULL) || (uri == NULL) || (uri->uri == NULL) ||
        (uri->handler == NULL)) return 0;

    web_route_t *route = route_claim(srv, uri->uri, WEB_ROUTE_HTTP);
    if (route == NULL) return 0;

    route->method     = uri->method;
    route->handler    = uri->handler;
    route->user_ctx   = uri->user_ctx;
    route->is_websocket = 0;
    if (!route_publish(srv, route)) {
        route_drop(srv, route);
        return 0;
    }

    /* Сервис, оказанный любому интерфейсу, регистрируется как есть: на горячем
     * пути (кадры WebSocket, страницы, файлы) не появляется лишних вызовов. */
    if (route->iface_mask != GW_IFACE_MASK_ANY)
        LOGI(TAG, "Сервис '%s' оказан интерфейсам 0x%08lx; прочим — 404",
                 uri->uri, (unsigned long)route->iface_mask);
    return 1;
}

/* ── Транспорт интерфейса ─────────────────────────────────────────────────── */

/* Привязать порождённый транспорт к объекту: web_server_close его освободит.
 * Зовёт open транспорта — единственное место, где транспорт входит в объект. */
void web_server_transport_attach(web_server_t *srv, transport_t *transport) {
    if ((srv == NULL) || (transport == NULL)) return;
    if (srv->transport_count >= WEB_SERVER_TRANSPORTS_MAX) {
        ESP_LOGE(TAG, "Объект интерфейса %lu уже породил %u транспортов",
                 (unsigned long)srv->iface, (unsigned)srv->transport_count);
        return;
    }
    srv->transports[srv->transport_count++] = transport;
}

/* Занять путь за интерфейсом объекта под порт WebSocket (шаг 1 открытия:
 * его делает open транспорта). 0 — путь уже занят или нет места. */
uint8_t web_server_claim_ws(web_server_t *srv, const char *uri) {
    if ((srv == NULL) || (uri == NULL)) return 0;
    web_route_t *route = route_claim(srv, uri, WEB_ROUTE_WS);
    if (route == NULL) return 0;
    route->is_websocket = 1;
    return 1;
}

uint8_t web_server_activate_ws(web_server_t *srv, const char *uri,
                               const web_ws_callbacks_t *callbacks, void *context) {
    if ((srv == NULL) || (uri == NULL) || (callbacks == NULL) ||
        (callbacks->handler == NULL)) return 0;

    web_route_t *route = route_reserved(srv, WEB_ROUTE_WS, uri);
    if ((route == NULL) || (route->handler != NULL)) {
        ESP_LOGE(TAG, "URI '%s' не закреплён за интерфейсом %lu или уже активирован",
                 uri, (unsigned long)srv->iface);
        return 0;
    }

    /* Колбэки и контекст экземпляра — в запись пути: с ними веб-сервер зовёт
     * обработчик, и по контексту тот узнаёт свой транспорт. */
    route->handler                   = callbacks->handler;
    route->post_handshake            = callbacks->post_handshake;
    route->handle_ws_control_frames  = callbacks->handle_control_frames;
    route->user_ctx                  = context;

    /* Отказ публикации — это отказ активации: точка снова становится заявкой, а
     * о причине скажет тот, кто её активировал (транспорт). */
    if (!route_publish(srv, route)) {
        route->handler  = NULL;
        route->user_ctx = NULL;
        return 0;
    }

    LOGI(TAG, "WebSocket-точка '%s' активна на интерфейсе %lu", uri,
             (unsigned long)srv->iface);
    return 1;
}

/* ── Поиск маршрута по пути запроса ──────────────────────────────────────────
 *
 * httpd выбирает обработчик по своей таблице, а она одна на порт: путь мог
 * зарегистрировать объект ДРУГОГО интерфейса. Поэтому объект, не найдя у себя
 * шаблон, которым httpd выбрал запрос, ищет по СВОЕЙ таблице и по пути запроса, в
 * порядке регистрации. Общий обработчик файлов (шаблон «звёздочка») есть у каждого
 * объекта, поэтому запрос с интерфейса, которому путь не оказан, означает обычный
 * путь файловой системы. */
static web_route_t *route_find_request(web_server_t *srv, web_route_kind_t kind,
                                       const char *path, httpd_method_t method) {
    if ((path == NULL) || (path[0] != '/')) return NULL;

    for (uint16_t i = 0; i < srv->route_count; i++) {
        web_route_t *route = &srv->routes[i];
        if (route->kind != kind) continue;
        if (route->handler == NULL) continue;
        if (route->uri == NULL) continue;
        if ((kind != WEB_ROUTE_WS) && (route->method != method)) continue;
        if (web_iface_path_matches(route->uri, path)) return route;
    }
    return NULL;
}

/* ── Диспетчеризация запроса ──────────────────────────────────────────────── */

/* Вызвать обработчик маршрута с контекстом, с которым его регистрировали: в
 * req->user_ctx на время вызова лежит то, что положил сокетный слой, поэтому
 * исходное значение возвращаем на место. */
static esp_err_t route_call(web_route_t *route, httpd_req_t *req) {
    if ((route == NULL) || (req == NULL)) return ESP_FAIL;

    void *dispatch_ctx = req->user_ctx;
    req->user_ctx = route->user_ctx;
    const esp_err_t err = route->handler(req);
    req->user_ctx = dispatch_ctx;
    return err;
}

esp_err_t web_server_handle(web_server_t *srv, web_route_kind_t kind,
                            const char *uri, httpd_req_t *req) {
    if ((srv == NULL) || (req == NULL)) return ESP_FAIL;

    web_route_t *route = route_find(srv, kind, uri, req->method);
    /* Шаблон, которым httpd выбрал запрос, мог принадлежать другому объекту:
     * тогда решает своя таблица, по пути запроса (см. route_find_request). */
    if (route == NULL) {
        route = route_find_request(srv, kind, req->uri, req->method);
        if (route != NULL)
            LOGD(TAG, "URI '%s' найден по таблице интерфейса %lu, а не по '%s'",
                     req->uri, (unsigned long)srv->iface, uri);
    }
    if (route == NULL) {
        LOGI(TAG, "URI '%s' не оказан интерфейсу %lu — 404", uri,
                 (unsigned long)srv->iface);
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
    }

    if (!web_iface_mask_allows(route->iface_mask, srv->iface)) {
        LOGI(TAG, "URI '%s' не обслуживается на интерфейсе %lu (маска 0x%08lx) — 404",
                 uri, (unsigned long)srv->iface, (unsigned long)route->iface_mask);
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
    }

    return route_call(route, req);
}

esp_err_t web_server_handshake(web_server_t *srv, web_route_kind_t kind,
                               const char *uri, httpd_req_t *req) {
    if ((srv == NULL) || (uri == NULL) || (req == NULL)) return ESP_FAIL;

    web_route_t *route = route_find(srv, kind, uri, HTTP_GET);
    if ((route == NULL) || (route->post_handshake == NULL)) {
        LOGI(TAG, "Рукопожатие '%s' не оказано интерфейсу %lu", uri,
                 (unsigned long)srv->iface);
        return ESP_FAIL;
    }
    /* Ответ рукопожатия httpd уже отправил, поэтому отказать «страницей» нельзя —
     * только закрыть соединение. */
    if (!web_iface_mask_allows(route->iface_mask, srv->iface)) return ESP_FAIL;

    void *dispatch_ctx = req->user_ctx;
    req->user_ctx = route->user_ctx;
    const esp_err_t err = route->post_handshake(req);
    req->user_ctx = dispatch_ctx;
    return err;
}

/* ── Учёт сессий ──────────────────────────────────────────────────────────── */

uint8_t web_server_session_open(web_server_t *srv, int fd) {
    if ((srv == NULL) || (fd <= 0)) return 0;
    for (uint16_t i = 0; i < srv->session_count; i++)      /* уже учтено */
        if (srv->sessions[i] == fd) return 1;
    if (srv->session_count >= WEB_SERVER_SESSIONS_MAX) return 0;

    srv->sessions[srv->session_count++] = fd;
    LOGD(TAG, "Сессия интерфейса %lu: fd=%d (открыто %u)", (unsigned long)srv->iface,
             fd, (unsigned)srv->session_count);
    return 1;
}

void web_server_session_release(web_server_t *srv, int fd) {
    if ((srv == NULL) || (fd <= 0)) return;
    for (uint16_t i = 0; i < srv->session_count; i++) {
        if (srv->sessions[i] != fd) continue;
        srv->session_count--;
        srv->sessions[i] = srv->sessions[srv->session_count];
        srv->sessions[srv->session_count] = 0;
        LOGD(TAG, "Сессия интерфейса %lu: fd=%d закрыта (осталось %u)",
                 (unsigned long)srv->iface, fd, (unsigned)srv->session_count);
        return;
    }
}

uint16_t web_server_session_count(const web_server_t *srv) {
    return (srv != NULL) ? srv->session_count : 0;
}

uint8_t web_server_session_close(web_server_t *srv, int fd) {
    if ((srv == NULL) || (fd <= 0)) return 0;
    uint8_t closed = 0;
    for (uint32_t kind = 0; kind < WEB_ROUTE_KINDS; kind++)
        closed |= web_httpd_kick(srv->ports[kind], fd);
    return closed;
}

int web_server_write(web_server_t *srv, int fd, const char *buf, size_t len,
                     uint32_t stall_ms) {
    if ((srv == NULL) || (srv->ports[WEB_ROUTE_WS] == NULL)) {
        ESP_LOGW(TAG, "write: нет порта WS (srv=%p fd=%d len=%u)", (void *)srv, fd, (unsigned)len);
        return -1;
    }
    return web_httpd_write(srv->ports[WEB_ROUTE_WS], fd, buf, len, stall_ms);
}

esp_err_t web_server_send_ws(web_server_t *srv, int fd, httpd_ws_frame_t *frame,
                             transfer_complete_cb done, void *arg) {
    if ((srv == NULL) || (srv->ports[WEB_ROUTE_WS] == NULL))
        return ESP_ERR_INVALID_STATE;
    return web_httpd_send_ws(srv->ports[WEB_ROUTE_WS], fd, frame, done, arg);
}

/* ── Подъём и гашение ─────────────────────────────────────────────────────── */

web_server_t *web_server_open(uint32_t iface) {
    if ((iface == GW_IFACE_NONE) || (iface > GW_IFACE_LAST)) return NULL;

    web_server_t *srv = calloc(1, sizeof(web_server_t));
    if (srv == NULL) return NULL;
    srv->iface = iface;

    /* Объект приходит к каждому поднятому порту сам: порт помнит, чей он, — иначе
     * появился бы скрытый реестр серверов. */
    for (uint32_t kind = 0; kind < WEB_ROUTE_KINDS; kind++) {
        web_httpd_t *port = web_httpd_port((web_route_kind_t)kind);
        if (port == NULL) {
            ESP_LOGW(TAG, "Порт назначения %lu не поднят: интерфейс %lu его не получит",
                     (unsigned long)kind, (unsigned long)iface);
            continue;
        }
        if (!web_httpd_attach(port, srv)) {
            for (uint32_t prev = 0; prev < kind; prev++)
                if (srv->ports[prev] != NULL) web_httpd_detach(srv->ports[prev], srv);
            free(srv);
            return NULL;
        }
        srv->ports[kind] = port;
    }

    LOGI(TAG, "Веб-сервер интерфейса %lu поднят", (unsigned long)iface);
    return srv;
}

void web_server_close(web_server_t *srv) {
    if (srv == NULL) return;

    /* Сначала отвязываемся: соединения интерфейса перестают попадать в объект, а
     * записи портов перестают ссылаться на его память. Открытые сессии остаются
     * за сокетным слоем — он их и закрывает. */
    for (uint32_t kind = 0; kind < WEB_ROUTE_KINDS; kind++)
        if (srv->ports[kind] != NULL) web_httpd_detach(srv->ports[kind], srv);

    while (srv->route_count > 0)
        route_drop(srv, &srv->routes[0]);

    /* Транспорты породил этот объект — он же их и освобождает: их контекст
     * ссылается на объект, и после free(srv) этой ссылке жить негде. */
    for (uint8_t i = 0; i < srv->transport_count; i++)
        ws_transport_destroy(srv->transports[i]);
    srv->transport_count = 0;

    LOGI(TAG, "Веб-сервер интерфейса %lu погашен", (unsigned long)srv->iface);
    free(srv);
}
