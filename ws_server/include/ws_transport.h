#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef enum {
    WS_FRAME_CONT   = 0x00,
    WS_FRAME_TEXT   = 0x01,
    WS_FRAME_BINARY = 0x02,
    WS_FRAME_CLOSE  = 0x08,
    WS_FRAME_PING   = 0x09,
    WS_FRAME_PONG   = 0x0A,
} ws_frame_type_t;

typedef struct {
    ws_frame_type_t  type;
    bool      final;
    bool      fragmented;
    uint8_t  *payload;
    size_t    len;
} ws_frame_t;

typedef void (*ws_send_done_cb_t)(int err, int fd, void *arg);

/* Opaque per-connection handle — defined in each transport implementation */
typedef struct ws_conn ws_conn_t;

int  ws_conn_get_fd(ws_conn_t *conn);
int  ws_conn_send_async(ws_conn_t *conn, const ws_frame_t *frame,
                        ws_send_done_cb_t cb, void *cb_arg);
void ws_conn_close(ws_conn_t *conn);
void ws_conn_set_user_data(ws_conn_t *conn, void *data);
void *ws_conn_get_user_data(ws_conn_t *conn);
