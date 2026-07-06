#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "cJSON.h"
#include "CodeLib.h"
#include "ws_hal.h"
#include "web_api.h"
#include "ws_transport.h"

#ifndef CONFIG_WEB_SOCKET_PING_DELAY
#define CONFIG_WEB_SOCKET_PING_DELAY 1000
#endif

static const char *TAG = "ws_server";

typedef struct {
    __LinkedListObject__
    ApiHandler_t fHandler;
    uint32_t ulFid;
    void *xHandlerContext;
} ApiHandlerItem_t;

typedef struct {
    ws_conn_t *conn;
    int fd;
    uint32_t ulAliveTs;
    uint32_t ulPingTs;
} ApiSession_t;

typedef enum
{
    CALL_FLAG_NEW       = (1 << 0),
    CALL_FLAG_ONE_SHOT  = (1 << 1),
    CALL_FLAG_LONG_TERM = (1 << 2),
    CALL_FLAG_TO_DELETE = (1 << 3)
} call_flags_t;

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
    call_flags_t flags;
    /* CALL_FLAG_LONG_TERM throttling: client-supplied ARG.RDL (re-poll delay,
     * ms) converted to ticks at parse time. ulNextPollTick is armed each time
     * a response comes back in, so the call isn't re-forwarded to its
     * peripheral queue before that delay elapses. Both are 0 (poll again
     * immediately) for calls that don't send RDL, preserving prior behavior. */
    uint32_t ulPollIntervalTicks;
    uint32_t ulNextPollTick;
} ApiCall_t;

typedef struct {
    uint32_t counter;
    ws_frame_t frame;
    uint8_t payload[];
} ApiData_t;

static ws_mutex_t xWsApiMutex = NULL;
static LinkedList_t pxWsApiHandlers = NULL;
static LinkedList_t pxWsApiCall = NULL;
static LinkedList_t pxWsApiWaitingForResponse = NULL;
static ws_queue_t xWsWorkerQueue = NULL;
static uint32_t call_id = 1; /* 0 is reserved as "broadcast" sentinel in periph_msg.id */

static ws_queue_t ws_get_fid_queue(uint32_t ulFid);

void *get_ws_worker_queue(void) {
    return xWsWorkerQueue;
}

static uint8_t bHandlerFidMatch(LinkedListItem_t *item, void *arg) {
    uint32_t fid = (uint32_t)arg;
    return LinkedListGetObject(ApiHandlerItem_t, item)->ulFid == fid;
}

static uint8_t bCallClientMatch(LinkedListItem_t *item, void *arg) {
    uint32_t fd  = cl_tuple_get(arg, 0, uint32_t);
    uint32_t fid = cl_tuple_get(arg, 1, uint32_t);
    ApiCall_t *call = LinkedListGetObject(ApiCall_t, item);
    return call->session && (call->session->fd == fd) && (call->ulFid == fid);
}

static uint8_t bCallClientIdMatch(LinkedListItem_t *item, void *arg) {
    uint32_t fd  = cl_tuple_get(arg, 0, uint32_t);
    uint32_t fid = cl_tuple_get(arg, 1, uint32_t);
    uint32_t id  = cl_tuple_get(arg, 2, uint32_t);
    ApiCall_t *call = LinkedListGetObject(ApiCall_t, item);
    return call->session && (call->session->fd == fd) && (call->ulFid == fid) && (call->ulId == id);
}

static uint8_t bCallIdMatch(LinkedListItem_t *item, void *arg) {
    ApiCall_t *call = LinkedListGetObject(ApiCall_t, item);
    return (call->ulId == (uint32_t)arg) && call->session;
}

static uint8_t bCallFidMatch(LinkedListItem_t *item, void *arg) {
    uint32_t fid = (uint32_t)arg;
    ApiCall_t *call = LinkedListGetObject(ApiCall_t, item);
    return ((call->ulFid == fid) && call->session);
}

static void vBreakApiCallByFd(LinkedListItem_t *item, void *arg) {
    ApiCall_t *call = LinkedListGetObject(ApiCall_t, item);
    if(call->session && (call->session->fd == (uint32_t)arg)) {
        ws_hal_log_i(TAG, "Break call id:%lu", call->ulId);
        call->session = NULL;
    }
}

