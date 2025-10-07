#include <esp_wifi.h>
#include <esp_log.h>
#include "cJSON.h"

#include "freertos/semphr.h"
#include "freertos/task.h"

#include <stdio.h>

#include "app.h"

#include "CodeLib.h"

#include "web_api.h"

static const char *TAG = "ws_server";

#define NEW_WS_QUEUE_LENGTH             10
#define NEW_CALL_ITEM_SIZE              sizeof(void *)

typedef struct {
    __LinkedListObject__
    ApiHandler_t fHandler;
    uint32_t ulFid;
    void *xHandlerContext;
} ApiHandlerItem_t;

typedef struct {
    __LinkedListObject__
    uint8_t *pucReqData;
    uint32_t ulReqDataLen;
    struct {
        httpd_handle_t hd;
        int fd;
    } pxWsc;
    uint32_t ulAliveTs;
    uint32_t ulPingTs;
    uint32_t ulId;
    void *pxHandlerContext;
    ApiHandler_t fHandler;
    uint32_t ulFid;
    uint32_t ulCallPending;
} ApiCall_t;

typedef struct {
    uint32_t counter;
    httpd_ws_frame_t frame;
    uint8_t payload[];
} ApiData_t;


static SemaphoreHandle_t xWsApiMutex = NULL;
static LinkedList_t pxWsApiHandlers = NULL;
static LinkedList_t pxWsApiCall = NULL;
static QueueHandle_t xWsApiNewCallQueue = NULL;

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
    return ((call->ulFid == fid) && call->ulCallPending);
}

static void vRefreshApiCallAliveTsByFd(LinkedListItem_t *item, void *arg) {
    ApiCall_t *call = LinkedListGetObject(ApiCall_t, item);
    uint32_t fd = (uint32_t)arg;
    if(call->pxWsc.fd == fd) {
        //ESP_LOGW(TAG, "Refresh call alive ts; socket:%d", call->pxWsc.fd);
        call->ulAliveTs = xTaskGetTickCount();
    }
}

static void vRefreshApiCallPingTsByFd(LinkedListItem_t *item, void *arg) {
    ApiCall_t *call = LinkedListGetObject(ApiCall_t, item);
    uint32_t fd = (uint32_t)arg;
    if(call->pxWsc.fd == fd) {
        //ESP_LOGW(TAG, "Refresh call ping ts; socket:%d", call->pxWsc.fd);
        call->ulPingTs = xTaskGetTickCount();
    }
}

static void vBreakApiCallByFd(LinkedListItem_t *item, void *arg) {
    ApiCall_t *call = LinkedListGetObject(ApiCall_t, item);
    uint32_t fd = (uint32_t)arg;
    if(call->pxWsc.fd == fd) {
        ESP_LOGI(TAG, "Break call id:%lu", call->ulId);
        call->ulCallPending = 0;
    }
}

static void vBrakeApiCallsByFd(uint32_t LockWait, int fd) {
    if(xSemaphoreTakeRecursive(xWsApiMutex, pdMS_TO_TICKS(LockWait)) == pdTRUE) {
        ulLinkedListDoForeach(pxWsApiCall, vBreakApiCallByFd, (void *)fd);
        xSemaphoreGiveRecursive(xWsApiMutex);
    }
}

static void vRefreshApiCallsAliveTsByFd(uint32_t LockWait, int fd) {
    if(xSemaphoreTakeRecursive(xWsApiMutex, pdMS_TO_TICKS(LockWait)) == pdTRUE) {
        ulLinkedListDoForeach(pxWsApiCall, vRefreshApiCallAliveTsByFd, (void *)fd);
        xSemaphoreGiveRecursive(xWsApiMutex);
    }
}

static void vRefreshApiCallsPingTsByFd(uint32_t LockWait, int fd) {
    if(xSemaphoreTakeRecursive(xWsApiMutex, pdMS_TO_TICKS(LockWait)) == pdTRUE) {
        ulLinkedListDoForeach(pxWsApiCall, vRefreshApiCallPingTsByFd, (void *)fd);
        xSemaphoreGiveRecursive(xWsApiMutex);
    }
}

