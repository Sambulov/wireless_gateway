#include <esp_wifi.h>
#include <esp_log.h>
#include "cJSON.h"

#include "freertos/semphr.h"
#include "freertos/task.h"

#include <stdio.h>

#include "app.h"

#include "LinkedList.h"
#include "web_api.h"

static const char *TAG = "ws_server";

#define NEW_WS_QUEUE_LENGTH             20
#define NEW_CALL_ITEM_SIZE              sizeof(void *)
//#define NEW_CLOSE_ITEM_SIZE             sizeof(int)

#define WS_FRAME_LEN_MAX 2000

#define API_CALL_STATUS_NEW     0
#define API_CALL_STATUS_WRK     1
#define API_CALL_STATUS_BRK     2
#define API_CALL_STATUS_FIN     3

typedef struct {
    __LinkedListObject__
    ApiHandler_t fHandler;
    uint32_t ulFid;
} ApiHandlerItem_t;

typedef struct {
    __LinkedListObject__
    uint8_t *pucReqData;
    uint32_t ulReqDataLen;
    struct {
        httpd_handle_t hd;
        int fd;
    } pxWsc;
    uint32_t ulTimestamp;
    uint32_t ulId;
    void *pxHandlerContext;
    ApiHandler_t fHandler;
    uint32_t ulFid;
    uint8_t ucStatus;
} ApiCall_t;

typedef struct {
    uint32_t counter;
    httpd_ws_frame_t frame;
    uint8_t payload[4];
} ApiData_t;


static esp_err_t ws_handler(httpd_req_t *req);

httpd_uri_t ws = {
    .uri        = "/ws", // http://<ip>/ws -> ws://<ip>/ws
    .method     = HTTP_GET,
    .handler    = ws_handler,
    .user_ctx   = NULL,
    .is_websocket = true,
    .handle_ws_control_frames = true
};

static SemaphoreHandle_t xWsApiMutex = NULL;
static LinkedList_t pxWsApiHandlers = NULL;
static LinkedList_t pxWsApiCall = NULL;
static QueueHandle_t xWsApiCallNewQueue = NULL;
//static QueueHandle_t xWsCloseQueue = NULL;


static uint8_t bHandlerFidMatch(LinkedListItem_t *item, void *arg) {
    uint32_t fid = (uint32_t)arg;
    return LinkedListGetObject(ApiHandlerItem_t, item)->ulFid == fid;
}

static uint8_t bCallClientMatch(LinkedListItem_t *item, void *arg) {
    uint32_t fd = ((uint32_t *)arg)[0];
    uint32_t fid = ((uint32_t *)arg)[1];
    ApiCall_t *call = LinkedListGetObject(ApiCall_t, item);
    return (call->pxWsc.fd == fd) && (call->ulFid == fid);
}

static uint8_t bCallFidMatch(LinkedListItem_t *item, void *arg) {
    uint32_t fid = (uint32_t)arg;
    ApiCall_t *call = LinkedListGetObject(ApiCall_t, item);
    return ((call->ulFid == fid) && (call->ucStatus != API_CALL_STATUS_FIN));
}

static void vTerminateApiCallByFid(LinkedListItem_t *item, void *arg) {
    ApiCall_t *call = LinkedListGetObject(ApiCall_t, item);
    uint32_t fid = (uint32_t)arg;
    if((call->ucStatus != API_CALL_STATUS_FIN) && (call->ulFid == fid)) {
        ESP_LOGI(TAG, "Terminate call id:%lu", call->ulId);
        call->fHandler(call, &call->pxHandlerContext, API_CALL_BREAK, NULL, 0);
        call->ucStatus = API_CALL_STATUS_FIN;
        call->fHandler = NULL;
    }
}

