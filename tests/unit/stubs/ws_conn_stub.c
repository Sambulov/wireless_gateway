#include "ws_transport.h"
#include <stdlib.h>

/*
 * Minimal ws_conn stub for unit tests.
 * ws_conn_t is opaque — tests create instances via ws_conn_stub_make().
 */

typedef struct {
    ws_send_done_cb_t cb;
    void *cb_arg;
} pending_cb_t;

struct ws_conn {
    int   fd;
    void *user_data;
    int   send_async_calls;
    int   send_async_last_err; /* return value for next ws_conn_send_async call */
    int   close_calls;
    /* Deferred mode: completion callbacks are held here until the test runs
     * them with ws_conn_stub_complete_pending(), like the httpd task would. */
    int           deferred;
    pending_cb_t *pending;
    int           pending_count;
    int           pending_cap;
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

void ws_conn_stub_set_deferred(ws_conn_t *c, int on) {
    if (c) c->deferred = on;
}

int ws_conn_stub_pending_count(ws_conn_t *c) {
    return c ? c->pending_count : 0;
}

void ws_conn_stub_complete_pending(ws_conn_t *c, int err) {
    if (!c) return;
    /* A callback may queue new sends; only run the ones pending right now. */
    int n = c->pending_count;
    pending_cb_t *batch = c->pending;
    c->pending = NULL;
    c->pending_count = c->pending_cap = 0;
    for (int i = 0; i < n; i++)
        batch[i].cb(err, c->fd, batch[i].cb_arg);
    free(batch);
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
    /* Same contract as httpd_ws_send_data_async(): if queuing fails the
     * error is returned and the callback is never called. */
    if (err) return err;
    if (!cb) return 0;
    if (conn->deferred) {
        if (conn->pending_count == conn->pending_cap) {
            int cap = conn->pending_cap ? conn->pending_cap * 2 : 8;
            pending_cb_t *p = realloc(conn->pending, cap * sizeof(*p));
            if (!p) return -1;
            conn->pending = p;
            conn->pending_cap = cap;
        }
        conn->pending[conn->pending_count++] = (pending_cb_t){ cb, cb_arg };
    } else {
        cb(0, conn->fd, cb_arg);
    }
    return 0;
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