static void vServeApiCall(LinkedListItem_t *item, void *arg) {
    ApiCall_t *call = LinkedListGetObject(ApiCall_t, item);
    if(call->ulCallPending > 0) {
        static httpd_ws_frame_t frame = { 
            .final = 1, 
            .fragmented = 0, 
            .payload = NULL,
            .len = 0,
            .type = HTTPD_WS_TYPE_PING
        };
        uint32_t now = xTaskGetTickCount();
        if(((now - call->ulAliveTs) >= CONFIG_WEB_SOCKET_PING_DELAY) && 
           ((now - call->ulPingTs) >= CONFIG_WEB_SOCKET_PING_DELAY)) {
            //ESP_LOGI(TAG, "Send ping with socket: %d", call->pxWsc.fd);
            if(httpd_ws_send_frame_async(call->pxWsc.hd, call->pxWsc.fd, &frame) != ESP_OK) {
                ESP_LOGW(TAG, "Send ping error");
                vApiCallComplete(call);
            }
            vRefreshApiCallsPingTsByFd(0, call->pxWsc.fd);
        }
    }
    if(call->ulCallPending == 0) {
        ESP_LOGI(TAG, "Api call complete, id: %lu", call->ulId);
        if(call->fHandler != NULL) call->fHandler(call, &call->pxHandlerContext, 0, NULL, 0);
        vLinkedListUnlink(item);
        free(call);
    }
}

static void vWsTransferComplete_cb(esp_err_t err, int socket, void *arg) {
    ApiData_t *apiData = (ApiData_t *)arg;
    ESP_LOGI(TAG, "Transfer Complete, err: %d, fd: %d", err, socket);
    if(err) vBrakeApiCallsByFd(0, socket);
    if((!apiData->counter) || !(--apiData->counter)) {
        free(arg);
    }
}

uint8_t bApiCallRegister(ApiHandler_t fHandler, uint32_t ulFid, void *pxContext) {
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
    hd->xHandlerContext = pxContext;
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
        ulLinkedListDoForeach(pxWsApiCall, vBreakApiCallByFd, (void *)ulFid);
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
    if((bLinkedListContains(pxWsApiCall, LinkedListItem(call))) && call->ulCallPending)
        call->ulCallPending--;
    xSemaphoreGiveRecursive(xWsApiMutex);
}

