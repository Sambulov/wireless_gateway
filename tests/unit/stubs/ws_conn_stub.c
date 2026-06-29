#include "ws_transport.h"
#include <stdlib.h>

/*
 * Minimal ws_conn stub for unit tests.
 * ws_conn_t is opaque — tests create instances via ws_conn_stub_make().
 */

struct ws_conn {
    int   fd;
    void *user_data;
    int   send_async_calls;
    int   send_async_last_err; /* return value for next ws_conn_send_async call */
    int   close_calls;
};

ws_conn_t *ws_conn_stub_make(int fd) {
    ws_conn_t *c = calloc(1, sizeof(ws_conn_t));
    if (c) c->fd = fd;
    return c;
}

void ws_conn_stub_set_send_result(ws_conn_t *c, int err) {
    if (c) c->send_async_last_err = err;
}

int ws_conn_stub_send_calls(ws_conn_t *c) {
    return c ? c->send_async_calls : 0;
}

int ws_conn_stub_close_calls(ws_conn_t *c) {
    return c ? c->close_calls : 0;
}

/* --- ws_transport.h interface ------------------------------------------ */

int ws_conn_get_fd(ws_conn_t *conn) {
    return conn ? conn->fd : -1;
}

int ws_conn_send_async(ws_conn_t *conn, const ws_frame_t *frame,
                       ws_send_done_cb_t cb, void *cb_arg) {
    if (!conn) return -1;
    int err = conn->send_async_last_err;
    conn->send_async_calls++;
    if (cb) cb(err, conn->fd, cb_arg);
    return err;
}

void ws_conn_close(ws_conn_t *conn) {
    if (conn) conn->close_calls++;
}

void ws_conn_set_user_data(ws_conn_t *conn, void *data) {
    if (conn) conn->user_data = data;
}

void *ws_conn_get_user_data(ws_conn_t *conn) {
    return conn ? conn->user_data : NULL;
}