static void vBreakApiCallByFid(LinkedListItem_t *item, void *arg) {
    ApiCall_t *call = LinkedListGetObject(ApiCall_t, item);
    if(call->ulFid == (uint32_t)arg) {
        ws_hal_log_i(TAG, "Break call id:%lu", call->ulId);
        call->session = NULL;
    }
}

static void vBreakApiCallsByFd(uint32_t timeout_ms, uint32_t fd) {
    if(ws_hal_mutex_take(xWsApiMutex, timeout_ms)) {
        ulLinkedListDoForeach(pxWsApiCall, vBreakApiCallByFd, (void *)fd);
        ulLinkedListDoForeach(pxWsApiWaitingForResponse, vBreakApiCallByFd, (void *)fd);
        ws_hal_mutex_give(xWsApiMutex);
    }
}

static void vBreakApiCallsByFid(uint32_t timeout_ms, uint32_t fid) {
    if(ws_hal_mutex_take(xWsApiMutex, timeout_ms)) {
        ulLinkedListDoForeach(pxWsApiCall, vBreakApiCallByFid, (void *)fid);
        ulLinkedListDoForeach(pxWsApiWaitingForResponse, vBreakApiCallByFid, (void *)fid);
        ws_hal_mutex_give(xWsApiMutex);
    }
}

static void vServeApiCall(LinkedListItem_t *item, void *arg) {
    ApiCall_t *call = LinkedListGetObject(ApiCall_t, item);
    if(call->session) {
        static ws_frame_t ping_frame = {
            .type       = WS_FRAME_PING,
            .final      = true,
            .fragmented = false,
            .payload    = NULL,
            .len        = 0,
        };
        uint32_t now = ws_hal_tick();
        if(((now - call->session->ulAliveTs) >= CONFIG_WEB_SOCKET_PING_DELAY) &&
           ((now - call->session->ulPingTs) >= CONFIG_WEB_SOCKET_PING_DELAY)) {
            if(ws_conn_send_async(call->session->conn, &ping_frame, NULL, NULL) != 0) {
                ws_hal_log_w(TAG, "Send ping error");
                vApiCallComplete(call);
                return;
            }
            call->session->ulPingTs = ws_hal_tick();
        }
    } else {
        ws_hal_log_i(TAG, "Api call complete, id: %lu", call->ulId);
        vLinkedListUnlink(item);
        free(call->pucReqData);
        free(call);
    }
}

static void vWsTransferComplete_cb(int err, int fd, void *arg) {
    ApiData_t *apiData = (ApiData_t *)arg;
    ws_hal_log_i(TAG, "Transfer Complete, err: %d, fd: %x", err, fd);
    if(err)
        vBreakApiCallsByFd(100, fd);
    if((!apiData->counter) || !(--apiData->counter))
        free(arg);
}

uint8_t bApiCallRegister(ApiHandler_t fHandler, uint32_t ulFid, void *pxContext) {
    if((fHandler == NULL) ||
       (ulFid == API_HANDLER_ID_GENEGAL) ||
       (!ws_hal_mutex_take(xWsApiMutex, 10))) return 0;

    ApiHandlerItem_t *registered = LinkedListGetObject(ApiHandlerItem_t, pxLinkedListFindFirst(pxWsApiHandlers, bHandlerFidMatch, (void *)ulFid));
    if(registered != NULL) {
        ws_hal_mutex_give(xWsApiMutex);
        return 0;
    }
    ApiHandlerItem_t *hd = malloc(sizeof(ApiHandlerItem_t));
    if(hd == NULL) {
        ws_hal_mutex_give(xWsApiMutex);
        return 0;
    }
    hd->fHandler = fHandler;
    hd->ulFid = ulFid;
    hd->xHandlerContext = pxContext;
    vLinkedListInsertLast(&pxWsApiHandlers, LinkedListItem(hd));
    ws_hal_log_i(TAG, "Api handler registered %lu", hd->ulFid);
    ws_hal_mutex_give(xWsApiMutex);
    return 1;
}

