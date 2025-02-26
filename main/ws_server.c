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

#include "freertos/semphr.h"
#include "freertos/task.h"

#include <esp_http_server.h>
#include <stdio.h>

#include "app.h"

#include "web_api.h"

static const char *TAG = "ws_server";

#define NEW_CALL_QUEUE_LENGTH           20
#define NEW_CALL_ITEM_SIZE              sizeof(void *)

#define WS_FRAME_LEN_MAX 2000

#define API_CALL_STATUS_NEW     0
#define API_CALL_STATUS_WRK     1
#define API_CALL_STATUS_FIN     2

typedef struct {
    __LinkedListObject__
    char *pucReqData;
    uint32_t ulReqDataLen;
    struct {
        httpd_handle_t hd;
        int fd;
    } pxWsc;
    uint32_t ulTimestamp;
    uint32_t ulId;
    void *pxHandlerContext;
    WsApiHandler_t *pxApiEP;
    uint8_t ucStatus;
} WsApiCall_t;

static esp_err_t ws_handler(httpd_req_t *req);

httpd_uri_t ws = {
    .uri        = "/ws", // http://<ip>/ws -> ws://<ip>/ws
    .method     = HTTP_GET,
    .handler    = ws_handler,
    .user_ctx   = NULL,
    .is_websocket = true
};

static SemaphoreHandle_t xWsApiMutex = NULL;
static LinkedList_t pxWsApiHandlers = NULL;
static LinkedList_t pxWsApiCall = NULL;
static QueueHandle_t xWsApiCallNewQueue = NULL;


static uint8_t bHandlerFidMatch(LinkedListItem_t *item, void *arg) {
    uint32_t fid = (uint32_t)arg;
    return LinkedListGetObject(WsApiHandler_t, item)->ulFunctionId == fid;
}

static uint8_t bCallFidMatch(LinkedListItem_t *item, void *arg) {
    uint32_t fd = ((uint32_t *)arg)[0];
    uint32_t fid = ((uint32_t *)arg)[1];
    WsApiCall_t *call = LinkedListGetObject(WsApiCall_t, item);
    return (call->pxWsc.fd == fd) && (call->pxApiEP->ulFunctionId == fid);
}

static uint8_t bCallIdMatch(LinkedListItem_t *item, void *arg) {
    uint32_t cid = (uint32_t)arg;
    WsApiCall_t *call = LinkedListGetObject(WsApiCall_t, item);
    return ((call->ulId == cid) && (call->ucStatus != API_CALL_STATUS_FIN));
}

uint8_t vWebApiRegisterHandler(WsApiHandler_t *handler) {
    if((handler == NULL) || 
       (xSemaphoreTake(xWsApiMutex, pdMS_TO_TICKS(10)) != pdTRUE)) return 0;
    vLinkedListInsertLast(&pxWsApiHandlers, LinkedListItem(handler));
    ESP_LOGI(TAG, "Api handler registered %lu", handler->ulFunctionId);
    xSemaphoreGive(xWsApiMutex);
    return 1;
}

void vWebApiCallComplete(uint32_t ulCallId) {
    xSemaphoreTake(xWsApiMutex, portMAX_DELAY);
    WsApiCall_t *call = LinkedListGetObject(WsApiCall_t, 
        pxLinkedListFindFirst(pxWsApiCall, bCallIdMatch, (void *)ulCallId));
    if(call != NULL) {
        call->ucStatus = API_CALL_STATUS_FIN;
        ESP_LOGI(TAG, "Api call complete %lu", ulCallId);
    }
    xSemaphoreGive(xWsApiMutex);
}