static void vBreakApiCallByFd(LinkedListItem_t *item, void *arg) {
    ApiCall_t *call = LinkedListGetObject(ApiCall_t, item);
    uint32_t fd = (uint32_t)arg;
    if((call->ucStatus == API_CALL_STATUS_WRK) && (call->pxWsc.fd == fd)) {
        ESP_LOGI(TAG, "Break call id:%lu", call->ulId);
        call->ucStatus = API_CALL_STATUS_BRK;
    }
}


static void vFreeApiCall(LinkedListItem_t *item, void *arg) {
    ApiCall_t *call = LinkedListGetObject(ApiCall_t, item);
    if(call->ucStatus == API_CALL_STATUS_BRK) {
        ESP_LOGI(TAG, "Do call break: %lu", call->ulId);
        call->fHandler(call, &call->pxHandlerContext, API_CALL_BREAK, NULL, 0);
        call->ucStatus = API_CALL_STATUS_FIN;        
    }
    if(call->ucStatus == API_CALL_STATUS_FIN) {
        vTaskDelay(1);
        vLinkedListUnlink(item);
        ESP_LOGI(TAG, "Call freed id:%lu", call->ulId);
        free(call);
    }
}

static void vWsTransferComplete_cb(esp_err_t err, int socket, void *arg) {
    ApiData_t *apiData = (ApiData_t *)arg;
    if((!apiData->counter) || !(--apiData->counter)) {
        ESP_LOGI(TAG, "Transfer Complete");
        free(arg);
    }
}

uint8_t bApiCallRegister(ApiHandler_t fHandler, uint32_t ulFid) {
    if((fHandler == NULL) || 
       (ulFid == API_HANDLER_ID_GENEGAL) ||
       (xSemaphoreTakeRecursive(xWsApiMutex, pdMS_TO_TICKS(10)) != pdTRUE)) return 0;

    ApiHandlerItem_t *registered = LinkedListGetObject(ApiHandlerItem_t, pxLinkedListFindFirst(pxWsApiHandlers, bHandlerFidMatch, (void *)ulFid));
    if(registered != NULL) {
        xSemaphoreGiveRecursive(xWsApiMutex);
        return 0;
    }
    ApiHandlerItem_t *hd = malloc(sizeof(ApiHandlerItem_t));
    if(hd == NULL) {
        xSemaphoreGiveRecursive(xWsApiMutex);
        return 0;
    }
    hd->fHandler = fHandler;
    hd->ulFid = ulFid;
    vLinkedListInsertLast(&pxWsApiHandlers, LinkedListItem(hd));
    ESP_LOGI(TAG, "Api handler registered %lu", hd->ulFid);
    xSemaphoreGiveRecursive(xWsApiMutex);
    return 1;
}

uint8_t bApiCallUnregister(uint32_t ulFid) {
    if((ulFid == API_HANDLER_ID_GENEGAL) ||
       (xSemaphoreTakeRecursive(xWsApiMutex, pdMS_TO_TICKS(10)) != pdTRUE)) return 0;
    ApiHandlerItem_t *registered = LinkedListGetObject(ApiHandlerItem_t, pxLinkedListFindFirst(pxWsApiHandlers, bHandlerFidMatch, (void *)ulFid));
    if(registered != NULL) {
        ulLinkedListDoForeach(pxWsApiCall, vTerminateApiCallByFid, (void *)ulFid);
        vLinkedListUnlink(LinkedListItem(registered));
        ESP_LOGI(TAG, "Api handler unregistered %08lx", registered->ulFid);
        xSemaphoreGiveRecursive(xWsApiMutex);
        return 1;
    }
    xSemaphoreGiveRecursive(xWsApiMutex);
    return 0;
}

void vApiCallComplete(void *pxApiCall) {
    xSemaphoreTakeRecursive(xWsApiMutex, portMAX_DELAY);
    ApiCall_t *call = (ApiCall_t *)pxApiCall;
    if((bLinkedListContains(pxWsApiCall, LinkedListItem(call))) && 
       (call->ucStatus != API_CALL_STATUS_FIN)) {
        call->ucStatus = API_CALL_STATUS_BRK;
        ESP_LOGI(TAG, "Api call break %lu", call->ulId);
    }
    xSemaphoreGiveRecursive(xWsApiMutex);
}

