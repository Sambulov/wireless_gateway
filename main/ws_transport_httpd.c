#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "CodeLib.h"
#include "connection.h"
#include "ws_transport.h"

static const char *TAG = "ws_transport";

/* ws_server callbacks — defined in ws_server.c */
void ws_server_on_connect(ws_conn_t *conn);
void ws_server_on_frame(ws_conn_t *conn, ws_frame_type_t type, uint8_t *data, size_t len);
void ws_server_on_disconnect(ws_conn_t *conn);

struct ws_conn {
    httpd_handle_t hd;
    int fd;
    delegate_t delegate;
    void *user_data;
};

/* --- ws_conn interface --------------------------------------------------- */

int ws_conn_get_fd(ws_conn_t *conn) {
    return conn->fd;
}

/* esp_err_t and int are ABI-compatible on Xtensa/RISC-V (both int32_t/int),
 * so ws_send_done_cb_t can be cast to httpd_send_func_t safely. */
int ws_conn_send_async(ws_conn_t *conn, const ws_frame_t *frame,
                       ws_send_done_cb_t cb, void *cb_arg) {
    httpd_ws_frame_t f = {
        .type       = (httpd_ws_type_t)frame->type,
        .final      = frame->final,
        .fragmented = frame->fragmented,
        .payload    = frame->payload,
        .len        = frame->len,
    };
    if (cb)
        return httpd_ws_send_data_async(conn->hd, conn->fd, &f,
                                        (transfer_complete_cb)cb, cb_arg);
    return httpd_ws_send_frame_async(conn->hd, conn->fd, &f);
}

void ws_conn_close(ws_conn_t *conn) {
    httpd_sess_trigger_close(conn->hd, conn->fd);
}

void ws_conn_set_user_data(ws_conn_t *conn, void *data) {
    conn->user_data = data;
}

void *ws_conn_get_user_data(ws_conn_t *conn) {
    return conn->user_data;
}

/* --- Internal httpd lifecycle -------------------------------------------- */

static void link_event_handler(void *event_trigger, void *sender, void *context) {
    (void)event_trigger; (void)sender;
    ws_conn_t *conn = (ws_conn_t *)context;
    ESP_LOGI(TAG, "Link down, triggering close for fd=%d", conn->fd);
    httpd_sess_trigger_close(conn->hd, conn->fd);
}

static void free_conn(void *ctx) {
    ws_conn_t *conn = (ws_conn_t *)ctx;
    ESP_LOGI(TAG, "Free conn fd=%d", conn->fd);
    event_unsubscribe(&conn->delegate);
    ws_server_on_disconnect(conn);
    free(conn);
}

static esp_err_t ws_httpd_handler(httpd_req_t *req) {
    int fd = httpd_req_to_sockfd(req);

    if (req->method == HTTP_GET) {
        ws_conn_t *conn = malloc(sizeof(ws_conn_t));
        if (!conn) return ESP_ERR_NO_MEM;
        conn->hd        = req->handle;
        conn->fd        = fd;
        conn->user_data = NULL;
        conn->delegate.handler = link_event_handler;
        conn->delegate.context = conn;
        socket_link_subscribe(fd, &conn->delegate);
        req->sess_ctx = conn;
        req->free_ctx = free_conn;
        ws_server_on_connect(conn);
        return ESP_OK;
    }

    ws_conn_t *conn = (ws_conn_t *)req->sess_ctx;

    httpd_ws_frame_t f = {0};
    esp_err_t ret = httpd_ws_recv_frame(req, &f, 0);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to read ws header (err %d)", ret);
        return ret;
    }

    uint8_t *payload = NULL;
    if (f.len) {
        payload = malloc(f.len + 1);
        if (!payload) return ESP_ERR_NO_MEM;
        payload[f.len] = '\0';
        f.payload = payload;
        ret = httpd_ws_recv_frame(req, &f, f.len);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to read ws payload (err %d)", ret);
            free(payload);
            return ret;
        }
    }

    /* All frame types delivered to ws_server — protocol responses (PONG, CLOSE)
     * are its responsibility. Ownership of payload transfers to ws_server_on_frame. */
    ws_server_on_frame(conn, (ws_frame_type_t)f.type, payload, f.len);
    return ESP_OK;
}

/* --- Public init --------------------------------------------------------- */

httpd_uri_t *ws_transport_httpd_init(char *uri) {
    httpd_uri_t *h = malloc(sizeof(httpd_uri_t));
    if (!h) return NULL;
    memset(h, 0, sizeof(httpd_uri_t));
    h->uri                      = uri;
    h->method                   = HTTP_GET;
    h->handler                  = ws_httpd_handler;
    h->is_websocket             = true;
    h->handle_ws_control_frames = true;
    return h;
}