static esp_err_t ws_handler(httpd_req_t *req) {
    static uint32_t call_id = 0;

    esp_err_t ret = ESP_OK;
    WsApiCall_t *wscd = NULL;
    uint8_t *buf = NULL;
    cJSON *json = NULL;
    char *data = NULL;

    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "Handshake done, the new connection was opened");
        return ESP_OK;
    }

    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    /* Set max_len = 0 to get the frame len */
    ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to get WS frame len with %d", ret);
        return ret;
    }

    ESP_LOGI(TAG, "Аrg len is %d", ws_pkt.len);
    if(!ws_pkt.len || (ws_pkt.len > WS_FRAME_LEN_MAX) || (ws_pkt.type != HTTPD_WS_TYPE_TEXT)) {
        ESP_LOGW(TAG, "No data or length too big or bad data type");
        return ESP_FAIL;
    }

    buf = malloc(ws_pkt.len);
    if (buf == NULL) {
        ESP_LOGW(TAG, "Failed to calloc memory for buf");
        return ESP_ERR_NO_MEM;
    }

    ws_pkt.payload = buf;
    /* Set max_len = ws_pkt.len to get the frame payload */
    ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "WS receive failed with %d", ret);
        goto err_exit;
    }

    ws_pkt.payload[ws_pkt.len] = '\0';
    ESP_LOGI(TAG, "Got packet with message: %s", ws_pkt.payload);

    json = cJSON_ParseWithLengthOpts((char *)ws_pkt.payload, ws_pkt.len, 0, 0);
    if (json == NULL) {
        ESP_LOGW(TAG, "Invalid JSON");
        ret = ESP_FAIL;
        goto err_exit;
    }

    cJSON *fid_json = cJSON_GetObjectItem(json, "FID");
    if (!cJSON_IsNumber(fid_json)) {
        ESP_LOGW(TAG, "Bad API arg");
        ret = ESP_FAIL;
        goto err_exit;
    }

    uint32_t fid = fid_json->valueint;
    if(xSemaphoreTake(xWsApiMutex, pdMS_TO_TICKS(10)) != pdTRUE ) {
        ESP_LOGW(TAG, "Mutex take err");
        ret = ESP_FAIL;
        goto err_exit;
    }
    WsApiHandler_t *ap = LinkedListGetObject(WsApiHandler_t, pxLinkedListFindFirst(pxWsApiHandlers, bHandlerFidMatch, (void *)fid));
    xSemaphoreGive(xWsApiMutex);

    if(ap == NULL) {
        ESP_LOGW(TAG, "No API handler");
        ret = ESP_FAIL;
        goto err_exit;
    }

    uint32_t dataLen = 0;
    cJSON *arg_json = cJSON_GetObjectItem(json, "ARG");
    if(arg_json != NULL) {
        if(arg_json->type != cJSON_Object) {
            ESP_LOGI(TAG, "Api call \"arg\" type is not an object");
            ret = ESP_FAIL;
            goto err_exit;
        }
        data = cJSON_PrintUnformatted(arg_json);
        dataLen = strlen(data);
        ESP_LOGI(TAG, "Api arg serialized %s", data);
    }

    wscd = malloc(sizeof(WsApiCall_t));
    if(wscd == NULL){
        ret = ESP_ERR_NO_MEM;
        goto err_exit;
    }

    wscd->pxWsc.fd = httpd_req_to_sockfd(req);
    wscd->pxWsc.hd = req->handle;
    wscd->pxHandlerContext = NULL;
    wscd->pxApiEP = ap;
    wscd->ulTimestamp = 0;
    wscd->pucReqData = data;
    wscd->ulReqDataLen = dataLen;
    wscd->ulId = call_id++;
    wscd->ucStatus = API_CALL_STATUS_NEW;

    if(xQueueSend(xWsApiCallNewQueue, (void *)&wscd, pdMS_TO_TICKS(10) ) != pdPASS ) {
        ret = ESP_ERR_NO_MEM;
        goto err_exit;
    }

    ESP_LOGI(TAG, "New api call %lu enqueued with id:%lu", fid, wscd->ulId);
    goto finally;

err_exit:
    if(wscd != NULL) free(wscd);
    if(data != NULL) free(data);
finally:
    if(buf != NULL) free(buf);
    if(json != NULL) cJSON_Delete(json);
    uint32_t heap = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    ESP_LOGI(TAG, "Heap left:%lu", heap);
   //return ret;
   return ESP_OK; //ignore error, drop packet;
}

static void vWsTransferComplete_cb(esp_err_t err, int socket, void *arg) {
    free(arg);
}