uint8_t bApiCallGetId(void *pxApiCall, uint32_t *pulOutId) {
    if((pxApiCall == NULL) || (pulOutId == NULL)) return 0;
    xSemaphoreTakeRecursive(xWsApiMutex, portMAX_DELAY);
    ApiCall_t *call = (ApiCall_t *)pxApiCall;
    if(bLinkedListContains(pxWsApiCall, LinkedListItem(call)) &&
       (call->ucStatus != API_CALL_STATUS_FIN)) {
        *pulOutId = call->ulId;
        xSemaphoreGiveRecursive(xWsApiMutex);
        return 1;
    }
    xSemaphoreGiveRecursive(xWsApiMutex);
    return 0;
}

uint8_t bApiCallGetSockFd(void *pxApiCall, int *pxOutFd) {
    if((pxApiCall == NULL) || (pxOutFd == NULL)) return 0;
    xSemaphoreTakeRecursive(xWsApiMutex, portMAX_DELAY);
    ApiCall_t *call = (ApiCall_t *)pxApiCall;
    if(bLinkedListContains(pxWsApiCall, LinkedListItem(call)) &&
       (call->ucStatus != API_CALL_STATUS_FIN)) {
        *pxOutFd = call->pxWsc.fd;
        xSemaphoreGiveRecursive(xWsApiMutex);
        return 1;
    }
    xSemaphoreGiveRecursive(xWsApiMutex);
    return 0;
}

static uint8_t _bApiCallSendJson(void *pxApiCall, uint32_t ulFid, const uint8_t *ucJson, uint32_t ulLen, const char *pucTemplate) {
    if(((pxApiCall == NULL) && (ulFid == 0)) || ((pxApiCall != NULL) && (ulFid != 0))) return 0;
    esp_err_t res = ESP_OK;
    xSemaphoreTakeRecursive(xWsApiMutex, portMAX_DELAY);
    ApiCall_t *call = pxApiCall;
    if(bLinkedListContains(pxWsApiCall, LinkedListItem(call))) {
        if(call->ucStatus == API_CALL_STATUS_FIN) call = NULL;
    }
    uint32_t fid_group = 0;
    if(call == NULL) {
        fid_group = ulLinkedListCount(pxWsApiCall, bCallFidMatch, (void *)ulFid);
        if(fid_group)
            call = LinkedListGetObject(ApiCall_t, pxLinkedListFindFirst(pxWsApiCall, bCallFidMatch, (void *)ulFid));
    }
    if(call != NULL) {
        uint32_t len = 46 + ulLen;
        ApiData_t *resp = malloc(sizeof(ApiData_t) + len);
        if(resp != NULL) {
            resp->counter = fid_group;
            uint32_t second_arg = call->ulId;
            if(fid_group) second_arg = xTaskGetTickCount();
            sprintf((char *)resp->payload, pucTemplate, call->ulFid, second_arg, ucJson);
            resp->frame.fragmented = 0;
            resp->frame.type = HTTPD_WS_TYPE_TEXT;
            resp->frame.payload = resp->payload;
            resp->frame.len = len - 1;
            while (call != NULL) {
                res = httpd_ws_send_data_async(call->pxWsc.hd, call->pxWsc.fd, &resp->frame, vWsTransferComplete_cb, resp);
                ESP_LOGI(TAG, "Api call %lu json sent with %d", call->ulId, res);
                if(fid_group)
                    call = LinkedListGetObject(ApiCall_t, pxLinkedListFindNextNoOverlap(LinkedListItem(call), bCallFidMatch, (void *)ulFid));
                else break;
            }
        }
        else res = ESP_ERR_NO_MEM;
    }
    xSemaphoreGiveRecursive(xWsApiMutex);
    return (res == ESP_OK);
}

