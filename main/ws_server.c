/* WebSocket Echo Server Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_system.h>
#include <nvs_flash.h>
#include <sys/param.h>
#include <sys/stat.h>
#include "esp_netif.h"
#include "esp_eth.h"
#include "protocol_examples_common.h"
#include "ws_api.h"
#include "esp_littlefs.h"
#include <tftp_server_wg.h>
#include "cJSON.h"

#include <esp_http_server.h>
#include <stdio.h>

#include "app.h"

#include "web_api.h"

// typedef struct {
//     __LinkedListObject__
//     uint8_t *pucReqData;
//     uint32_t ulReqDataLen;
//      *pxWsc;
//     uint32_t ulTimestamp;
//     uint32_t ulId;
//     void *pxHandlerContext;
//     WsApiHandler_t *pxApiEP;
//     uint8_t ucStatus;
//   } WsApiCall_t;

/* A simple example that demonstrates using websocket echo server
 */
static const char *TAG = "ws_echo_server";

/*
 * Structure holding server handle
 * and internal socket fd in order
 * to use out of request send
 */
struct async_resp_arg {
    httpd_handle_t hd;
    int fd;
};

/*
 * async send function, which we put into the httpd work queue
 */
static void ws_async_send(void *arg)
{
    static const char * data = "Async data";
    struct async_resp_arg *resp_arg = arg;
    httpd_handle_t hd = resp_arg->hd;
    int fd = resp_arg->fd;
    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.payload = (uint8_t*)data;
    ws_pkt.len = strlen(data);
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;

    httpd_ws_send_frame_async(hd, fd, &ws_pkt);
    free(resp_arg);
}

static esp_err_t trigger_async_send(httpd_handle_t handle, httpd_req_t *req)
{
    struct async_resp_arg *resp_arg = malloc(sizeof(struct async_resp_arg));
    if (resp_arg == NULL) {
        return ESP_ERR_NO_MEM;
    }
    resp_arg->hd = req->handle;
    resp_arg->fd = httpd_req_to_sockfd(req);
    esp_err_t ret = httpd_queue_work(handle, ws_async_send, resp_arg);
    if (ret != ESP_OK) {
        free(resp_arg);
    }
    return ret;
}

/*
 * This handler echos back the received ws data
 * and triggers an async send if certain message received
 */
static esp_err_t echo_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {

        ESP_LOGI(TAG, "Handshake done, the new connection was opened");
        return ESP_OK;
    }
    httpd_ws_frame_t ws_pkt;
    uint8_t *buf = NULL;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;
    /* Set max_len = 0 to get the frame len */
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_ws_recv_frame failed to get frame len with %d", ret);
        return ret;
    }
    ESP_LOGI(TAG, "frame len is %d", ws_pkt.len);
    if (ws_pkt.len) {
        /* ws_pkt.len + 1 is for NULL termination as we are expecting a string */
        buf = calloc(1, ws_pkt.len + 1);
        if (buf == NULL) {
            ESP_LOGE(TAG, "Failed to calloc memory for buf");
            return ESP_ERR_NO_MEM;
        }
        ws_pkt.payload = buf;
        /* Set max_len = ws_pkt.len to get the frame payload */
        ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "httpd_ws_recv_frame failed with %d", ret);
            free(buf);
            return ret;
        }
        ESP_LOGI(TAG, "Got packet with message: %s", ws_pkt.payload);
    }
    ESP_LOGI(TAG, "Packet type: %d", ws_pkt.type);
    if (ws_pkt.type == HTTPD_WS_TYPE_TEXT &&
        strcmp((char*)ws_pkt.payload,"Trigger async") == 0) {
        free(buf);
        return trigger_async_send(req->handle, req);
    }

    ret = httpd_ws_send_frame(req, &ws_pkt);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_ws_send_frame failed with %d", ret);
    }
    free(buf);
    return ret;
}

// Обработчик WebSocket-сообщений
// esp_err_t ws_handler(httpd_req_t *req) {
//     if (req->method == HTTP_GET) {
//         ESP_LOGI(TAG, "WebSocket connection established");
//         return ESP_OK;
//     }

//     uint8_t buf[512];
//     int len = httpd_ws_recv_frame(req, buf, sizeof(buf));
//     if (len < 0) {
//         ESP_LOGE(TAG, "WebSocket receive failed");
//         return ESP_FAIL;
//     }

//     // Обработка полученного сообщения
//     buf[len] = '\0';
//     ESP_LOGI(TAG, "WebSocket message received: %s", buf);

//     // Парсинг JSON
//     cJSON *json = cJSON_Parse((char *)buf);
//     if (json == NULL) {
//         ESP_LOGE(TAG, "Invalid JSON");
//         return ESP_FAIL;
//     }

//     // Обработка команды для настройки UART
//     cJSON *baud_rate_json = cJSON_GetObjectItem(json, "baud_rate");
//     if (baud_rate_json != NULL) {
//         int baud_rate = baud_rate_json->valueint;

//         uart_reconfigure(baud_rate);
//     }

//     // Обработка команды для настройки Wi-Fi STA
//     cJSON *wifi_ssid_json = cJSON_GetObjectItem(json, "wifi_ssid");
//     cJSON *wifi_pass_json = cJSON_GetObjectItem(json, "wifi_pass");
//     if (wifi_ssid_json != NULL && wifi_pass_json != NULL) {
//         const char *ssid = wifi_ssid_json->valuestring;
//         const char *pass = wifi_pass_json->valuestring;
//         save_wifi_config(ssid, pass);
//         wifi_init_ap_sta(ssid, pass);
//     }

//     // Обработка команды для отправки данных на UART
//     cJSON *uart_data_json = cJSON_GetObjectItem(json, "uart_data");
//     if (uart_data_json != NULL) {
//         const char *uart_data = uart_data_json->valuestring;
//         uart_send_data(uart_data, strlen(uart_data));
//     }

//     cJSON_Delete(json);
//     return ESP_OK;
// }

// // Задача для чтения данных с UART и отправки через WebSocket
// void uart_to_websocket_send(app_context_t *context, const char *data, size_t len) {
//     if (server != NULL) {
//         httpd_ws_frame_t ws_pkt = {
//             .final = true,
//             .fragmented = false,
//             .type = HTTPD_WS_TYPE_TEXT,
//             .payload = data,
//             .len = len,
//         };
//         httpd_ws_send_frame_async(context->web_server, httpd_req_to_sockfd(server->hd_req), &ws_pkt);
//     }
// }

#include "app.h"

httpd_uri_t ws = {
    .uri        = "/ws", // http://<ip>/ws -> ws://<ip>/ws
    .method     = HTTP_GET,
    .handler    = echo_handler,
    .user_ctx   = NULL,
    .is_websocket = true
};

void ws_init(app_context_t *context) {
    ws.user_ctx = context;
}