uint8_t bWebApiResponseStatus(uint32_t ulCallId, uint32_t ulSta) {
    esp_err_t res = ESP_FAIL;
    xSemaphoreTake(xWsApiMutex, portMAX_DELAY);
    WsApiCall_t *call = LinkedListGetObject(WsApiCall_t,
        pxLinkedListFindFirst(pxWsApiCall, bCallIdMatch, (void *)ulCallId));
    if(call != NULL) {
        uint8_t *buf = malloc(59);
        if(buf != NULL) {
            sprintf((char *)buf, "{\"FID\":\"0x%08lx\",\"CID\":\"0x%08lx\",\"STA\":\"0x%08lx\"}", call->pxApiEP->ulFunctionId, ulCallId, ulSta);
            httpd_ws_frame_t frame = {
                .fragmented = 0,
                .type = HTTPD_WS_TYPE_TEXT,
                .payload = buf,
                .len = 58
            };
            res = httpd_ws_send_data_async(call->pxWsc.hd, call->pxWsc.fd, &frame, vWsTransferComplete_cb, buf);
        }
        else {
            res = ESP_ERR_NO_MEM;
        }
    }
    xSemaphoreGive(xWsApiMutex);
    ESP_LOGI(TAG, "Api call %lu responce status %lu complete with %d", ulCallId, ulSta, res);
    return (res == ESP_OK);
}

static void vFreeApiCall(LinkedListItem_t *item, void *arg) {
    WsApiCall_t *call = LinkedListGetObject(WsApiCall_t, item);
    if(call->ucStatus == API_CALL_STATUS_FIN) {
        vLinkedListUnlink(item);
        free(call);
    }
}

static void vWsApiCallWorker( void * pvParameters ) {
    for( ;; ) {
        WsApiCall_t *call = NULL;
        if(xQueueReceive(xWsApiCallNewQueue, &call, portMAX_DELAY) == pdPASS ) {
            call->ucStatus = API_CALL_STATUS_WRK;
            xSemaphoreTake(xWsApiMutex, portMAX_DELAY);
            ulLinkedListDoForeach(pxWsApiCall, vFreeApiCall, NULL);
            WsApiCall_t *call_prev = LinkedListGetObject(WsApiCall_t,
                pxLinkedListFindFirst(pxWsApiCall, bCallFidMatch,
                    (void *)((void *[]){ (void *)call->pxWsc.fd, (void *)call->pxApiEP->ulFunctionId})));
            if(call_prev != NULL) {
                call_prev->pucReqData = call->pucReqData;
                call_prev->ulReqDataLen = call->ulReqDataLen;
                free(call);
                call = call_prev;
            }
            else vLinkedListInsertLast(&pxWsApiCall, LinkedListItem(call));
            xSemaphoreGive(xWsApiMutex);            
            uint8_t res = call->pxApiEP->pfHandler(call->ulId, &call->pxHandlerContext, call->pucReqData, call->ulReqDataLen);
            if(call->pucReqData != NULL) free(call->pucReqData);
            call->ulReqDataLen = 0;
            if(res) call->ucStatus = API_CALL_STATUS_FIN;
        }
    }
    vTaskDelete(NULL);
}

void ws_init(app_context_t *context) {
    static StaticQueue_t xWsApiCallNewQueueStat;
    static uint8_t ucWsApiCallNewQueueStorageArea[NEW_CALL_QUEUE_LENGTH * NEW_CALL_ITEM_SIZE];
    static StaticSemaphore_t xMutexBuffer;
    /* Create a queue capable of containing 10 uint64_t values. */
    xWsApiCallNewQueue = xQueueCreateStatic(NEW_CALL_QUEUE_LENGTH,
                            NEW_CALL_ITEM_SIZE,
                            ucWsApiCallNewQueueStorageArea,
                            &xWsApiCallNewQueueStat);
    ws.user_ctx = context;

    xWsApiMutex = xSemaphoreCreateMutexStatic( &xMutexBuffer );

    xTaskCreate(vWsApiCallWorker, "ApiCallWork", 4096, NULL, uxTaskPriorityGet(NULL), NULL);
}