uint8_t bApiCallGetId(void *pxApiCall, uint32_t *pulOutId) {
    if((pxApiCall == NULL) || (pulOutId == NULL)) return 0;
    xSemaphoreTakeRecursive(xWsApiMutex, portMAX_DELAY);
    ApiCall_t *call = (ApiCall_t *)pxApiCall;
    if(bLinkedListContains(pxWsApiCall, LinkedListItem(call)) && call->ulCallPending) {
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
    if(bLinkedListContains(pxWsApiCall, LinkedListItem(call)) && call->ulCallPending) {
        *pxOutFd = call->pxWsc.fd;
        xSemaphoreGiveRecursive(xWsApiMutex);
        return 1;
    }
    xSemaphoreGiveRecursive(xWsApiMutex);
    return 0;
}

static uint8_t _bApiCallSendJson(void *pxApiCall, uint32_t ulFid, const uint8_t *ucJson, uint32_t ulLen) {
    if(((pxApiCall == NULL) && (ulFid == 0)) || ((pxApiCall != NULL) && (ulFid != 0))) return 0;
    esp_err_t res = ESP_OK;
    xSemaphoreTakeRecursive(xWsApiMutex, portMAX_DELAY);
    ApiCall_t *call = pxApiCall;
    uint32_t fid_group = 1;
    uint32_t sid = xTaskGetTickCount();
    if(call != NULL) {
        if(!bLinkedListContains(pxWsApiCall, LinkedListItem(call)) || !call->ulCallPending) {
            res = ESP_ERR_INVALID_STATE;
            call = NULL;
        }
        else sid = call->ulId;
    }
    else {
        fid_group = ulLinkedListCount(pxWsApiCall, bCallFidMatch, (void *)ulFid);
        if(fid_group) call = LinkedListGetObject(ApiCall_t, pxLinkedListFindFirst(pxWsApiCall, bCallFidMatch, (void *)ulFid));
    }
    if(call != NULL) {
        char pucTemplate[] = "{\"FID\":\"0x%08lx\",\"SID\":\"0x%08lx\",\"ARG\":";
        uint32_t len = ulLen + sizeof(pucTemplate) + /* length_dif("%08lx", "00000000") X 2 = 6 symb, and ("/0" -> "}" */ 6;
        ApiData_t *resp = malloc(sizeof(ApiData_t) + len);
        if(resp != NULL) {
            int offset = sprintf((char *)resp->payload, pucTemplate, call->ulFid, sid);
            mem_cpy(&resp->payload[offset], ucJson, ulLen);
            offset += ulLen;
            resp->payload[offset] = '}';
            resp->counter = fid_group;
            resp->frame.fragmented = 0;
            resp->frame.type = HTTPD_WS_TYPE_TEXT;
            resp->frame.payload = resp->payload;
            resp->frame.len = len;
            while (call != NULL) {
                res = httpd_ws_send_data_async(call->pxWsc.hd, call->pxWsc.fd, &resp->frame, vWsTransferComplete_cb, resp);
                ESP_LOGI(TAG, "Api call %lu json sending with result: %d", call->ulId, res);
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
    return _bApiCallSendJson(pxApiCall, 0, ucJson, ulLen);
}

uint8_t bApiCallSendStatus(void *pxApiCall, uint32_t ulSta) {
    uint8_t sta[21];
    uint32_t len = sprintf((char *)sta, "{\"STA\":\"0x%08lx\"}", ulSta);
    return _bApiCallSendJson(pxApiCall, 0, sta, len);
}

uint8_t bApiCallSendJsonFidGroup(uint32_t ulFid, const uint8_t *ucJson, uint32_t ulLen) {
    return _bApiCallSendJson(NULL, ulFid, ucJson, ulLen);
}

static void vWsParseApiRequest(uint8_t *payload, int32_t pl_len, httpd_handle_t hd, int fd) {
    uint8_t err = 1;
    ApiCall_t *wscd = malloc(sizeof(ApiCall_t));
    if(wscd != NULL) {
        wscd->ulFid = 0;
        wscd->pucReqData = NULL;
        wscd->ulReqDataLen = 0;
        wscd->pxWsc.fd = fd;
        wscd->pxWsc.hd = hd;
        cJSON *json = cJSON_ParseWithLengthOpts((char *)payload, pl_len, 0, 0);
        if (json != NULL) {
            cJSON *fid_json = cJSON_GetObjectItem(json, "FID");
            if (cJSON_IsNumber(fid_json)) wscd->ulFid = fid_json->valueint;
            else if (cJSON_IsString(fid_json)) wscd->ulFid = (uint32_t)strtol(fid_json->valuestring, NULL, 16);
            if(wscd->ulFid != API_HANDLER_ID_GENEGAL) {
                cJSON *arg_json = cJSON_GetObjectItem(json, "ARG");
                if(arg_json != NULL) {
                    //if(arg_json->type == cJSON_Object) {
                        wscd->pucReqData = (uint8_t *)cJSON_PrintUnformatted(arg_json);
                        wscd->ulReqDataLen = lStrLen(wscd->pucReqData);
                        ESP_LOGI(TAG, "Api arg %s", wscd->pucReqData);
                    //}
                    //else ESP_LOGI(TAG, "Api call ARG property type is not an object");
                }
                if(xQueueSend(xWsApiNewCallQueue, (void *)&wscd, pdMS_TO_TICKS(10)) == pdPASS) {
                    err = 0;
                    ESP_LOGI(TAG, "New api call %lu enqueued with id:%lu", wscd->ulFid, wscd->ulId);
                }
                else ESP_LOGW(TAG, "Queue full");
            }
            else ESP_LOGW(TAG, "Api call bad FID property");
            cJSON_Delete(json);
        }
        else ESP_LOGW(TAG, "Invalid JSON");
        if(err) {
            if(wscd->pucReqData != NULL) free(wscd->pucReqData);
            free(wscd);
        }
    }
    else ESP_LOGW(TAG, "Failed to malloc memory for ws api call");
}

static esp_err_t eWsHandler(httpd_req_t *req) {
    httpd_ws_frame_t resp = {0};
    httpd_ws_frame_t ws_pkt = {0};

    ESP_LOGI(TAG, "%s: --> req %p", __func__, req);
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "%s: Handshake done, the new connection was opened", __func__);
        return ESP_OK;
    }

    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "%s: Failed to read web socket header (err %d)", __func__, ret);
        return ESP_FAIL;
    }

    uint32_t fd = httpd_req_to_sockfd(req);
    vRefreshApiCallsAliveTsByFd(0, fd);

    ws_pkt.payload = NULL;
    if(ws_pkt.len) {
        uint32_t size = ws_pkt.len * sizeof(uint8_t) + 1; /* packet data +1 '\0' */
        ws_pkt.payload = malloc(size); 
        if (!ws_pkt.payload) {
            ESP_LOGI(TAG, "%s: Can't allocate %u bytes", __func__, ws_pkt.len);
            return ESP_FAIL;
        }
        ((uint8_t *)ws_pkt.payload)[size - 1] = '\0';
        ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to read web socket data (err %d)", ret);
            free(ws_pkt.payload);
            return ESP_FAIL;
        }
    }

    switch (ws_pkt.type) {
        case HTTPD_WS_TYPE_TEXT:
            if(ws_pkt.payload)
                ESP_LOGI(TAG, "%s: Got packet with message: %s", __func__, ws_pkt.payload);
            else 
                ESP_LOGI(TAG, "%s: Got packet with empty message", __func__);
            vWsParseApiRequest((uint8_t *)ws_pkt.payload, ws_pkt.len, req->handle, fd);
            break;
        case HTTPD_WS_TYPE_PING:
            ESP_LOGI(TAG, "%s: PING frame received, len = %d", __func__, ws_pkt.len);
            memset(&resp, 0, sizeof(httpd_ws_frame_t));
            resp.type = HTTPD_WS_TYPE_PONG;
            resp.final = true;
            resp.fragmented = false;
            resp.payload = ws_pkt.payload;
            resp.len = ws_pkt.len;
            if (httpd_ws_send_frame(req, &resp) != ESP_OK) {
                ESP_LOGI(TAG, "%s: Cannot send PONG frame", __func__);
                free(ws_pkt.payload);
                return ESP_ERR_INVALID_STATE;
            }
            break;
        case HTTPD_WS_TYPE_PONG:
            ESP_LOGI(TAG, "%s: PONG frame received, len = %d", __func__, ws_pkt.len);
            //TODO: update keep alive status
            break;
        case HTTPD_WS_TYPE_CLOSE:
            ESP_LOGI(TAG, "%s: CLOSE frame received, len = %d", __func__, ws_pkt.len);
            //TODO: works not good when close frame received. Check protocol implementation
            resp.type = HTTPD_WS_TYPE_CLOSE;
            if (httpd_ws_send_frame(req, &resp) != ESP_OK) {
                ESP_LOGI(TAG, "%s: Cannot send CLOSE frame", __func__);
                free(ws_pkt.payload);
                return ESP_ERR_INVALID_STATE;
            }
            break;
        case HTTPD_WS_TYPE_CONTINUE:
            ESP_LOGI(TAG, "%s: CONTINUE frame received, len = %d", __func__, ws_pkt.len);
            break;
        case HTTPD_WS_TYPE_BINARY:
            ESP_LOGI(TAG, "%s: BINARY frame received, len = %d", __func__, ws_pkt.len);
                break;
        default:
            ESP_LOGI(TAG, "%s: UNSUPPORTED frame received, type = %d", __func__, ws_pkt.type);
            break;
    }

    free(ws_pkt.payload);

