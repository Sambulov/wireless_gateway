#pragma once

/* Заглушка транспорта для хостовых тестов: полноценная реализация
 * ws_transport_iface_t, которая принимает кадры «извне» и отдаёт отправленные
 * сообщения наружу. Ведёт себя как настоящий транспорт (main/ws_transport_httpd.c):
 * склеивает фрагменты в сообщение, сам отвечает на управляющие кадры, ядру
 * отдаёт только готовые СООБЩЕНИЯ и завершения отправок. */

#include "ws_transport.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Пределы те же, что у настоящего транспорта. */
#define WS_TRANSPORT_STUB_FRAME_MAX_LEN   16384
#define WS_TRANSPORT_STUB_MESSAGE_MAX_LEN 16384

typedef struct ws_transport_stub_conn ws_transport_stub_conn_t;

/* Кадры уровня WebSocket — то, что транспорт получает от сети. */
typedef enum {
    WS_STUB_FRAME_CONT  = 0x00,   /* продолжение фрагментированного сообщения */
    WS_STUB_FRAME_TEXT  = 0x01,
    WS_STUB_FRAME_CLOSE = 0x08,
    WS_STUB_FRAME_PING  = 0x09,
    WS_STUB_FRAME_PONG  = 0x0A,
} ws_transport_stub_frame_t;

/* Модуль транспорта: то, что настоящий транспорт отдаёт ядру как self. */
const ws_transport_iface_t *ws_transport_stub_iface(void);
void                       *ws_transport_stub_module(void);
/* ws_server_transport_attach(iface, module) — подключить заглушку к ядру. */
ws_transport_result_t       ws_transport_stub_attach(void);

/* Соединение: connect отдаёт ядру ESTABLISHED, disconnect — CLOSED. */
ws_transport_stub_conn_t *ws_transport_stub_connect(int fd);
/* Соединение, о котором ядру не сообщали: проверяем, что события из него
 * (сообщение, CLOSED) ядро переживает, не зная такого соединения. */
ws_transport_stub_conn_t *ws_transport_stub_connect_silent(int fd);
void                      ws_transport_stub_disconnect(ws_transport_stub_conn_t *conn);

/* Кадр из сети. final == 0 — сообщение продолжится (транспорт копит фрагменты). */
void ws_transport_stub_frame(ws_transport_stub_conn_t *conn, ws_transport_stub_frame_t type,
                             const void *data, size_t len, int final);

/* Отправка наружу. */
void        ws_transport_stub_set_send_result(ws_transport_stub_conn_t *conn,
                                             ws_transport_result_t res);
int         ws_transport_stub_send_attempts(ws_transport_stub_conn_t *conn);
const char *ws_transport_stub_last_sent(ws_transport_stub_conn_t *conn);
int         ws_transport_stub_delivered(ws_transport_stub_conn_t *conn);

/* Отложенные завершения: при deferred == 1 begin_send копит отправки, а
 * завершение отдаёт ws_transport_stub_complete_pending() — как это делает
 * задача httpd. res == WS_TRANSPORT_OK даёт TRANSFER_COMPLETE, иначе TRANSFER_ERR. */
void ws_transport_stub_set_deferred(ws_transport_stub_conn_t *conn, int on);
int  ws_transport_stub_pending_count(ws_transport_stub_conn_t *conn);
void ws_transport_stub_complete_pending(ws_transport_stub_conn_t *conn, ws_transport_result_t res);

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
