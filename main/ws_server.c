#include <esp_wifi.h>
#include <esp_log.h>
#include "cJSON.h"

#include "freertos/semphr.h"
#include "freertos/task.h"

#include <stdio.h>

#include "app.h"

#include "CodeLib.h"

#include "web_api.h"

typedef struct {
    __LinkedListObject__
    ApiHandler_t fHandler;
    uint32_t ulFid;
    void *xHandlerContext;
} ApiHandlerItem_t;

typedef struct {
    httpd_handle_t hd;
    int fd;
    int32_t pending_api;
    uint32_t ulAliveTs;
    uint32_t ulPingTs;
    delegate_t delegate;
} ApiSession_t;

typedef struct {
    __LinkedListObject__
    ApiSession_t *session;
    uint8_t *pucReqData;
    uint32_t ulReqDataLen;
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
static SemaphoreHandle_t xWsNewApiReqSem = NULL;
static LinkedList_t pxWsApiHandlers = NULL;
static LinkedList_t pxWsApiCall = NULL;
static LinkedList_t pxWsApiNewCall = NULL;

extern httpd_handle_t server;

static uint8_t bHandlerFidMatch(LinkedListItem_t *item, void *arg) {
    uint32_t fid = (uint32_t)arg;
    return LinkedListGetObject(ApiHandlerItem_t, item)->ulFid == fid;
}

static uint8_t bCallClientMatch(LinkedListItem_t *item, void *arg) {
    uint32_t fd = cl_tuple_get(arg, 0, uint32_t);
    uint32_t fid = cl_tuple_get(arg, 1, uint32_t);
    ApiCall_t *call = LinkedListGetObject(ApiCall_t, item);
    return call->session && (call->session->fd == fd) && (call->ulFid == fid);
}

static uint8_t bCallFidMatch(LinkedListItem_t *item, void *arg) {
    uint32_t fid = (uint32_t)arg;
    ApiCall_t *call = LinkedListGetObject(ApiCall_t, item);
    return ((call->ulFid == fid) && call->session);
}

static void vBreakApiCallByFd(LinkedListItem_t *item, void *arg) {
    ApiCall_t *call = LinkedListGetObject(ApiCall_t, item);
    if(call->session && (call->session->fd == (uint32_t)arg)) {
        ESP_LOGI(TAG, "Break call id:%lu", call->ulId);
        call->session = NULL;
    }
}

static void vBreakApiCallByFid(LinkedListItem_t *item, void *arg) {
    ApiCall_t *call = LinkedListGetObject(ApiCall_t, item);
    if(call->ulFid == (uint32_t)arg) {
        ESP_LOGI(TAG, "Break call id:%lu", call->ulId);
        call->session = NULL;
    }
}

static void vBreakApiCallsByFd(uint32_t LockWait, uint32_t fd) {
    if(xSemaphoreTakeRecursive(xWsApiMutex, pdMS_TO_TICKS(LockWait)) == pdTRUE) {
        ulLinkedListDoForeach(pxWsApiCall, vBreakApiCallByFd, (void *)fd);
        ulLinkedListDoForeach(pxWsApiNewCall, vBreakApiCallByFd, (void *)fd);
        xSemaphoreGiveRecursive(xWsApiMutex);
    }
}

static void vBreakApiCallsByFid(uint32_t LockWait, uint32_t fid) {
    if(xSemaphoreTakeRecursive(xWsApiMutex, pdMS_TO_TICKS(LockWait)) == pdTRUE) {
        ulLinkedListDoForeach(pxWsApiCall, vBreakApiCallByFid, (void *)fid);
        ulLinkedListDoForeach(pxWsApiNewCall, vBreakApiCallByFid, (void *)fid);
        xSemaphoreGiveRecursive(xWsApiMutex);
    }
}

static void vServeApiCall(LinkedListItem_t *item, void *arg) {
    ApiCall_t *call = LinkedListGetObject(ApiCall_t, item);
    if(call->session) {
        static httpd_ws_frame_t frame = { 
            .final = 1, 
            .fragmented = 0, 
            .payload = NULL,
            .len = 0,
            .type = HTTPD_WS_TYPE_PING
        };
        uint32_t now = xTaskGetTickCount();
        if(((now - call->session->ulAliveTs) >= CONFIG_WEB_SOCKET_PING_DELAY) && 
           ((now - call->session->ulPingTs) >= CONFIG_WEB_SOCKET_PING_DELAY)) {
            //ESP_LOGI(TAG, "Send ping with socket: %d", call->pxWsc.fd);
            if(httpd_ws_send_frame_async(call->session->hd, call->session->fd, &frame) != ESP_OK) {
                ESP_LOGW(TAG, "Send ping error");
                vApiCallComplete(call);
            }
            call->session->ulPingTs = xTaskGetTickCount();
        }
    }
    else {
        ESP_LOGI(TAG, "Api call complete, id: %lu", call->ulId);
        if(call->fHandler != NULL) call->fHandler(call, &call->pxHandlerContext, 0, NULL, 0);
        vLinkedListUnlink(item);
        free(call);
    }
}

static void vWsTransferComplete_cb(esp_err_t err, int socket, void *arg) {
    ApiData_t *apiData = (ApiData_t *)arg;
    ESP_LOGI(TAG, "Transfer Complete, err: %d, fd: %x", err, socket);
    if(err) 
        vBreakApiCallsByFd(100, socket);
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
        vBreakApiCallsByFid(0, ulFid);
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
    if(bLinkedListContains(pxWsApiCall, LinkedListItem(call))) {
        if(call->ulCallPending)
            call->ulCallPending--;
        if(!call->ulCallPending) 
            call->session = NULL;
    }
    xSemaphoreGiveRecursive(xWsApiMutex);
}

uint8_t bApiCallGetId(void *pxApiCall, uint32_t *pulOutId) {
    if((pxApiCall == NULL) || (pulOutId == NULL)) return 0;
    xSemaphoreTakeRecursive(xWsApiMutex, portMAX_DELAY);
    ApiCall_t *call = (ApiCall_t *)pxApiCall;
    if(bLinkedListContains(pxWsApiCall, LinkedListItem(call)) && call->session) {
        *pulOutId = call->ulId;
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
        if(!bLinkedListContains(pxWsApiCall, LinkedListItem(call)) || !call->session) {
            res = ESP_ERR_INVALID_STATE;
            call = NULL;
        }
        else 
            sid = call->ulId;
    }
    else {
        fid_group = ulLinkedListCount(pxWsApiCall, bCallFidMatch, (void *)ulFid);
        if(fid_group) 
            call = LinkedListGetObject(ApiCall_t, pxLinkedListFindFirst(pxWsApiCall, bCallFidMatch, (void *)ulFid));
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
                res = httpd_ws_send_data_async(call->session->hd, call->session->fd, &resp->frame, vWsTransferComplete_cb, resp);
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

void free_ctx_func(void *ctx) {
    ESP_LOGI(TAG, "Free session: %p", ctx);
    event_unsubscribe(&((ApiSession_t *)ctx)->delegate);
    vBreakApiCallsByFd(10, ((ApiSession_t *)ctx)->fd);
    free(ctx);
}

static esp_err_t frame_handle_text(httpd_req_t *req, httpd_ws_frame_t *ws_pkt, uint32_t fd) {
    if(ws_pkt->payload) {
        ESP_LOGI(TAG, "Got packet with message: %s", ws_pkt->payload);
        ApiCall_t *wscd = malloc(sizeof(ApiCall_t));
        if(wscd != NULL) {
            memset(wscd, 0, sizeof(ApiCall_t));
            cJSON *json = cJSON_ParseWithLengthOpts((char *)ws_pkt->payload, ws_pkt->len, 0, 0);
            if (json != NULL) {
                cJSON *fid_json = cJSON_GetObjectItem(json, "FID");
                if (cJSON_IsNumber(fid_json)) 
                    wscd->ulFid = fid_json->valueint;
                else if (cJSON_IsString(fid_json)) 
                    wscd->ulFid = (uint32_t)strtol(fid_json->valuestring, NULL, 16);
                if(wscd->ulFid != API_HANDLER_ID_GENEGAL) {
                    cJSON *arg_json = cJSON_GetObjectItem(json, "ARG");
                    if(arg_json != NULL) {
                        wscd->pucReqData = (uint8_t *)cJSON_PrintUnformatted(arg_json);
                        wscd->ulReqDataLen = lStrLen(wscd->pucReqData);
                        ESP_LOGI(TAG, "Api arg %s", wscd->pucReqData);
                    }
                    wscd->ulCallPending = 1;
                    wscd->session = req->sess_ctx;
                    xSemaphoreTakeRecursive(xWsApiMutex, portMAX_DELAY);
                    wscd->session->pending_api++;
                    vLinkedListInsertLast(&pxWsApiNewCall, LinkedListItem(wscd));
                    xSemaphoreGive(xWsNewApiReqSem);
                    xSemaphoreGiveRecursive(xWsApiMutex);
                    ESP_LOGI(TAG, "New api call %lu enqueued with id:%lu", wscd->ulFid, wscd->ulId);
                }
                else 
                    ESP_LOGW(TAG, "Api call bad FID property");
                cJSON_Delete(json);
            }
            else 
                ESP_LOGW(TAG, "Invalid JSON");
        }
        else 
            ESP_LOGW(TAG, "Failed to malloc memory for ws api call");
    }
    else 
        ESP_LOGI(TAG, "Got packet with empty message");
    return ESP_OK;
}

static esp_err_t frame_handle_ping(httpd_req_t *req, httpd_ws_frame_t *ws_pkt) {
    httpd_ws_frame_t resp = {0};
    ESP_LOGI(TAG, "PING frame received, len = %d", ws_pkt->len);
    resp.type = HTTPD_WS_TYPE_PONG;
    resp.final = true;
    resp.fragmented = false;
    resp.payload = ws_pkt->payload;
    resp.len = ws_pkt->len;
    if (httpd_ws_send_frame(req, &resp) != ESP_OK) {
        ESP_LOGI(TAG, "Cannot send PONG frame");
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

static esp_err_t frame_handle_pong(httpd_req_t *req, httpd_ws_frame_t *ws_pkt) {
    (void)req;
    ESP_LOGI(TAG, "PONG frame received, len = %d", ws_pkt->len);
    return ESP_OK;
}

static esp_err_t frame_handle_close(httpd_req_t *req, httpd_ws_frame_t *ws_pkt) {
    httpd_ws_frame_t resp = {0};
    //memset(&resp, 0, sizeof(httpd_ws_frame_t));
    ESP_LOGI(TAG, "CLOSE frame received, len = %d", ws_pkt->len);
    //TODO: works not good when close frame received. Check protocol implementation
    resp.type = HTTPD_WS_TYPE_CLOSE;
    if (httpd_ws_send_frame(req, &resp) != ESP_OK) {
        ESP_LOGI(TAG, "Cannot send CLOSE frame");
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

static esp_err_t frame_handle_dummy(httpd_req_t *req, httpd_ws_frame_t *ws_pkt) {
    (void)req;
    ESP_LOGI(TAG, "UNSUPPORTED frame received, type = %d", ws_pkt->type);
    return ESP_OK; /* just drop the frame */
}

static esp_err_t frame_obtain_payload(httpd_req_t *req, httpd_ws_frame_t *ws_pkt) {
    esp_err_t ret = ESP_OK;
    if(ws_pkt->len) {
        uint32_t size = ws_pkt->len * sizeof(uint8_t) + 1; /* packet data +1 '\0' */
        ws_pkt->payload = malloc(size); 
        if (ws_pkt->payload) {
            ((uint8_t *)ws_pkt->payload)[size - 1] = '\0';
            ret = httpd_ws_recv_frame(req, ws_pkt, ws_pkt->len);
            if (ret != ESP_OK)
                ESP_LOGW(TAG, "Failed to read web socket data (err %d)", ret);
        }
        else {
            ESP_LOGI(TAG, "Can't allocate %u bytes for payload buffer", ws_pkt->len);
            ret = ESP_ERR_NO_MEM;
        }
    }
    return ret;
}

static inline void frame_cleanup(httpd_ws_frame_t *ws_pkt) {
    free(ws_pkt->payload);
}

void link_event_handler(void *event_trigger, void *sender, void *context) {
    ESP_LOGI(TAG, "Trigger FD close %p", context);
    httpd_sess_trigger_close(server, (int)context);
}

static esp_err_t frame_receive(httpd_req_t *req, httpd_ws_frame_t *ws_pkt, uint32_t fd) {
    ESP_LOGI(TAG, "--> req %p, meth: %x", req->handle, req->method);
    ApiSession_t *sess;
    if (req->method == HTTP_GET) {
        sess = malloc(sizeof(ApiSession_t));
        if(sess) {
            sess->pending_api = 0;
            sess->fd = fd;
            sess->hd = req->handle;
            sess->ulPingTs = sess->ulAliveTs = xTaskGetTickCount();
            sess->delegate.context = (void *)fd;
            sess->delegate.handler = &link_event_handler;
            socket_link_subscribe(fd, &sess->delegate);
            req->sess_ctx = sess;
            req->free_ctx = free_ctx_func;

            ESP_LOGI(TAG, "Handshake done, the new connection was opened, session %p", sess);
            return ESP_OK;
        }
        return ESP_ERR_NO_MEM;
    }
    sess = (ApiSession_t *)req->sess_ctx;
    if(sess->pending_api >= CONFIG_WEB_SOCKET_MAX_PENDING_API) {
        ESP_LOGW(TAG, "Too much pending requests for the session %p", sess);
        return ESP_FAIL; /* drop request */
    }
    esp_err_t result = httpd_ws_recv_frame(req, ws_pkt, 0);
    if (result != ESP_OK)
        ESP_LOGW(TAG, "Failed to read web socket header (err %d)", result);
    return result;
}

static esp_err_t eWsHandler(httpd_req_t *req) {
    //req->user_ctx
    httpd_ws_frame_t ws_pkt = {0};
    uint32_t fd = httpd_req_to_sockfd(req);
    esp_err_t result = frame_receive(req, &ws_pkt, fd);
    if((result == ESP_OK) && (req->method == 0)) {
        ((ApiSession_t *)req->sess_ctx)->ulAliveTs = xTaskGetTickCount();
        //todo max payload size
        result = frame_obtain_payload(req, &ws_pkt);
        if(result == ESP_OK)
            switch (ws_pkt.type) {
                case HTTPD_WS_TYPE_TEXT:
                    result = frame_handle_text(req, &ws_pkt, fd);
                    break;
                case HTTPD_WS_TYPE_PING:
                    result = frame_handle_ping(req, &ws_pkt);
                    break;
                case HTTPD_WS_TYPE_PONG:
                    result = frame_handle_pong(req, &ws_pkt);
                    break;
                case HTTPD_WS_TYPE_CLOSE:
                    result = frame_handle_close(req, &ws_pkt);
                    break;
                case HTTPD_WS_TYPE_CONTINUE:
                case HTTPD_WS_TYPE_BINARY:
                default:
                    result = frame_handle_dummy(req, &ws_pkt);
                    break;
            }
        frame_cleanup(&ws_pkt);
    }
    return result;
}

static void vWsApiCallWorker(void *pvParameters) {
    static uint32_t call_id = 0;    
    for(;;) {
        xSemaphoreTakeRecursive(xWsApiMutex, portMAX_DELAY);
        ulLinkedListDoForeach(pxWsApiCall, vServeApiCall, NULL);
        xSemaphoreGiveRecursive(xWsApiMutex);
        if(xSemaphoreTake(xWsNewApiReqSem, pdMS_TO_TICKS(10)) == pdPASS) {
            xSemaphoreTakeRecursive(xWsApiMutex, portMAX_DELAY);
            ApiCall_t *call = LinkedListGetObject(ApiCall_t, pxWsApiNewCall);
            call->fHandler = NULL;
            call->pxHandlerContext = NULL;
            call->ulId = call_id++;
            call->session->pending_api--;
            if(call->session) {
                ApiCall_t *call_prev = LinkedListGetObject(ApiCall_t,
                    pxLinkedListFindFirst(pxWsApiCall, bCallClientMatch,
                        cl_tuple_make((void *)call->session->fd, (void *)call->ulFid)));
                if(call_prev != NULL) {
                    call_prev->pucReqData = call->pucReqData;
                    call_prev->ulReqDataLen = call->ulReqDataLen;
                    vLinkedListUnlink(pxWsApiNewCall);
                    free(call);
                    call = call_prev;
                    call->ulCallPending++;
                }
                else {
                    vLinkedListInsertLast(&pxWsApiCall, pxWsApiNewCall);
                    ApiHandlerItem_t *hlr = LinkedListGetObject(ApiHandlerItem_t, pxLinkedListFindFirst(pxWsApiHandlers, bHandlerFidMatch, (void *)call->ulFid));
                    if(hlr != NULL) {
                        call->fHandler = hlr->fHandler;
                        call->pxHandlerContext = hlr->xHandlerContext;
                    }
                    else {
                        bApiCallSendStatus(call, API_CALL_ERROR_STATUS_NO_HANDLER);
                        call->session = NULL;
                    }
                }
                if(call->fHandler != NULL) {
                    uint8_t res = call->fHandler(call, &call->pxHandlerContext, call->ulCallPending, call->pucReqData, call->ulReqDataLen);
                    if(res && call->ulCallPending) 
                        call->ulCallPending--;
                    if(!call->ulCallPending)
                        call->session = NULL;
                }
            }
            else 
                vLinkedListInsertLast(&pxWsApiCall, pxWsApiNewCall);
            call->ulReqDataLen = 0;
            free(call->pucReqData);
            call->pucReqData = NULL;
            xSemaphoreGiveRecursive(xWsApiMutex);
        }
    }
    vTaskDelete(NULL);
}

httpd_uri_t *pxWsServerInit(char *uri) {
    httpd_uri_t *ws_h = malloc(sizeof(httpd_uri_t));
    if(ws_h != NULL) {
        memset(ws_h, 0, sizeof(httpd_uri_t));
        ws_h->uri                      = uri;
        ws_h->method                   = HTTP_GET;
        ws_h->handler                  = eWsHandler;
        ws_h->user_ctx                 = NULL; /* todo add context */
        ws_h->is_websocket             = true;
        ws_h->handle_ws_control_frames = true;
        static StaticSemaphore_t xSemBuffer;
        static StaticSemaphore_t xReqSemBuffer;
        xWsApiMutex = xSemaphoreCreateRecursiveMutexStatic( &xSemBuffer );
        xWsNewApiReqSem = xSemaphoreCreateCountingStatic(-1, 0, &xReqSemBuffer);
        xTaskCreate(vWsApiCallWorker, "ApiCallWork", 8192, NULL, uxTaskPriorityGet(NULL), NULL); /* todo add context */
    }
    return ws_h;
}


uint8_t api_call_register(api_handler_t, uint32_t, void *) __attribute__ ((alias ("bApiCallRegister")));
uint8_t api_call_unregister(uint32_t) __attribute__ ((alias ("bApiCallUnregister")));

uint8_t api_call_get_id(void *, uint32_t *) __attribute__ ((alias ("bApiCallGetId")));
void api_call_complete(void *) __attribute__ ((alias ("vApiCallComplete")));

uint8_t api_call_send_status(void *, uint32_t) __attribute__ ((alias ("bApiCallSendStatus")));
uint8_t api_call_send_json(void *, const uint8_t *, uint32_t) __attribute__ ((alias ("bApiCallSendJson")));
uint8_t api_call_send_json_fid_group(uint32_t, const uint8_t *, uint32_t) __attribute__ ((alias ("bApiCallSendJsonFidGroup")));