uint8_t bApiCallUnregister(uint32_t ulFid) {
    if((ulFid == API_HANDLER_ID_GENEGAL) ||
       (!ws_hal_mutex_take(xWsApiMutex, 10))) return 0;
    ApiHandlerItem_t *registered = LinkedListGetObject(ApiHandlerItem_t, pxLinkedListFindFirst(pxWsApiHandlers, bHandlerFidMatch, (void *)ulFid));
    if(registered != NULL) {
        vBreakApiCallsByFid(WS_HAL_WAIT_NONE, ulFid);
        vLinkedListUnlink(LinkedListItem(registered));
        ws_hal_log_i(TAG, "Api handler unregistered %08lx", registered->ulFid);
        ws_hal_mutex_give(xWsApiMutex);
        return 1;
    }
    ws_hal_mutex_give(xWsApiMutex);
    return 0;
}

void vApiCallComplete(void *pxApiCall) {
    ws_hal_mutex_take(xWsApiMutex, WS_HAL_WAIT_FOREVER);
    ApiCall_t *call = (ApiCall_t *)pxApiCall;
    if(bLinkedListContains(pxWsApiCall, LinkedListItem(call)) ||
       bLinkedListContains(pxWsApiWaitingForResponse, LinkedListItem(call))) {
        if(call->ulCallPending)
            call->ulCallPending--;
        if(!call->ulCallPending)
            call->session = NULL;
    }
    ws_hal_mutex_give(xWsApiMutex);
}

uint8_t bApiCallGetId(void *pxApiCall, uint32_t *pulOutId) {
    if((pxApiCall == NULL) || (pulOutId == NULL)) return 0;
    ws_hal_mutex_take(xWsApiMutex, WS_HAL_WAIT_FOREVER);
    ApiCall_t *call = (ApiCall_t *)pxApiCall;
    if(bLinkedListContains(pxWsApiCall, LinkedListItem(call)) && call->session) {
        *pulOutId = call->ulId;
        ws_hal_mutex_give(xWsApiMutex);
        return 1;
    }
    ws_hal_mutex_give(xWsApiMutex);
    return 0;
}

static void _send_to_session(ApiCall_t *sub, ApiData_t *resp, int *res) {
    *res = ws_conn_send_async(sub->session->conn, &resp->frame, vWsTransferComplete_cb, resp);
    ws_hal_log_i(TAG, "Api call %lu json sending with result: %d", sub->ulId, *res);
    if(*res != 0) {
        if((!resp->counter) || !(--resp->counter))
            free(resp);
    }
}

