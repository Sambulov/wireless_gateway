#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "esp_http_server.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "CodeLib.h"
#include "connection.h"
#include "ws_transport.h"

static const char *TAG = "ws_transport";

/* esp_http_server answers the WebSocket handshake itself since ESP-IDF v6 and
 * returns before invoking the URI handler ("If the request is websocket
 * handshake, then do not call the uri->handler", httpd_uri.c). The handshake
 * request is handed to ws_post_handshake_cb instead, so the per-session
 * ws_conn_t has to be allocated from that callback. Up to v5.x the handler was
 * still called with method == HTTP_GET, which is where the allocation used to
 * live; that path is kept for older IDF versions. */
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
#define WS_CONN_FROM_POST_HANDSHAKE_CB 1
#else
#define WS_CONN_FROM_POST_HANDSHAKE_CB 0
#endif

#if WS_CONN_FROM_POST_HANDSHAKE_CB && !CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT
#error "ESP-IDF >= 6.0 never calls the URI handler for the WebSocket handshake: enable CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT"
#endif

/* ws_server callbacks — defined in ws_server.c */
void ws_server_on_connect(ws_conn_t *conn);
void ws_server_on_frame(ws_conn_t *conn, ws_frame_type_t type, uint8_t *data, size_t len, uint8_t bFinal);
void ws_server_on_disconnect(ws_conn_t *conn);

/* Предел размера входящего кадра. Раньше его не было вовсе: длина бралась из
 * заголовка кадра и под неё сразу выделялась память, а дальше кадр уходил в
 * разбор. Проверено на устройстве: кадр с вложенным JSON ронял его — разбор
 * рекурсивный, и стек задачи httpd (4 КБ) кончался на глубине ~400, что давало
 * «stack overflow … Rebooting», а вложенные объекты — панику LoadProhibited.
 *
 * 16 КБ — по просьбе: столько же, сколько разрешено собрать из фрагментов, чтобы
 * одиночный кадр и разрезанное сообщение имели одинаковый предел. Штатные
 * запросы сильно меньше: запрос регистров ~80 байт, строка терминала — base64
 * введённого текста, список файлов карт — сотни байт. */
#define WS_MAX_FRAME_LEN 16384

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

/* Allocates the connection object once per session and hands it to ws_server.
 * It is stored in req->sess_ctx: esp_http_server propagates that into the
 * session on request cleanup and copies it back into every later request of the
 * same session, while free_conn releases it when the session goes away. */
static ws_conn_t *ws_conn_open(httpd_req_t *req, int fd) {
    ws_conn_t *conn = (ws_conn_t *)req->sess_ctx;
    if (conn) return conn;

    conn = malloc(sizeof(ws_conn_t));
    if (!conn) return NULL;
    conn->hd        = req->handle;
    conn->fd        = fd;
    conn->user_data = NULL;
    conn->delegate.handler = link_event_handler;
    conn->delegate.context = conn;
    socket_link_subscribe(fd, &conn->delegate);
    req->sess_ctx = conn;
    req->free_ctx = free_conn;
    ws_server_on_connect(conn);
    return conn;
}

#if WS_CONN_FROM_POST_HANDSHAKE_CB
/* The handshake response has already been sent when this runs; the socket is a
 * WebSocket from here on. */
static esp_err_t ws_httpd_post_handshake(httpd_req_t *req) {
    int fd = httpd_req_to_sockfd(req);
    if (!ws_conn_open(req, fd)) {
        ESP_LOGE(TAG, "No memory for WS session, fd=%d", fd);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "WS session opened, fd=%d", fd);
    return ESP_OK;
}
#endif

static esp_err_t ws_httpd_handler(httpd_req_t *req) {
    int fd = httpd_req_to_sockfd(req);

    if (req->method == HTTP_GET) {
#if WS_CONN_FROM_POST_HANDSHAKE_CB
        /* Handshake already answered by esp_http_server; the session was created
         * in ws_httpd_post_handshake() above. */
        return ESP_OK;
#else
        if (!ws_conn_open(req, fd)) return ESP_ERR_NO_MEM;
        return ESP_OK;
#endif
    }

    ws_conn_t *conn = (ws_conn_t *)req->sess_ctx;
    if (!conn) {
        /* Without the session object there is no one to dispatch to; dropping
         * the frame is better than dereferencing NULL. */
        ESP_LOGW(TAG, "WS frame without session context, fd=%d", fd);
        return ESP_FAIL;
    }

    httpd_ws_frame_t f = {0};
    esp_err_t ret = httpd_ws_recv_frame(req, &f, 0);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to read ws header (err %d)", ret);
        return ret;
    }

    /* Кадр больше предела: не читаем и не разбираем. Клиент получает закрытие
     * (RFC 6455 §7.4.1, код 1009 — сообщение слишком велико) вместо ответа API,
     * потому что FID и SID этого кадра мы намеренно не разбираем. */
    if (f.len > WS_MAX_FRAME_LEN) {
        ESP_LOGW(TAG, "Кадр %u байт больше предела %d — закрываю сессию fd=%d",
                 (unsigned)f.len, WS_MAX_FRAME_LEN, fd);
        ws_server_on_frame(conn, WS_FRAME_CLOSE, NULL, 0, 1);
        httpd_sess_trigger_close(req->handle, fd);
        return ESP_OK;
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
    /* bFinal — признак последнего фрагмента (бит FIN): сообщение может прийти
     * по частям, и склеивает их каркас (см. ws_server_on_frame). */
    ws_server_on_frame(conn, (ws_frame_type_t)f.type, payload, f.len, f.final ? 1 : 0);
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
#if WS_CONN_FROM_POST_HANDSHAKE_CB
    h->ws_post_handshake_cb     = ws_httpd_post_handshake;
#endif
    return h;
}
