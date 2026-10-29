#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "esp_http_server.h"

#include "web_server.h"
#include "transport.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Сокетный слой веб-сервера: приём соединений, TCP_NODELAY, бюджеты отправки и
 * учёт прогресса по сессиям.
 *
 * Экземпляр заводится НА ПОРТ, а не на интерфейс: httpd_config_t не умеет
 * привязываться к адресу, поэтому один экземпляр слушает INADDR_ANY, а развод по
 * интерфейсу делает порт — соединение получает объект веб-сервера своего
 * интерфейса (web_server.c). Портов два: страницы и файлы — на
 * CONFIG_WEB_SERVER_PORT, WebSocket — на CONFIG_WEB_SOCKET_SERVER_PORT. */

typedef struct web_httpd web_httpd_t;

/* Настройки одного порта. */
typedef struct {
    uint16_t port;               /* порт, который слушает httpd */
    uint16_t max_open_sockets;   /* сколько сессий держит экземпляр */
    uint16_t max_uri_handlers;   /* предел зарегистрированных путей в httpd */
    uint32_t send_budget_ms;     /* бюджет одной отправки, см. web_httpd.c */
} web_httpd_config_t;

/* ── Бюджеты отправки ────────────────────────────────────────────────────────
 *
 * У одной отправки есть бюджет: как только байты пошли, вызов возвращается, и
 * бюджет начинается заново. То есть бюджет — это сколько терпим ПОЛНОЕ молчание
 * сокета, а не сколько длится передача.
 *
 * Задача httpd одна на порт: пока она ждёт молчащий сокет, остальные сессии
 * порта не обслуживаются. Поэтому терпение короткое, а не «пять секунд на
 * всякий случай»: нечитающий клиент (фоновая вкладка с полным буфером) должен
 * отваливаться за доли секунды, иначе он замораживает порт целиком — это и
 * выглядело как «страница недогружается». Клиенту, которому действительно тесно
 * в Wi-Fi, хватает и этого: подтверждения идут десятки миллисекунд. */
#define WEB_SEND_BUDGET_FILE_MS  1000
#define WEB_SEND_BUDGET_WS_MS    5000

/* Поднять по одному httpd на каждый порт из таблицы config (индексы —
 * web_route_kind_t). 1 — поднялись все. */
uint8_t web_httpd_start(const web_httpd_config_t config[WEB_ROUTE_KINDS]);

/* Погасить httpd всех портов. */
void web_httpd_stop(void);

/* Порт по назначению маршрута; NULL — этот порт не поднят. */
web_httpd_t *web_httpd_port(web_route_kind_t kind);

/* У порта свой набор объектов — по одному на интерфейс. Привязку делает
 * web_server_open (объект приходит к своему порту сам). */

/* Привязать объект к порту. 0 — на этот интерфейс порт уже привязан. */
uint8_t web_httpd_attach(web_httpd_t *port, web_server_t *srv);

/* Отвязать объект: соединения интерфейса перестают попадать в него. */
void web_httpd_detach(web_httpd_t *port, web_server_t *srv);

/* Объект веб-сервера интерфейса iface; NULL — объект не поднят. */
web_server_t *web_httpd_server(web_httpd_t *port, uint32_t iface);

/* Путь регистрирует ОБЪЕКТ, но httpd у порта один на всех, поэтому порт помнит,
 * сколько объектов предложили путь: в httpd он попадает один раз, а снимается,
 * когда его сняли все. */

/* Зарегистрировать путь маршрута в httpd порта. 0 — не вышло. */
uint8_t web_httpd_register(web_httpd_t *port, const web_route_t *route);

/* Снять путь, снятый одним объектом; из httpd путь уходит, когда его сняли все. */
uint8_t web_httpd_unregister(web_httpd_t *port, const char *uri, httpd_method_t method);

/* ── Сессии ────────────────────────────────────────────────────────────────── */

/* Закрыть соединение fd на этом порту. 1 — закрытие запущено. */
uint8_t web_httpd_kick(web_httpd_t *port, int fd);

/* Записать кусок в сокет сессии: длина — записано всё; 0 — не записано ничего
 * (кадр можно отбросить целиком, сеанс жив); -1 — записано частично или сокет
 * сломан (поток испорчен, сеанс надо закрывать). Ждёт не дольше stall_ms. */
int web_httpd_write(web_httpd_t *port, int fd, const char *buf, size_t len,
                    uint32_t stall_ms);

/* Отправить кадр WebSocket в соединение fd. done == NULL — без колбэка. */
esp_err_t web_httpd_send_ws(web_httpd_t *port, int fd, httpd_ws_frame_t *frame,
                            transfer_complete_cb done, void *arg);

#ifdef __cplusplus
}
#endif