uint8_t bApiCallSendJson(void *pxApiCall, const uint8_t *ucJson, uint32_t ulLen) {
    return _bApiCallSendJson(pxApiCall, 0, ucJson, ulLen, "{\"FID\":\"0x%08lx\",\"CID\":\"0x%08lx\",\"ARG\":%s}");
}

uint8_t bApiCallSendStatus(void *pxApiCall, uint32_t ulSta) {
    uint8_t sta[13];
    sprintf((char *)sta, "\"0x%08lx\"", ulSta);
    return _bApiCallSendJson(pxApiCall, 0, sta, 13, "{\"FID\":\"0x%08lx\",\"CID\":\"0x%08lx\",\"STA\":%s}");
}

uint8_t bApiCallSendJsonFidGroup(uint32_t ulFid, const uint8_t *ucJson, uint32_t ulLen) {
    return _bApiCallSendJson(NULL, ulFid, ucJson, ulLen, "{\"FID\":\"0x%08lx\",\"RTS\":\"0x%08lx\",\"ARG\":%s}");
}

static esp_err_t ws_handler(httpd_req_t *req) {
    static uint32_t call_id = 0;

    esp_err_t ret = ESP_OK;
    ApiCall_t *wscd = NULL;
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
        goto err_exit;
    }

    if (ws_pkt.type == HTTPD_WS_TYPE_CLOSE) {
        ESP_LOGI(TAG, "Close frame");
        uint32_t fd = httpd_req_to_sockfd(req);
        httpd_ws_send_frame(req, &ws_pkt);
        httpd_sess_trigger_close(req->handle, fd);
        xSemaphoreTakeRecursive(xWsApiMutex, portMAX_DELAY);
        ulLinkedListDoForeach(pxWsApiCall, vBreakApiCallByFd, (void *)fd);
        xSemaphoreGiveRecursive(xWsApiMutex);
        xQueueSend(xWsApiCallNewQueue, (void *)&wscd, pdMS_TO_TICKS(10));
        return ESP_OK;
    }

    if (ws_pkt.type == HTTPD_WS_TYPE_PING) {
        ESP_LOGI(TAG, "Ping frame");
        httpd_ws_frame_t frame = {
            .type = HTTPD_WS_TYPE_PONG,
            .payload = NULL,
            .len = 0
        };
        httpd_ws_send_frame(req, &frame);
        return ESP_OK;
    }

    if (ws_pkt.type == HTTPD_WS_TYPE_PONG) {
        ESP_LOGI(TAG, "Pong frame");
        return ESP_OK;
    }
    
    ESP_LOGI(TAG, "Аrg len is %d", ws_pkt.len);
    if(!ws_pkt.len || (ws_pkt.len > WS_FRAME_LEN_MAX) || (ws_pkt.type != HTTPD_WS_TYPE_TEXT)) {
        ESP_LOGW(TAG, "No data or length too big or bad data type");
        ret = ESP_FAIL;
        goto err_exit;
    }

    buf = malloc(ws_pkt.len);
    if (buf == NULL) {
        ESP_LOGW(TAG, "Failed to calloc memory for buf");
        ret = ESP_ERR_NO_MEM;
        goto err_exit;
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
    uint32_t fid = 0;
    if (cJSON_IsNumber(fid_json)) {
        fid = fid_json->valueint;
    }
    else if (cJSON_IsString(fid_json)) {
        fid = (uint32_t)strtol(fid_json->valuestring, NULL, 16);
    }
    if(fid == API_HANDLER_ID_GENEGAL) {
        ESP_LOGW(TAG, "Bad API arg");
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

    wscd = malloc(sizeof(ApiCall_t));
    if(wscd == NULL){
        ret = ESP_ERR_NO_MEM;
        goto err_exit;
    }
    wscd->ulFid = fid;
    wscd->fHandler = NULL; //assigned in worker
    wscd->pxWsc.fd = httpd_req_to_sockfd(req);
    wscd->pxWsc.hd = req->handle;
    wscd->pxHandlerContext = NULL;
    wscd->ulTimestamp = 0;
    wscd->pucReqData = (uint8_t *)data;
    wscd->ulReqDataLen = dataLen;
    wscd->ulId = call_id++;
    wscd->ucStatus = API_CALL_STATUS_NEW;
    
    if(xQueueSend(xWsApiCallNewQueue, (void *)&wscd, pdMS_TO_TICKS(10)) != pdPASS ) {
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

static void vWsApiCallWorker( void * pvParameters ) {
    for( ;; ) {
        ApiCall_t *call = NULL;
        if(xQueueReceive(xWsApiCallNewQueue, &call, pdMS_TO_TICKS(1000)) == pdPASS ) {
            xSemaphoreTakeRecursive(xWsApiMutex, portMAX_DELAY);
            ESP_LOGI(TAG, "Call worker");
            ulLinkedListDoForeach(pxWsApiCall, vFreeApiCall, NULL);
            if(call != NULL) {
                call->ucStatus = API_CALL_STATUS_WRK;
                ApiCall_t *call_prev = LinkedListGetObject(ApiCall_t,
                    pxLinkedListFindFirst(pxWsApiCall, bCallClientMatch,
                        (void *)((void *[]){ (void *)call->pxWsc.fd, (void *)call->ulFid})));
                if(call_prev != NULL) {
                    call_prev->pucReqData = call->pucReqData;
                    call_prev->ulReqDataLen = call->ulReqDataLen;
                    free(call);
                    call = call_prev;
                }
                else {
                    ApiHandlerItem_t *hlr = LinkedListGetObject(ApiHandlerItem_t, pxLinkedListFindFirst(pxWsApiHandlers, bHandlerFidMatch, (void *)call->ulFid));
                    if(hlr != NULL) {
                        call->fHandler = hlr->fHandler;
                        vLinkedListInsertLast(&pxWsApiCall, LinkedListItem(call));
                    }
                }
                if(call->fHandler != NULL) {
                    uint8_t res = call->fHandler(call, &call->pxHandlerContext,
                        ((call_prev != NULL)? API_CALL_SECONDARY: API_CALL_PRIMARY),
                        call->pucReqData, call->ulReqDataLen);
                    if(res) call->ucStatus = API_CALL_STATUS_FIN;
                }
                else call->ucStatus = API_CALL_STATUS_FIN;
                if(call->pucReqData != NULL) free(call->pucReqData);
                call->ulReqDataLen = 0;
            }
            xSemaphoreGiveRecursive(xWsApiMutex);
        }
    }
    vTaskDelete(NULL);
}

void ws_init(app_context_t *context) {
    static StaticQueue_t xWsApiCallNewQueueStat;
    static uint8_t ucWsApiCallNewQueueStorageArea[NEW_WS_QUEUE_LENGTH * NEW_CALL_ITEM_SIZE];

    //static StaticQueue_t xWsCloseNewQueueStat;
    //static uint8_t ucWsCloseNewQueueStorageArea[NEW_WS_QUEUE_LENGTH * NEW_CALL_ITEM_SIZE];

    static StaticSemaphore_t xMutexBuffer;

    
    //xWsCloseQueue = xQueueCreateStatic(NEW_WS_QUEUE_LENGTH,
    //                        NEW_CLOSE_ITEM_SIZE,
    //                        ucWsCloseNewQueueStorageArea,
    //                        &xWsCloseNewQueueStat);

    xWsApiCallNewQueue = xQueueCreateStatic(NEW_WS_QUEUE_LENGTH,
                            NEW_CALL_ITEM_SIZE,
                            ucWsApiCallNewQueueStorageArea,
                            &xWsApiCallNewQueueStat);
    
    ws.user_ctx = context;
    xWsApiMutex = xSemaphoreCreateRecursiveMutexStatic( &xMutexBuffer );
    xTaskCreate(vWsApiCallWorker, "ApiCallWork", 4096, NULL, uxTaskPriorityGet(NULL), NULL);
}
