#pragma once

#include "ws_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

ws_conn_t *ws_conn_stub_make(int fd);
void       ws_conn_stub_set_send_result(ws_conn_t *conn, int err);
int        ws_conn_stub_send_calls(ws_conn_t *conn);
int        ws_conn_stub_close_calls(ws_conn_t *conn);

#ifdef __cplusplus
}
#endif