#if 1
    //TODO: add to debug build, remove from release
    uint32_t heap = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    if(heap < 30000)
        ESP_LOGI(TAG, "Heap left:%lu", heap);
#endif

    return ESP_OK; //ignore errors, drop packet;
}

static void vWsApiCallWorker(void *pvParameters) {
    ApiCall_t *call = NULL;
    for(;;) {
        xSemaphoreTakeRecursive(xWsApiMutex, portMAX_DELAY);
        ulLinkedListDoForeach(pxWsApiCall, vServeApiCall, NULL);
        xSemaphoreGiveRecursive(xWsApiMutex);
        if(xQueueReceive(xWsApiNewCallQueue, &call, pdMS_TO_TICKS(10)) == pdPASS ) {
            static uint32_t call_id = 0;
            call->fHandler = NULL;
            call->pxHandlerContext = NULL;
            call->ulAliveTs = xTaskGetTickCount();
            call->ulId = call_id++;
            call->ulCallPending = 1;
            xSemaphoreTakeRecursive(xWsApiMutex, portMAX_DELAY);
            ApiCall_t *call_prev = LinkedListGetObject(ApiCall_t,
                pxLinkedListFindFirst(pxWsApiCall, bCallClientMatch,
                    (void *)((void *[]){ (void *)call->pxWsc.fd, (void *)call->ulFid})));
            if(call_prev != NULL) {
                call_prev->pucReqData = call->pucReqData;
                call_prev->ulReqDataLen = call->ulReqDataLen;
                call_prev->ulAliveTs = call->ulAliveTs;
                free(call);
                call = call_prev;
                call->ulCallPending++;
            }
            else {
                vLinkedListInsertLast(&pxWsApiCall, LinkedListItem(call));
                ApiHandlerItem_t *hlr = LinkedListGetObject(ApiHandlerItem_t, pxLinkedListFindFirst(pxWsApiHandlers, bHandlerFidMatch, (void *)call->ulFid));
                if(hlr != NULL) {
                    call->fHandler = hlr->fHandler;
                    call->pxHandlerContext = hlr->xHandlerContext;
                }
                else {
                    bApiCallSendStatus(pxWsApiCall, API_CALL_ERROR_STATUS_NO_HANDLER);
                    call->ulCallPending = 0;
                }
            }
            if(call->fHandler != NULL) {
                uint8_t res = call->fHandler(call, &call->pxHandlerContext, call->ulCallPending, call->pucReqData, call->ulReqDataLen);
                if(res && (call->ulCallPending > 0)) 
                    call->ulCallPending--;
            }
            if(call->pucReqData != NULL) free(call->pucReqData);
            call->ulReqDataLen = 0;
            xSemaphoreGiveRecursive(xWsApiMutex);
        }
    }
    vTaskDelete(NULL);
}

