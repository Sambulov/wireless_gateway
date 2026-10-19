#pragma once

/* Заглушка транспорта для хостовых тестов: полноценная реализация
 * transport_iface_t, которая принимает кадры «извне» и отдаёт отправленные
 * сообщения наружу. Ведёт себя как настоящий транспорт (main/ws_transport.c):
 * склеивает фрагменты в сообщение, сам отвечает на управляющие кадры, ядру
 * отдаёт только готовые СООБЩЕНИЯ и завершения отправок. */

#include "api.h"
#include "transport.h"
#include <stddef.h>
#include "net_iface.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Пределы те же, что у настоящего транспорта. */
#define TRANSPORT_STUB_FRAME_MAX_LEN   16384
#define TRANSPORT_STUB_MESSAGE_MAX_LEN 16384

typedef struct ws_transport_stub_conn ws_transport_stub_conn_t;

/* Кадры уровня WebSocket — то, что транспорт получает от сети. */
typedef enum {
    WS_STUB_FRAME_CONT  = 0x00,   /* продолжение фрагментированного сообщения */
    WS_STUB_FRAME_TEXT  = 0x01,
    WS_STUB_FRAME_CLOSE = 0x08,
    WS_STUB_FRAME_PING  = 0x09,
    WS_STUB_FRAME_PONG  = 0x0A,
} ws_transport_stub_frame_t;

/* Модуль транспорта: то, что настоящий транспорт держит как свой контекст
 * реализации.
 *
 * Заглушка повторяет устройство настоящего транспорта: у неё ДВА экземпляра — по
 * одному на веб-сервер интерфейса (точка доступа и станция), как два транспорта,
 * порождённых разными объектами веб-сервера (web_server_open_websocket). Соединение
 * обслуживает экземпляр своего интерфейса, а значит и подписанное на него ядро.
 * Идентификатор экземпляра — указатель на его контекст (transport_t.context). */
const transport_iface_t *ws_transport_stub_iface(void);
/* Модуль интерфейса iface (NULL — такого интерфейса у заглушки нет). Нужен,
 * чтобы собрать transport_t вручную — например, с неполной таблицей методов. */
void                       *ws_transport_stub_module(uint32_t iface);
/* Объект транспорта интерфейса iface — то, что получает api_init. NULL, если
 * такого интерфейса у заглушки нет. */
const transport_t        *ws_transport_stub_object(uint32_t iface);

/* Поднять ядро над объектом точки доступа — так живёт большинство тестов,
 * которым нужен один работающий транспорт. Возвращает дескриптор ядра или NULL;
 * освобождает его api_deinit. */
api_t                    *ws_transport_stub_init(void);
/* То же, но над объектом конкретного интерфейса: второй интерфейс остаётся
 * ничьим, и его соединения, как и в прошивке, закрываются. */
api_t                    *ws_transport_stub_init_only(uint32_t iface);

/* Соединение: connect отдаёт ESTABLISHED, disconnect — CLOSED. Соединение
 * попадает к экземпляру своего интерфейса; если на этот интерфейс ядро не
 * подписано, соединение считается закрытым — как у настоящего транспорта. */
ws_transport_stub_conn_t *ws_transport_stub_connect(int fd);
/* Соединение, о котором ядру не сообщали: проверяем, что события из него
 * (сообщение, CLOSED) ядро переживает, не зная такого соединения. */
ws_transport_stub_conn_t *ws_transport_stub_connect_silent(int fd);
void                      ws_transport_stub_disconnect(ws_transport_stub_conn_t *conn);

/* Через какой интерфейс пришло соединение (GW_IFACE_*). Стаб, как и настоящий
 * транспорт, фиксирует его при установлении: set_iface() задаёт интерфейс
 * СЛЕДУЮЩИХ соединений (по умолчанию GW_IFACE_AP), conn_iface() — что получилось
 * у конкретного. */
void     ws_transport_stub_set_iface(uint32_t iface);
uint32_t ws_transport_stub_conn_iface(ws_transport_stub_conn_t *conn);

/* Кадр из сети. final == 0 — сообщение продолжится (транспорт копит фрагменты). */
void ws_transport_stub_frame(ws_transport_stub_conn_t *conn, ws_transport_stub_frame_t type,
                             const void *data, size_t len, int final);

/* Отправка наружу. */
void        ws_transport_stub_set_send_result(ws_transport_stub_conn_t *conn,
                                             transport_result_t res);
int         ws_transport_stub_send_attempts(ws_transport_stub_conn_t *conn);
const char *ws_transport_stub_last_sent(ws_transport_stub_conn_t *conn);
int         ws_transport_stub_delivered(ws_transport_stub_conn_t *conn);

/* Отложенные завершения: при deferred == 1 begin_send копит отправки, а
 * завершение отдаёт ws_transport_stub_complete_pending() — как это делает
 * задача httpd. res == TRANSPORT_OK даёт TRANSFER_COMPLETE, иначе TRANSFER_ERR. */
void ws_transport_stub_set_deferred(ws_transport_stub_conn_t *conn, int on);
int  ws_transport_stub_pending_count(ws_transport_stub_conn_t *conn);
void ws_transport_stub_complete_pending(ws_transport_stub_conn_t *conn, transport_result_t res);
/* Адрес буфера, который ядро отдало в begin_send (в режиме deferred — по
 * индексу). Тесту это нужно, чтобы проверить, что один и тот же буфер ушёл
 * нескольким подписчикам, а не был собран каждому свой. */
const void *ws_transport_stub_pending_data(ws_transport_stub_conn_t *conn, int idx);

/* Отдать ядру CLOSED ещё раз, даже если соединение уже закрыто: транспорт вправе
 * сообщить о разрыве дважды, и ядро обязано пережить это без двойного
 * освобождения. */
void ws_transport_stub_emit_closed(ws_transport_stub_conn_t *conn);

/* Что транспорт сделал сам, без ядра. */
int  ws_transport_stub_close_calls(ws_transport_stub_conn_t *conn);   /* close() позвало ядро */
int  ws_transport_stub_reply_calls(ws_transport_stub_conn_t *conn);   /* ответы на PING/CLOSE */
int  ws_transport_stub_closed(ws_transport_stub_conn_t *conn);

/* Полный сброс стаба между тестами: отложенные отправки завершаются ошибкой
 * (чтобы ядро освободило свои буферы), дескрипторы соединений освобождаются. */
void ws_transport_stub_reset(void);

#ifdef __cplusplus
}
#endif