static uint8_t _bApiCallSendJson(void *pxApiCall, uint32_t ulFid, const uint8_t *ucJson, uint32_t ulLen) {
    if(((pxApiCall == NULL) && (ulFid == 0)) || ((pxApiCall != NULL) && (ulFid != 0))) return 0;

    int res = 0;
    ApiCall_t *call = pxApiCall;
    uint32_t fid_out = (call != NULL) ? call->ulFid : ulFid;
    uint32_t sid = (call != NULL) ? call->ulId : 0;

    ws_hal_mutex_take(xWsApiMutex, WS_HAL_WAIT_FOREVER);

    uint32_t targets;
    if(call != NULL) {
        targets = (call->session != NULL) ? 1 : 0;
    } else {
        targets = ulLinkedListCount(pxWsApiWaitingForResponse, bCallFidMatch, (void *)ulFid)
                + ulLinkedListCount(pxWsApiCall, bCallFidMatch, (void *)ulFid);
    }

    if(targets == 0) {
        ws_hal_mutex_give(xWsApiMutex);
        return 0;
    }

    char pucTemplate[] = "{\"FID\":\"0x%08lx\",\"SID\":\"0x%08lx\",\"ARG\":";
    uint32_t len = ulLen + sizeof(pucTemplate) + 6;

    ApiData_t *resp = malloc(sizeof(ApiData_t) + len);
    if(resp != NULL) {
        int offset = sprintf((char *)resp->payload, pucTemplate, fid_out, sid);
        mem_cpy(&resp->payload[offset], ucJson, ulLen);
        offset += ulLen;
        resp->payload[offset] = '}';
        resp->counter        = targets;
        resp->frame.final      = true;
        resp->frame.fragmented = false;
        resp->frame.type       = WS_FRAME_TEXT;
        resp->frame.payload    = resp->payload;
        resp->frame.len        = len;

        if(call != NULL) {
            _send_to_session(call, resp, &res);
        } else {
            LinkedListItem_t *item = pxLinkedListFindFirst(pxWsApiWaitingForResponse, bCallFidMatch, (void *)ulFid);
            while(item != NULL) {
                ApiCall_t *sub = LinkedListGetObject(ApiCall_t, item);
                item = pxLinkedListFindNextNoOverlap(item, bCallFidMatch, (void *)ulFid);
                _send_to_session(sub, resp, &res);
            }
            item = pxLinkedListFindFirst(pxWsApiCall, bCallFidMatch, (void *)ulFid);
            while(item != NULL) {
                ApiCall_t *sub = LinkedListGetObject(ApiCall_t, item);
                item = pxLinkedListFindNextNoOverlap(item, bCallFidMatch, (void *)ulFid);
                _send_to_session(sub, resp, &res);
            }
        }
    } else {
        res = -1;
    }

    ws_hal_mutex_give(xWsApiMutex);
    return (res == 0);
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

/* --- FID → peripheral queue routing table -------------------------------- */

typedef struct {
    __LinkedListObject__
    uint32_t ulFid;
    void *xQueue;
} FidQueueItem_t;

static LinkedList_t pxWsFidQueues = NULL;

static uint8_t bFidQueueMatch(LinkedListItem_t *item, void *arg) {
    return LinkedListGetObject(FidQueueItem_t, item)->ulFid == (uint32_t)(uintptr_t)arg;
}

uint8_t ws_server_register_fid_queue(uint32_t ulFid, void *xQueue) {
    if(!ws_hal_mutex_take(xWsApiMutex, 10)) return 0;
    FidQueueItem_t *existing = LinkedListGetObject(FidQueueItem_t,
        pxLinkedListFindFirst(pxWsFidQueues, bFidQueueMatch, (void *)(uintptr_t)ulFid));
    if(existing) {
        ws_hal_mutex_give(xWsApiMutex);
        return 0;
    }
    FidQueueItem_t *fq = malloc(sizeof(FidQueueItem_t));
    if(!fq) {
        ws_hal_mutex_give(xWsApiMutex);
        return 0;
    }
    fq->ulFid  = ulFid;
    fq->xQueue = xQueue;
    vLinkedListInsertLast(&pxWsFidQueues, LinkedListItem(fq));
    ws_hal_log_i(TAG, "FID queue registered 0x%lx", ulFid);
    ws_hal_mutex_give(xWsApiMutex);
    return 1;
}

static ws_queue_t ws_get_fid_queue(uint32_t ulFid) {
    FidQueueItem_t *fq = LinkedListGetObject(FidQueueItem_t,
        pxLinkedListFindFirst(pxWsFidQueues, bFidQueueMatch, (void *)(uintptr_t)ulFid));
    return fq ? fq->xQueue : NULL;
}

static void ws_server_on_text(ws_conn_t *conn, uint8_t *data, size_t len);

/* --- Transport callbacks (called from ws_transport_httpd.c) -------------- */

void ws_server_on_frame(ws_conn_t *conn, ws_frame_type_t type, uint8_t *data, size_t len) {
    ApiSession_t *sess = ws_conn_get_user_data(conn);
    if (sess)
        sess->ulAliveTs = ws_hal_tick();

    switch (type) {
    case WS_FRAME_TEXT:
        ws_server_on_text(conn, data, len);
        return; /* ws_server_on_text takes ownership */
    case WS_FRAME_PING: {
        ws_frame_t pong = {
            .type = WS_FRAME_PONG, .final = true, .fragmented = false,
            .payload = data, .len = len,
        };
        ws_conn_send_async(conn, &pong, NULL, NULL);
        break;
    }
    case WS_FRAME_PONG:
        /* alive timestamp already updated above */
        break;
    case WS_FRAME_CLOSE: {
        ws_frame_t close_f = {.type = WS_FRAME_CLOSE, .final = true};
        ws_conn_send_async(conn, &close_f, NULL, NULL);
        break;
    }
    default:
        break;
    }
    free(data);
}

void ws_server_on_connect(ws_conn_t *conn) {
    ApiSession_t *sess = malloc(sizeof(ApiSession_t));
    if(!sess) return;
    sess->conn      = conn;
    sess->fd        = ws_conn_get_fd(conn);
    sess->ulAliveTs = sess->ulPingTs = ws_hal_tick();
    ws_conn_set_user_data(conn, sess);
    ws_hal_log_i(TAG, "New connection fd=%d sess=%p", sess->fd, (void *)sess);
}

static void ws_server_on_text(ws_conn_t *conn, uint8_t *data, size_t len) {
    ApiSession_t *sess = ws_conn_get_user_data(conn);
    if(!sess) return;

    if(!data || !len) {
        ws_hal_log_i(TAG, "Got packet with empty message");
        return;
    }
    ws_hal_log_i(TAG, "Got packet with message: %s", data);

    ApiCall_t *wscd = malloc(sizeof(ApiCall_t));
    if(!wscd) {
        ws_hal_log_w(TAG, "Failed to malloc memory for ws api call");
        free(data);
        return;
    }
    memset(wscd, 0, sizeof(ApiCall_t));
    wscd->flags |= CALL_FLAG_NEW;

    cJSON *json = cJSON_ParseWithLengthOpts((char *)data, len, 0, 0);
    free(data);
    if(!json) {
        ws_hal_log_w(TAG, "Invalid JSON");
        free(wscd);
        return;
    }

    /* FID can be a decimal integer or a hex string (e.g. "0x1012") */
    cJSON *fid_json = cJSON_GetObjectItem(json, "FID");
    if(cJSON_IsNumber(fid_json))
        wscd->ulFid = fid_json->valueint;
    else if(cJSON_IsString(fid_json))
        wscd->ulFid = (uint32_t)strtol(fid_json->valuestring, NULL, 16);

    if(wscd->ulFid == API_HANDLER_ID_GENEGAL) {
        ws_hal_log_w(TAG, "Api call bad FID property");
        free(wscd);
        cJSON_Delete(json);
        return;
    }

    /* Serialize ARG back to a compact string — handlers receive it as raw bytes */
    cJSON *arg_json = cJSON_GetObjectItem(json, "ARG");
    if(arg_json) {
        wscd->pucReqData   = (uint8_t *)cJSON_PrintUnformatted(arg_json);
        wscd->ulReqDataLen = lStrLen((char *)wscd->pucReqData);
        ws_hal_log_i(TAG, "Api arg %s", wscd->pucReqData);

        /* ARG.RDL (re-poll delay, ms) throttles CALL_FLAG_LONG_TERM re-arming;
         * generic framework field, meaningful to any FID that uses FLAGS:4. */
        cJSON *rdl_json = cJSON_GetObjectItem(arg_json, "RDL");
        if(cJSON_IsNumber(rdl_json) && rdl_json->valueint > 0)
            wscd->ulPollIntervalTicks = ws_hal_ms_to_ticks((uint32_t)rdl_json->valueint);
    }

    cJSON *flags_json = cJSON_GetObjectItem(json, "FLAGS");
    if(cJSON_IsNumber(flags_json))
        wscd->flags |= (uint32_t)flags_json->valueint;
    wscd->ulCallPending = 1;
    wscd->session = sess;

    cJSON *sid_json = cJSON_GetObjectItem(json, "SID");

    /* Both FLAGS and SID are mandatory — drop calls that omit either */
    if(!cJSON_IsNumber(flags_json) || !cJSON_IsNumber(sid_json)) {
        ws_hal_log_w(TAG, "Missing FLAGS or SID, dropping call fid:%lu", wscd->ulFid);
        bApiCallSendStatus(wscd, API_CALL_ERROR_STATUS_BAD_REQ);
        free(wscd->pucReqData);
        free(wscd);
        cJSON_Delete(json);
        return;
    }

    ws_hal_mutex_take(xWsApiMutex, WS_HAL_WAIT_FOREVER);
    wscd->ulId = (uint32_t)sid_json->valueint & 0xffff;

    /* Bind handler if registered; queue-routed FIDs may have no handler */
    ApiHandlerItem_t *hlr = LinkedListGetObject(ApiHandlerItem_t,
                                                pxLinkedListFindFirst(pxWsApiHandlers,
                                                                      bHandlerFidMatch,
                                                                      (void *)wscd->ulFid));
    uint8_t has_queue = (ws_get_fid_queue(wscd->ulFid) != NULL);
    if(!hlr && !has_queue) {
        ws_hal_log_w(TAG, "No handler or queue for FID %lu, dropping call id:%lu", wscd->ulFid, wscd->ulId);
        bApiCallSendStatus(wscd, API_CALL_ERROR_STATUS_NO_HANDLER);
        free(wscd->pucReqData);
        free(wscd);
    } else {
        if(hlr) {
            wscd->fHandler         = hlr->fHandler;
            wscd->pxHandlerContext = hlr->xHandlerContext;
        }
        vLinkedListInsertLast(&pxWsApiCall, LinkedListItem(wscd));
        ws_hal_log_i(TAG, "New api call %lu enqueued with id:%lu", wscd->ulFid, wscd->ulId);
    }

    ws_hal_mutex_give(xWsApiMutex);
    cJSON_Delete(json);
}

void ws_server_on_disconnect(ws_conn_t *conn) {
    ApiSession_t *sess = ws_conn_get_user_data(conn);
    if(!sess) return;
    ws_conn_set_user_data(conn, NULL);
    ws_hal_log_i(TAG, "Disconnect fd=%d sess=%p", sess->fd, (void *)sess);
    vBreakApiCallsByFd(WS_HAL_WAIT_FOREVER, sess->fd);
    free(sess);
}

/* --- Peripheral forwarding ----------------------------------------------- */

static void vForwardCallToPeripheral(ApiCall_t *call, ws_queue_t queue) {
    webapi_msg_t *msg = malloc(sizeof(webapi_msg_t));
    if(!msg)
        return;
    msg->fid = call->ulFid;
    msg->id  = call->ulId;
    msg->len = call->ulReqDataLen;

    if(call->flags & CALL_FLAG_LONG_TERM) {
        /* Long-term calls retain ownership of pucReqData so they can be re-polled.
         * Give the peripheral a copy that it can free independently. */
        if(call->pucReqData && call->ulReqDataLen > 0) {
            msg->data = malloc(call->ulReqDataLen + 1);
            if(!msg->data) { free(msg); return; }
            memcpy(msg->data, call->pucReqData, call->ulReqDataLen);
            msg->data[call->ulReqDataLen] = '\0';
        } else {
            msg->data = NULL;
        }
        if(!ws_hal_queue_send(queue, &msg, WS_HAL_WAIT_NONE)) {
            ws_hal_log_w(TAG, "periph queue full, FID 0x%lx dropped", call->ulFid);
            free(msg->data);
            free(msg);
            bApiCallSendStatus(call, API_CALL_STATUS_BUSY);
            call->session = NULL;
        }
    } else {
        msg->data = call->pucReqData;
        if(ws_hal_queue_send(queue, &msg, WS_HAL_WAIT_NONE)) {
            call->pucReqData = NULL; /* ownership transferred to msg */
        } else {
            ws_hal_log_w(TAG, "periph queue full, FID 0x%lx dropped", call->ulFid);
            free(msg);
            bApiCallSendStatus(call, API_CALL_STATUS_BUSY);
            call->session = NULL;
        }
    }
}

/* --- Worker task --------------------------------------------------------- */

static void vWsApiCallWorker(void *pvParameters) {
    for(;;) {
        /* 1. Keep connections alive and garbage-collect completed/dead calls */
        ws_hal_mutex_take(xWsApiMutex, WS_HAL_WAIT_FOREVER);
        ulLinkedListDoForeach(pxWsApiCall, vServeApiCall, NULL);
        ulLinkedListDoForeach(pxWsApiWaitingForResponse, vServeApiCall, NULL);
        ws_hal_mutex_give(xWsApiMutex);

        /* 2. Forward active calls to their peripheral queue */
        ws_hal_mutex_take(xWsApiMutex, WS_HAL_WAIT_FOREVER);
        LinkedListItem_t *item = pxLinkedListFindFirst(pxWsApiCall, NULL, NULL);
        while(item != NULL) {
            LinkedListItem_t *next = pxLinkedListFindNextNoOverlap(item, NULL, NULL);
            ApiCall_t *call = LinkedListGetObject(ApiCall_t, item);

            if(call->flags & CALL_FLAG_TO_DELETE) {
                ws_hal_log_i(TAG, "received status DELETE");
                bApiCallSendStatus(call, API_CALL_STATUS_DELIVERED);
                if(call->session) {
                    void *key = cl_tuple_make((void *)(uintptr_t)call->session->fd,
                                             (void *)(uintptr_t)call->ulFid,
                                             (void *)(uintptr_t)call->ulId);
                    call->session = NULL;
                    LinkedListItem_t *found;
                    while((found = pxLinkedListFindFirst(pxWsApiWaitingForResponse, bCallClientIdMatch, key)) != NULL) {
                        ApiCall_t *fc = LinkedListGetObject(ApiCall_t, found);
                        vLinkedListUnlink(found);
                        free(fc->pucReqData);
                        free(fc);
                    }
                    while((found = pxLinkedListFindFirst(pxWsApiCall, bCallClientIdMatch, key)) != NULL) {
                        ApiCall_t *fc = LinkedListGetObject(ApiCall_t, found);
                        vLinkedListUnlink(found);
                        free(fc->pucReqData);
                        free(fc);
                    }
                }
                vLinkedListUnlink(item);
                free(call->pucReqData);
                free(call);
                item = next;
                continue;
            }

            /* Long-term calls that haven't earned another poll yet (ARG.RDL)
             * stay put in pxWsApiCall until ulNextPollTick elapses, instead
             * of being re-forwarded to the peripheral queue immediately. */
            if((call->flags & CALL_FLAG_LONG_TERM) && !(call->flags & CALL_FLAG_NEW)
               && ((int32_t)(ws_hal_tick() - call->ulNextPollTick) < 0)) {
                item = next;
                continue;
            }

            ws_queue_t queue = ws_get_fid_queue(call->ulFid);
            if(queue) {
                vForwardCallToPeripheral(call, queue);
                vLinkedListInsertLast(&pxWsApiWaitingForResponse, item);

                if(call->flags & CALL_FLAG_NEW) {
                    ws_hal_log_i(TAG, "send status DELIVERED");
                    bool sent = bApiCallSendStatus(call, API_CALL_STATUS_DELIVERED);
                    if(sent)
                        call->flags &= ~CALL_FLAG_NEW;
                }
            } else {
                ws_hal_log_i(TAG, "no handler or queue for FID %lu, send status INVALID", call->ulFid);
                bApiCallSendStatus(call, API_CALL_STATUS_INVALID);
                vLinkedListUnlink(item);
                free(call->pucReqData);
                free(call);
            }
            item = next;
        }
        ws_hal_mutex_give(xWsApiMutex);

        /* 3. Handle responses from peripherals */
        webapi_msg_t periph_msg;
        while(ws_hal_queue_receive(xWsWorkerQueue, &periph_msg, 10)) {
            ws_hal_mutex_take(xWsApiMutex, WS_HAL_WAIT_FOREVER);

            if(periph_msg.id == 0) {
                /* Broadcast: deliver to all waiting subscribers of this FID */
                LinkedListItem_t *it = pxLinkedListFindFirst(pxWsApiWaitingForResponse, bCallFidMatch, (void *)periph_msg.fid);
                while(it != NULL) {
                    LinkedListItem_t *next_it = pxLinkedListFindNextNoOverlap(it, bCallFidMatch, (void *)periph_msg.fid);
                    ApiCall_t *c = LinkedListGetObject(ApiCall_t, it);
                    if(c->fHandler)
                        c->fHandler(c, &c->pxHandlerContext, c->ulCallPending, periph_msg.data, periph_msg.len);
                    if(periph_msg.len) {
                        ws_hal_log_i(TAG, "broadcast fid:%lu len:%lu", periph_msg.fid, periph_msg.len);
                        _bApiCallSendJson(c, 0, periph_msg.data, periph_msg.len);
                    }
                    if(!(c->flags & CALL_FLAG_LONG_TERM)) {
                        vLinkedListUnlink(it);
                        free(c);
                    } else {
                        c->ulNextPollTick = ws_hal_tick() + c->ulPollIntervalTicks;
                        vLinkedListInsertLast(&pxWsApiCall, it);
                    }
                    it = next_it;
                }
                goto next;
            }

            LinkedListItem_t *it2 = pxLinkedListFindFirst(pxWsApiWaitingForResponse, bCallIdMatch, (void *)periph_msg.id);
            if(!it2) {
                ws_hal_log_i(TAG, "Arrived message from peripheral with id %lu that are no tied to any API calls", periph_msg.id);
                goto next;
            }

            ApiCall_t *call2 = LinkedListGetObject(ApiCall_t, it2);
            if(!call2) {
                ws_hal_log_i(TAG, "System error");
                goto next;
            }

            if(call2->fHandler)
                call2->fHandler(call2, &call2->pxHandlerContext, call2->ulCallPending, periph_msg.data, periph_msg.len);
            if(periph_msg.len) {
                ws_hal_log_i(TAG, "sent: %s : %lu", periph_msg.data, periph_msg.len);
                _bApiCallSendJson(call2, 0, periph_msg.data, periph_msg.len);
            }

            if(!(call2->flags & CALL_FLAG_LONG_TERM)) {
                vLinkedListUnlink(it2);
                free(call2);
            } else {
                call2->ulNextPollTick = ws_hal_tick() + call2->ulPollIntervalTicks;
                vLinkedListInsertLast(&pxWsApiCall, it2);
            }
next:
            ws_hal_mutex_give(xWsApiMutex);
            free(periph_msg.data);
        }
    }

    ws_hal_task_self_delete();
}

void ws_server_init(void) {
    xWsApiMutex    = ws_hal_mutex_create();
    xWsWorkerQueue = ws_hal_queue_create(10, sizeof(webapi_msg_t));
    /* Same priority as ws_mb (5) and the HTTP server's default task priority.
     * Used to run one level above (6) "so responses are forwarded
     * immediately", but under sustained CALL_FLAG_LONG_TERM load this task
     * rarely blocks (its queue-receive keeps finding work), and being
     * strictly higher priority than httpd starved it of CPU entirely rather
     * than just delaying it. Matching priority lets FreeRTOS's time-slicing
     * round-robin give httpd (and ws_mb) a fair share even while this task
     * is continuously busy. */
    ws_hal_task_create(vWsApiCallWorker, "ApiCallWork", 8192, NULL, 5);
}

#ifdef WS_SERVER_TEST
/* Reset all global state between unit tests. Never call in production. */
void ws_server_test_reset(void) {
    LinkedListItem_t *item;
    while ((item = pxLinkedListFindFirst(pxWsApiCall, NULL, NULL)) != NULL) {
        ApiCall_t *c = LinkedListGetObject(ApiCall_t, item);
        vLinkedListUnlink(item);
        free(c->pucReqData);
        free(c);
    }
    while ((item = pxLinkedListFindFirst(pxWsApiWaitingForResponse, NULL, NULL)) != NULL) {
        ApiCall_t *c = LinkedListGetObject(ApiCall_t, item);
        vLinkedListUnlink(item);
        free(c->pucReqData);
        free(c);
    }
    while ((item = pxLinkedListFindFirst(pxWsApiHandlers, NULL, NULL)) != NULL) {
        ApiHandlerItem_t *h = LinkedListGetObject(ApiHandlerItem_t, item);
        vLinkedListUnlink(item);
        free(h);
    }
    while ((item = pxLinkedListFindFirst(pxWsFidQueues, NULL, NULL)) != NULL) {
        FidQueueItem_t *fq = LinkedListGetObject(FidQueueItem_t, item);
        vLinkedListUnlink(item);
        free(fq);
    }
}
#endif


uint8_t api_call_register(api_handler_t, uint32_t, void *) __attribute__ ((alias ("bApiCallRegister")));
uint8_t api_call_unregister(uint32_t) __attribute__ ((alias ("bApiCallUnregister")));

uint8_t api_call_get_id(void *, uint32_t *) __attribute__ ((alias ("bApiCallGetId")));
void api_call_complete(void *) __attribute__ ((alias ("vApiCallComplete")));

uint8_t api_call_send_status(void *, uint32_t) __attribute__ ((alias ("bApiCallSendStatus")));
uint8_t api_call_send_json(void *, const uint8_t *, uint32_t) __attribute__ ((alias ("bApiCallSendJson")));
uint8_t api_call_send_json_fid_group(uint32_t, const uint8_t *, uint32_t) __attribute__ ((alias ("bApiCallSendJsonFidGroup")));