httpd_uri_t *pxWsServerInit(char *uri) {
    //static esp_err_t eWsHandler(httpd_req_t *req);
    httpd_uri_t *ws_h = malloc(sizeof(httpd_uri_t));
    if(ws_h != NULL) {
        memset(ws_h, 0, sizeof(httpd_uri_t));
        ws_h->uri                      = uri;
        ws_h->method                   = HTTP_GET;
        ws_h->handler                  = eWsHandler;
        ws_h->user_ctx                 = NULL;
        ws_h->is_websocket             = true;
        ws_h->handle_ws_control_frames = true;
        static StaticQueue_t xWsApiNewCallQueueStat;
        static uint8_t ucWsApiCallNewQueueStorageArea[NEW_WS_QUEUE_LENGTH * NEW_CALL_ITEM_SIZE];
        static StaticSemaphore_t xMutexBuffer;
        xWsApiNewCallQueue = xQueueCreateStatic(NEW_WS_QUEUE_LENGTH,
                                NEW_CALL_ITEM_SIZE,
                                ucWsApiCallNewQueueStorageArea,
                                &xWsApiNewCallQueueStat);
        xWsApiMutex = xSemaphoreCreateRecursiveMutexStatic( &xMutexBuffer );
        xTaskCreate(vWsApiCallWorker, "ApiCallWork", 4096, NULL, uxTaskPriorityGet(NULL), NULL);
    }
    return ws_h;
}
