#pragma once

#include <stdint.h>
#include "net_iface.h"

#include "esp_err.h"
#include "esp_http_server.h"

#include "transport.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Объект веб-сервера на сетевой интерфейс.
 *
 * httpd умеет только «путь → обработчик» и слушает порт: привязать экземпляр к
 * адресу или интерфейсу в httpd_config_t нельзя. Поэтому порт поднимает сокетный
 * слой (web_httpd.c — один httpd на порт), а «что оказано клиенту» решает объект
 * на интерфейс: своя таблица URI с обработчиками, своя политика доступа
 * (web_iface_policy.c) и учёт своих сессий. Запрос с сокета интерфейса iface
 * сокетный слой передаёт объекту ЭТОГО интерфейса; если объекта нет, соединение
 * закрывается.
 *
 * Объектов столько, сколько обслуживаемых интерфейсов: в прошивке два — точка
 * доступа и станция. Все пути объекта назначаются его методами, и транспорт он
 * порождает сам. */

typedef struct web_server web_server_t;

/* Порт, к которому относится маршрут: страницы и файлы отдаёт httpd на
 * CONFIG_WEB_SERVER_PORT, WebSocket — httpd на CONFIG_WEB_SOCKET_SERVER_PORT. */
typedef enum {
    WEB_ROUTE_HTTP = 0,   /* страницы, файлы, прошивка */
    WEB_ROUTE_WS   = 1,   /* WebSocket-точки */
    WEB_ROUTE_KINDS
} web_route_kind_t;

/* Один маршрут веб-сервера: путь, обработчик и то, кому сервис оказан. */
typedef struct {
    const char *uri;                 /* путь или шаблон пути (как в httpd_uri_t) */
    httpd_method_t method;           /* метод запроса */
    esp_err_t (*handler)(httpd_req_t *req);
    void *user_ctx;                  /* контекст, с которым обработчик регистрировали */
    uint32_t iface_mask;             /* GW_IFACE_MASK_* — кому этот сервис оказан */
    web_route_kind_t kind;           /* порт: страницы или WebSocket */
    uint8_t is_websocket;            /* путь — WebSocket-точка */
    uint8_t handle_ws_control_frames;/* PING/PONG/CLOSE отдаются обработчику */
    esp_err_t (*post_handshake)(httpd_req_t *req);  /* сессия создана (IDF >= 6) */
} web_route_t;

/* Колбэки WebSocket-точки. Их даёт транспорт: ему веб-сервер отдаёт соединение и
 * кадры. Колбэки принадлежат ЭКЗЕМПЛЯРУ транспорта, а не пути — обработчик находит
 * свой экземпляр по контексту активации. */
typedef struct {
    esp_err_t (*handler)(httpd_req_t *req);          /* GET-рукопожатие и кадры */
    esp_err_t (*post_handshake)(httpd_req_t *req);   /* рукопожатие отвечено */
    uint8_t handle_control_frames;                   /* PING/PONG/CLOSE — обработчику */
} web_ws_callbacks_t;

/* Поднять объект веб-сервера интерфейса iface (GW_IFACE_AP/GW_IFACE_STA/...).
 * NULL — объект не поднялся (нет памяти, порты не запущены или такой интерфейс
 * не обслуживается). С этого момента соединения интерфейса попадают в него. */
web_server_t *web_server_open(uint32_t iface);

/* Зарегистрировать в объекте URI из статической таблицы httpd_uri_t (страницы,
 * файлы, прошивка). Кому сервис оказан, решает политика по шаблону пути, и отказ
 * отдаётся при запросе, а не при регистрации. */
uint8_t web_server_register_uri(web_server_t *srv, const httpd_uri_t *uri);

/* Погасить объект: соединения интерфейса перестают обслуживаться, пути снимаются
 * с httpd, порождённые транспорты и память освобождаются. */
void web_server_close(web_server_t *srv);

/* Интерфейс, который обслуживает объект. */
uint32_t web_server_iface(const web_server_t *srv);

/* ── Методы для транспорта ───────────────────────────────────────────────────
 *
 * Зовёт транспорт, порождённый ws_transport_create: своего httpd у него нет, и
 * работу с соединениями он ведёт через объект веб-сервера. */

/* Привязать порождённый транспорт к объекту (зовёт open транспорта). */
void web_server_transport_attach(web_server_t *srv, transport_t *transport);

/* Занять путь за интерфейсом объекта под порт WebSocket.
 * 0 — путь уже занят или нет места. */
uint8_t web_server_claim_ws(web_server_t *srv, const char *uri);

/* Активировать закреплённый за объектом URI: с этого момента веб-сервер отдаёт
 * колбэкам соединения и кадры и передаёт им context.
 * 0 — URI объекту не закреплён или уже активирован. */
uint8_t web_server_activate_ws(web_server_t *srv, const char *uri,
                               const web_ws_callbacks_t *callbacks, void *context);

/* Зовутся из web_httpd.c: сокетный слой знает соединение, объект — что этому
 * соединению оказано. */

/* Обработать запрос: найти маршрут по шаблону, который выбрал httpd, сверить
 * политику интерфейса и вызвать обработчик с его собственным user_ctx. Шаблона
 * может не быть в этом объекте (таблица порта общая для интерфейсов): тогда
 * маршрут ищется по пути запроса в таблице объекта, как это делает httpd. Отказ —
 * 404, как у несуществующего пути. */
esp_err_t web_server_handle(web_server_t *srv, web_route_kind_t kind,
                            const char *uri, httpd_req_t *req);

/* Обработать состоявшееся рукопожатие WebSocket: ответ httpd уже отправил, поэтому
 * отказать можно только закрытием соединения (ESP_FAIL). Маршрут и его контекст
 * ищутся так же, как в web_server_handle. */
esp_err_t web_server_handshake(web_server_t *srv, web_route_kind_t kind,
                               const char *uri, httpd_req_t *req);

/* Учёт сессий объекта: открыть/снять. Зовётся при установлении и завершении
 * соединения; объект знает, сколько соединений его интерфейса открыто. */
uint8_t  web_server_session_open(web_server_t *srv, int fd);
void     web_server_session_release(web_server_t *srv, int fd);
uint16_t web_server_session_count(const web_server_t *srv);

/* Закрыть соединение fd на своём интерфейсе (им пользуется транспорт, когда
 * соединение пришло в негодность). 1 — закрытие запущено. */
uint8_t web_server_session_close(web_server_t *srv, int fd);

/* Записать кусок в сокет сессии WebSocket, не дольше stall_ms: длина — записано
 * всё, 0 — ничего (кадр можно отбросить), -1 — частично (поток испорчен). */
int web_server_write(web_server_t *srv, int fd, const char *buf, size_t len,
                     uint32_t stall_ms);

/* Отправить кадр WebSocket в соединение fd. done == NULL — без колбэка
 * завершения. Настройки отправки держит сокетный слой. */
esp_err_t web_server_send_ws(web_server_t *srv, int fd, httpd_ws_frame_t *frame,
                             transfer_complete_cb done, void *arg);

#ifdef __cplusplus
}
#endif
