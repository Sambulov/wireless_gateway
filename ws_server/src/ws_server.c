#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "GwJson.h"
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
    char fid[API_FID_NAME_MAX];
    void *xHandlerContext;
} ApiHandlerItem_t;

typedef struct {
    ws_conn_t *conn;
    int fd;
    uint32_t ulAliveTs;
    uint32_t ulPingTs;
    /* Сборка фрагментированного сообщения: WebSocket разрешает резать одно
     * сообщение на несколько кадров (первый с FIN=0, дальше продолжения). */
    uint8_t *pucFrag;
    uint32_t ulFragLen;
} ApiSession_t;

/* Предел длины собранного сообщения — тот же, что у одного кадра: 16 КБ.
 * Столько влезает и в кучу (свободно ~150 КБ), и в разбор: токенов на такой
 * объём хватает, потому что ограничение по токенам отсекает «многоэлементные»
 * пакеты раньше, чем они что-то сломают. */
#define WS_FRAG_MAX_LEN 16384

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
    /* SID назначает клиент: он необязателен, и сервер лишь возвращает его в
     * ответе. Без него ответ уходит без поля SID (публичные подписки). */
    uint8_t  bHasSid;
    void *pxHandlerContext;
    ApiHandler_t fHandler;
    char fid[API_FID_NAME_MAX];
    uint32_t ulCallPending;
    /* Обработчику уже отдали первичный запрос: только после этого его можно
     * дёргать тактом работы, иначе он получит data == NULL раньше запроса. */
    uint8_t  bHandlerStarted;
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

static ws_queue_t ws_get_fid_queue(const char *name);

void *get_ws_worker_queue(void) {
    return xWsWorkerQueue;
}

static uint8_t bHandlerFidMatch(LinkedListItem_t *item, void *arg) {
    const char *name = (const char *)arg;
    return (name != NULL) && (strcmp(LinkedListGetObject(ApiHandlerItem_t, item)->fid, name) == 0);
}

static uint8_t bCallClientIdMatch(LinkedListItem_t *item, void *arg) {
    uint32_t fd  = cl_tuple_get(arg, 0, uint32_t);
    const char *name = cl_tuple_get(arg, 1, const char *);
    uint32_t id  = cl_tuple_get(arg, 2, uint32_t);
    ApiCall_t *call = LinkedListGetObject(ApiCall_t, item);
    return call->session && (call->session->fd == (int)fd)
           && (strcmp(call->fid, name) == 0) && (call->ulId == id);
}

static uint8_t bCallIdMatch(LinkedListItem_t *item, void *arg) {
    ApiCall_t *call = LinkedListGetObject(ApiCall_t, item);
    return (call->ulId == (uint32_t)arg) && call->session;
}

static uint8_t bCallFidMatch(LinkedListItem_t *item, void *arg) {
    const char *name = (const char *)arg;
    ApiCall_t *call = LinkedListGetObject(ApiCall_t, item);
    return (name != NULL) && (strcmp(call->fid, name) == 0) && call->session;
}

static void vBreakApiCallByFd(LinkedListItem_t *item, void *arg) {
    ApiCall_t *call = LinkedListGetObject(ApiCall_t, item);
    if(call->session && (call->session->fd == (uint32_t)arg)) {
        ws_hal_log_i(TAG, "Break call id:%lu", call->ulId);
        call->session = NULL;
    }
}

static void vBreakApiCallByFid(LinkedListItem_t *item, void *arg) {
    const char *name = (const char *)arg;
    ApiCall_t *call = LinkedListGetObject(ApiCall_t, item);
    if((name != NULL) && (strcmp(call->fid, name) == 0)) {
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

static void vBreakApiCallsByFid(uint32_t timeout_ms, const char *name) {
    if(ws_hal_mutex_take(xWsApiMutex, timeout_ms)) {
        ulLinkedListDoForeach(pxWsApiCall, vBreakApiCallByFid, (void *)name);
        ulLinkedListDoForeach(pxWsApiWaitingForResponse, vBreakApiCallByFid, (void *)name);
        ws_hal_mutex_give(xWsApiMutex);
    }
}

/* Объявлено заранее: завершение вызова зовётся из обслуживания, описано ниже. */
static void _vCallTeardown(ApiCall_t *call);

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
        ws_hal_log_d(TAG, "Api call complete, id: %lu", call->ulId);
        _vCallTeardown(call);
        vLinkedListUnlink(item);
        free(call->pucReqData);
        free(call);
    }
}

/* Принудительное завершение вызова: по контракту обработчик получает call == NULL
 * и обязан освободить свой контекст, возвращая FALSE. Пошаговое завершение
 * допускается, поэтому зовём до FALSE, но с ограничением — иначе обработчик,
 * который не понял завершение, повесит задачу. */
#define WS_API_TEARDOWN_STEPS 8

static void _vCallTeardown(ApiCall_t *call) {
    if(!call || !call->fHandler) return;
    for (int i = 0; i < WS_API_TEARDOWN_STEPS; i++) {
        if(call->fHandler(NULL, &call->pxHandlerContext, NULL, 0)) continue;
        break;
    }
    /* pxHandlerContext НЕ обнуляем: обработчик сам решает, что там оставить.
     * Для подписки (FLAGS:4) вызов после тел-дауна живёт дальше и при
     * следующем переопросе должен снова получить свой контекст регистрации —
     * иначе он не знает, какой периферией управляет. */
}

static void vWsTransferComplete_cb(int err, int fd, void *arg) {
    ApiData_t *apiData = (ApiData_t *)arg;
    ws_hal_log_d(TAG, "Transfer Complete, err: %d, fd: %x", err, fd);
    if(err)
        vBreakApiCallsByFd(100, fd);
    if((!apiData->counter) || !(--apiData->counter))
        free(arg);
}

uint8_t bApiCallRegister(ApiHandler_t fHandler, const char *name, void *pxContext) {
    char fid[API_FID_NAME_MAX];
    /* Имя обязательно: раньше «нет FID» выражалось нулевым кодом, теперь —
     * пустой строкой или NULL, и оба случая регистрацией не считаются. */
    if((fHandler == NULL) || !bApiFidSet(fid, name) ||
       (!ws_hal_mutex_take(xWsApiMutex, 10))) return 0;

    ApiHandlerItem_t *registered = LinkedListGetObject(ApiHandlerItem_t, pxLinkedListFindFirst(pxWsApiHandlers, bHandlerFidMatch, (void *)fid));
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
    mem_cpy(hd->fid, fid, sizeof(hd->fid));
    hd->xHandlerContext = pxContext;
    vLinkedListInsertLast(&pxWsApiHandlers, LinkedListItem(hd));
    ws_hal_log_i(TAG, "Api handler registered %s", hd->fid);
    ws_hal_mutex_give(xWsApiMutex);
    return 1;
}

uint8_t bApiCallUnregister(const char *name) {
    char fid[API_FID_NAME_MAX];
    if(!bApiFidSet(fid, name) ||
       (!ws_hal_mutex_take(xWsApiMutex, 10))) return 0;
    ApiHandlerItem_t *registered = LinkedListGetObject(ApiHandlerItem_t, pxLinkedListFindFirst(pxWsApiHandlers, bHandlerFidMatch, (void *)fid));
    if(registered != NULL) {
        vBreakApiCallsByFid(WS_HAL_WAIT_NONE, fid);
        vLinkedListUnlink(LinkedListItem(registered));
        ws_hal_log_i(TAG, "Api handler unregistered %s", registered->fid);
        free(registered);
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
    ws_hal_log_d(TAG, "Api call %lu json sending with result: %d", sub->ulId, *res);
    if(*res != 0) {
        if((!resp->counter) || !(--resp->counter))
            free(resp);
    }
}

/* Ответ конкретному вызову (pxApiCall != NULL) или рассылка всем вызовам с
 * этим именем (pxApiCall == NULL, задано имя). Смешивать нельзя: вызов уже
 * знает своё имя, а рассылка — нет. */
static uint8_t _bApiCallSendJson(void *pxApiCall, const char *group_name, const uint8_t *ucJson, uint32_t ulLen) {
    if(((pxApiCall == NULL) && ((group_name == NULL) || !group_name[0])) ||
       ((pxApiCall != NULL) && (group_name != NULL))) return 0;

    int res = 0;
    ApiCall_t *call = pxApiCall;
    const char *fid_out = (call != NULL) ? call->fid : group_name;
    uint32_t sid = (call != NULL) ? call->ulId : 0;

    ws_hal_mutex_take(xWsApiMutex, WS_HAL_WAIT_FOREVER);

    uint32_t targets;
    if(call != NULL) {
        targets = (call->session != NULL) ? 1 : 0;
    } else {
        targets = ulLinkedListCount(pxWsApiWaitingForResponse, bCallFidMatch, (void *)group_name)
                + ulLinkedListCount(pxWsApiCall, bCallFidMatch, (void *)group_name);
    }

    if(targets == 0) {
        ws_hal_mutex_give(xWsApiMutex);
        return 0;
    }

    /* SID в ответе — только если клиент его назначил. Публичные подписки
     * (рассылка по FID без конкретного вызова) идут без SID всегда. */
    uint8_t has_sid = (call != NULL) && call->bHasSid;
    /* В ответе — то же имя, что прислал клиент. Шаблоны с запасом под имя и
     * SID, длину берём по факту ниже. */
    char pucTemplate[] = "{\"FID\":\"%s\",\"SID\":\"0x%08lx\",\"ARG\":";
    char pucTemplateNoSid[] = "{\"FID\":\"%s\",\"ARG\":";
    uint32_t len = ulLen + sizeof(pucTemplate) + sizeof(pucTemplateNoSid)
                   + 2 * API_FID_NAME_MAX + 16;

    ApiData_t *resp = malloc(sizeof(ApiData_t) + len);
    if(resp != NULL) {
        int offset = has_sid
                   ? sprintf((char *)resp->payload, pucTemplate, fid_out, (unsigned long)sid)
                   : sprintf((char *)resp->payload, pucTemplateNoSid, fid_out);
        mem_cpy(&resp->payload[offset], ucJson, ulLen);
        offset += ulLen;
        resp->payload[offset] = '}';
        resp->counter        = targets;
        resp->frame.final      = true;
        resp->frame.fragmented = false;
        resp->frame.type       = WS_FRAME_TEXT;
        resp->frame.payload    = resp->payload;
        /* Длину берём по факту: шаблон без SID короче, и объявлять кадр по
         * длинному шаблону нельзя — клиент получит мусор в хвосте. */
        resp->frame.len        = (uint32_t)offset + 1;

        if(call != NULL) {
            _send_to_session(call, resp, &res);
        } else {
            LinkedListItem_t *item = pxLinkedListFindFirst(pxWsApiWaitingForResponse, bCallFidMatch, (void *)group_name);
            while(item != NULL) {
                ApiCall_t *sub = LinkedListGetObject(ApiCall_t, item);
                item = pxLinkedListFindNextNoOverlap(item, bCallFidMatch, (void *)group_name);
                _send_to_session(sub, resp, &res);
            }
            item = pxLinkedListFindFirst(pxWsApiCall, bCallFidMatch, (void *)group_name);
            while(item != NULL) {
                ApiCall_t *sub = LinkedListGetObject(ApiCall_t, item);
                item = pxLinkedListFindNextNoOverlap(item, bCallFidMatch, (void *)group_name);
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
    return _bApiCallSendJson(pxApiCall, NULL, ucJson, ulLen);
}

uint8_t bApiCallSendStatus(void *pxApiCall, uint32_t ulSta) {
    uint8_t sta[21];
    uint32_t len = sprintf((char *)sta, "{\"STA\":\"0x%08lx\"}", (unsigned long)ulSta);
    return _bApiCallSendJson(pxApiCall, NULL, sta, len);
}

uint8_t bApiCallSendJsonFidGroup(const char *name, const uint8_t *ucJson, uint32_t ulLen) {
    return _bApiCallSendJson(NULL, name, ucJson, ulLen);
}

/* --- FID → peripheral queue routing table -------------------------------- */

typedef struct {
    __LinkedListObject__
    char fid[API_FID_NAME_MAX];
    void *xQueue;
} FidQueueItem_t;

static LinkedList_t pxWsFidQueues = NULL;

static uint8_t bFidQueueMatch(LinkedListItem_t *item, void *arg) {
    const char *name = (const char *)arg;
    return (name != NULL) && (strcmp(LinkedListGetObject(FidQueueItem_t, item)->fid, name) == 0);
}

uint8_t ws_server_register_fid_queue(const char *name, void *xQueue) {
    char fid[API_FID_NAME_MAX];
    if(!bApiFidSet(fid, name) || !xQueue) return 0;
    if(!ws_hal_mutex_take(xWsApiMutex, 10)) return 0;
    FidQueueItem_t *existing = LinkedListGetObject(FidQueueItem_t,
        pxLinkedListFindFirst(pxWsFidQueues, bFidQueueMatch, (void *)fid));
    if(existing) {
        ws_hal_mutex_give(xWsApiMutex);
        return 0;
    }
    FidQueueItem_t *fq = malloc(sizeof(FidQueueItem_t));
    if(!fq) {
        ws_hal_mutex_give(xWsApiMutex);
        return 0;
    }
    mem_cpy(fq->fid, fid, sizeof(fq->fid));
    fq->xQueue = xQueue;
    vLinkedListInsertLast(&pxWsFidQueues, LinkedListItem(fq));
    ws_hal_log_i(TAG, "FID queue registered %s", fq->fid);
    ws_hal_mutex_give(xWsApiMutex);
    return 1;
}

static ws_queue_t ws_get_fid_queue(const char *name) {
    if(!name || !name[0]) return NULL;
    FidQueueItem_t *fq = LinkedListGetObject(FidQueueItem_t,
        pxLinkedListFindFirst(pxWsFidQueues, bFidQueueMatch, (void *)name));
    return fq ? fq->xQueue : NULL;
}

static void ws_server_on_text(ws_conn_t *conn, uint8_t *data, size_t len);

/* --- Transport callbacks (called from ws_transport_httpd.c) -------------- */

/* Добавить кусок фрагментированного сообщения. Возвращает 0, если сообщение
 * длиннее предела (тогда сессию закрываем: принимать его целиком негде). */
static uint8_t _bFragAppend(ApiSession_t *sess, const uint8_t *data, size_t len)
{
    if (!sess->pucFrag) {
        sess->pucFrag = malloc(WS_FRAG_MAX_LEN + 1);
        if (!sess->pucFrag)
            return 0;
        sess->ulFragLen = 0;
    }
    if (sess->ulFragLen + len > WS_FRAG_MAX_LEN)
        return 0;

    mem_cpy(sess->pucFrag + sess->ulFragLen, data, len);
    sess->ulFragLen += (uint32_t)len;
    return 1;
}

void ws_server_on_frame(ws_conn_t *conn, ws_frame_type_t type, uint8_t *data, size_t len,
                        uint8_t bFinal) {
    ApiSession_t *sess = ws_conn_get_user_data(conn);

    /* Фрагментированное сообщение: копим части, а разбираем собранное целиком.
     * Раньше продолжения не обрабатывались вовсе — сообщение молча терялось
     * вместе с ответом, и клиент ждал его до таймаута. */
    if (sess && (type == WS_FRAME_TEXT || type == WS_FRAME_CONT) && !bFinal) {
        if (!_bFragAppend(sess, data, len)) {
            ws_hal_log_w(TAG, "Фрагментированное сообщение больше %d байт, закрываю fd=%d",
                         WS_FRAG_MAX_LEN, sess->fd);
            free(sess->pucFrag);
            sess->pucFrag = NULL;
            sess->ulFragLen = 0;
            free(data);
            ws_frame_t close_f = {.type = WS_FRAME_CLOSE, .final = true};
            ws_conn_send_async(conn, &close_f, NULL, NULL);
            return;
        }
        sess->ulAliveTs = ws_hal_tick();
        free(data);
        return;
    }

    if (sess && type == WS_FRAME_CONT) {
        /* Последний фрагмент: собираем и отдаём как обычное сообщение. Буфер
         * переходит во владение ws_server_on_text, поэтому не копируем. */
        if (sess->pucFrag && _bFragAppend(sess, data, len)) {
            uint8_t *msg = sess->pucFrag;
            size_t   n   = sess->ulFragLen;
            sess->pucFrag = NULL;
            sess->ulFragLen = 0;
            free(data);
            msg[n] = '\0';
            sess->ulAliveTs = ws_hal_tick();
            ws_server_on_text(conn, msg, n);
            return;
        }
        ws_hal_log_w(TAG, "Продолжение без начала сообщения, отброшено fd=%d",
                     sess ? sess->fd : -1);
        free(sess->pucFrag);
        if (sess) { sess->pucFrag = NULL; sess->ulFragLen = 0; }
        free(data);
        return;
    }

    if (sess && type == WS_FRAME_TEXT && sess->pucFrag) {
        /* Новое сообщение началось, не дождавшись конца предыдущего: старое
         * незавершённое выбрасываем, оно уже не станет целым. */
        ws_hal_log_w(TAG, "Незавершённое сообщение отброшено, fd=%d", sess->fd);
        free(sess->pucFrag);
        sess->pucFrag = NULL;
        sess->ulFragLen = 0;
    }

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
    sess->pucFrag   = NULL;
    sess->ulFragLen = 0;
    ws_conn_set_user_data(conn, sess);
    ws_hal_log_i(TAG, "New connection fd=%d sess=%p", sess->fd, (void *)sess);
}

/* Сколько токенов отдаём разбору входящего кадра и разбору ARG. Токен — это
 * ключ, значение или скобка структуры; конверт протокола укладывается в
 * единицы, ARG запроса регистров — в десятки. Выход за предел означает отказ
 * разбора, а не рост памяти: токены лежат на стеке задачи, jsmn не рекурсивен. */
#define WS_FRAME_MAX_TOKENS 48
#define WS_ARG_MAX_TOKENS   48

static void ws_server_on_text(ws_conn_t *conn, uint8_t *data, size_t len) {
    ApiSession_t *sess = ws_conn_get_user_data(conn);
    if(!sess) {
        /* Кадр уже наш (транспорт передал владение): без сессии его некуда
         * адресовать, но и терять память нельзя. */
        free(data);
        return;
    }

    if(!data || !len) {
        ws_hal_log_i(TAG, "Got packet with empty message");
        free(data);
        return;
    }
    ws_hal_log_d(TAG, "Got packet with message: %s", data);

    ApiCall_t *wscd = malloc(sizeof(ApiCall_t));
    if(!wscd) {
        ws_hal_log_w(TAG, "Failed to malloc memory for ws api call");
        free(data);
        return;
    }
    memset(wscd, 0, sizeof(ApiCall_t));
    wscd->flags |= CALL_FLAG_NEW;

    /* Разбор конверта: jsmn, без выделения памяти на каждый объект. Токены —
     * на стеке задачи, поэтому вложенный или огромный пакет не может ни съесть
     * кучу, ни переполнить стек разбором (jsmn не рекурсивен). */
    GW_JSON_TOKENS(toks, WS_FRAME_MAX_TOKENS);
    gw_json_t json;
    if (!gw_json_parse(&json, (const char *)data, len, toks, WS_FRAME_MAX_TOKENS)) {
        ws_hal_log_w(TAG, "Кадр не разобран как JSON или слишком сложен (%u токенов)",
                     (unsigned)WS_FRAME_MAX_TOKENS);
        free(data);
        free(wscd);
        return;
    }

    /* FID — только строка: имя вида "<подсистема>.<действие>". Числовой код
     * отклоняем: протокол переведён на имена целиком, и принимать оба вида
     * значило бы держать два протокола в одном месте. */
    if (!gw_json_str(&json, "FID", wscd->fid, sizeof(wscd->fid))) {
        ws_hal_log_w(TAG, "Api call bad FID property (нужно имя строкой, например \"uart2.modbus\")");
        free(data);
        free(wscd);
        return;
    }

    /* ARG отдаём обработчику как есть — срезом исходного кадра, без дерева и
     * без печати в строку (у прежнего сборщика JSON это была вторая
     * аллокация на запрос). */
    const char *arg_s = NULL;
    size_t      arg_n = 0;
    if (gw_json_raw(&json, "ARG", &arg_s, &arg_n)) {
        wscd->pucReqData = (uint8_t *)malloc(arg_n + 1);
        if (!wscd->pucReqData) {
            free(data);
            free(wscd);
            return;
        }
        mem_cpy(wscd->pucReqData, arg_s, arg_n);
        wscd->pucReqData[arg_n] = '\0';
        wscd->ulReqDataLen = (uint32_t)arg_n;

        /* ARG.RDL (задержка повторного опроса, мс) — общее поле каркаса для
         * вызовов с FLAGS:4; смотрим тем же разбором, что и обработчики. */
        GW_JSON_TOKENS(arg_toks, WS_ARG_MAX_TOKENS);
        gw_json_t arg;
        if (gw_json_parse(&arg, (const char *)wscd->pucReqData, arg_n,
                          arg_toks, WS_ARG_MAX_TOKENS)) {
            int32_t rdl;
            if (gw_json_int(&arg, "RDL", &rdl) && rdl > 0)
                wscd->ulPollIntervalTicks = ws_hal_ms_to_ticks((uint32_t)rdl);
        }
    }

    /* Сессия нужна до первой проверки: ответ об ошибке уходит через неё. */
    wscd->session = sess;

    /* FLAGS остаётся обязательным полем протокола */
    int32_t flags;
    if (!gw_json_int(&json, "FLAGS", &flags)) {
        ws_hal_log_w(TAG, "Missing FLAGS, dropping call fid:%s", wscd->fid);
        bApiCallSendStatus(wscd, API_CALL_ERROR_STATUS_BAD_REQ);
        free(wscd->pucReqData);
        free(wscd);
        free(data);
        return;
    }
    wscd->flags |= (uint32_t)flags;
    wscd->ulCallPending = 1;

    /* SID назначает клиент, чтобы отличить ответ на свой запрос. Сервер его не
     * требует и не придумывает: нет SID — ответ уйдёт без этого поля. */
    int32_t sid;
    if (gw_json_int(&json, "SID", &sid)) {
        wscd->bHasSid = 1;
        wscd->ulId    = (uint32_t)sid;
    }

    /* Токены указывают внутрь кадра, поэтому буфер живёт до последнего чтения
     * из них (у прежнего сборщика JSON дерево владело своими копиями строк, и
     * это было неважно). */
    free(data);

    ws_hal_mutex_take(xWsApiMutex, WS_HAL_WAIT_FOREVER);

    /* Bind handler if registered; queue-routed FIDs may have no handler */
    ApiHandlerItem_t *hlr = LinkedListGetObject(ApiHandlerItem_t,
                                                pxLinkedListFindFirst(pxWsApiHandlers,
                                                                      bHandlerFidMatch,
                                                                      (void *)wscd->fid));
    uint8_t has_queue = (ws_get_fid_queue(wscd->fid) != NULL);
    if(!hlr && !has_queue) {
        ws_hal_log_w(TAG, "No handler or queue for FID %s, dropping call id:%lu", wscd->fid, wscd->ulId);
        bApiCallSendStatus(wscd, API_CALL_ERROR_STATUS_NO_HANDLER);
        free(wscd->pucReqData);
        free(wscd);
    } else {
        if(hlr) {
            wscd->fHandler         = hlr->fHandler;
            wscd->pxHandlerContext = hlr->xHandlerContext;
        }
        vLinkedListInsertLast(&pxWsApiCall, LinkedListItem(wscd));
        ws_hal_log_d(TAG, "New api call %s enqueued with id:%lu", wscd->fid, wscd->ulId);
    }

    ws_hal_mutex_give(xWsApiMutex);
}

void ws_server_on_disconnect(ws_conn_t *conn) {
    ApiSession_t *sess = ws_conn_get_user_data(conn);
    if(!sess) return;
    ws_conn_set_user_data(conn, NULL);
    ws_hal_log_i(TAG, "Disconnect fd=%d sess=%p", sess->fd, (void *)sess);
    vBreakApiCallsByFd(WS_HAL_WAIT_FOREVER, sess->fd);
    free(sess->pucFrag);
    free(sess);
}

/* --- Peripheral forwarding ----------------------------------------------- */

static void vForwardCallToPeripheral(ApiCall_t *call, ws_queue_t queue) {
    webapi_msg_t *msg = malloc(sizeof(webapi_msg_t));
    if(!msg)
        return;
    /* Имя копируем, а не передаём указателем: вызов живёт на своём стеке
     * задачи, а сообщение уезжает в очередь другого. */
    bApiFidSet(msg->fid, call->fid);
    msg->id  = call->ulId;
    msg->len = call->ulReqDataLen;
    /* Socket the request came in on, so handlers can tell AP clients from
     * station-side ones; the session may already be gone for parked calls. */
    msg->fd  = (call->session != NULL) ? call->session->fd : -1;

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
            ws_hal_log_w(TAG, "periph queue full, FID %s dropped", call->fid);
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
            ws_hal_log_w(TAG, "periph queue full, FID %s dropped", call->fid);
            free(msg);
            bApiCallSendStatus(call, API_CALL_STATUS_BUSY);
            call->session = NULL;
        }
    }
}

/* --- Worker task --------------------------------------------------------- */

/* One full pass of the worker: ping/GC, forward to peripherals, handle
 * peripheral responses. Split out of the task loop so host tests/fuzzers
 * can drive it synchronously (see ws_server_test_worker_step()). */
/* Такт работы. Ядро само решает, когда дёрнуть обработчики, — обработчик не
 * должен крутиться внутри себя в ожидании периферии. Такт ограничен по частоте,
 * а обработчик, которому нечего делать, возвращается сразу.
 *
 * Такт 10 мс стоил обработчику периферии двух лишних тактов на транзакцию
 * (старт и съём ответа), а при нескольких запросах в очереди — ещё и ожидания:
 * замерено на приборе 9600 бод: ожидание такта 52 мс, транзакция 70 мс при
 * физических 20 мс на шине, оборот 123 мс. С тактом 1 мс оборот падает до
 * времени самой шины, а обработчик пульса остаётся дешёвым: проверка состояния
 * и возврат. */
#define WS_API_WORK_TICK_MS 1
static uint32_t ulLastWorkTick;

/* Обработчики нельзя звать под xWsApiMutex: обработчик отправляет ответ через
 * bApiCallSendJson, а тот берёт тот же (нерекурсивный) мьютекс — выходит
 * самоблокировка и задача API встаёт намертво. Поэтому под мьютексом только
 * собираем список вызовов, а дёргаем их после освобождения. Освобождать вызовы
 * может лишь эта же задача, так что собранные указатели остаются валидными. */
#define WS_API_WORK_BATCH 16

/* Сколько токенов отдаём разбору входящего кадра и разбору ARG. Токен — это
 * ключ, значение или скобка структуры; конверт протокола укладывается в
 * единицы, ARG запроса регистров — в десятки. Выход за предел означает отказ
 * разбора (BAD_JSON), а не рост памяти: токены лежат на стеке задачи. */
#define WS_FRAME_MAX_TOKENS 48
#define WS_ARG_MAX_TOKENS   48
static ApiCall_t *apxWorkBatch[WS_API_WORK_BATCH];

static int vWsApiCollectHandled(LinkedList_t list, int from) {
    int n = from;
    LinkedListItem_t *item = pxLinkedListFindFirst(list, NULL, NULL);
    while(item != NULL && n < WS_API_WORK_BATCH) {
        LinkedListItem_t *next = pxLinkedListFindNextNoOverlap(item, NULL, NULL);
        ApiCall_t *call = LinkedListGetObject(ApiCall_t, item);
        if(call->fHandler && call->bHandlerStarted && !(call->flags & CALL_FLAG_TO_DELETE))
            apxWorkBatch[n++] = call;
        item = next;
    }
    return n;
}

static void vWsApiWorkCycle(void) {
    uint32_t now = ws_hal_tick();
    if((now - ulLastWorkTick) < ws_hal_ms_to_ticks(WS_API_WORK_TICK_MS)) return;
    ulLastWorkTick = now;

    ws_hal_mutex_take(xWsApiMutex, WS_HAL_WAIT_FOREVER);
    int n = vWsApiCollectHandled(pxWsApiCall, 0);
    n = vWsApiCollectHandled(pxWsApiWaitingForResponse, n);
    ws_hal_mutex_give(xWsApiMutex);

    for(int i = 0; i < n; i++) {
        ApiCall_t *call = apxWorkBatch[i];
        /* data == NULL — такт работы, а не запрос клиента */
        if(call->fHandler(call, &call->pxHandlerContext, NULL, 0)) continue;

        /* Обработчик вернул FALSE — вызов завершён. Завершаем его здесь же.
         * В очереди периферии это делал приход ответа; у обработчика ответ
         * отправляет он сам, и без этого вызов с контекстом оставался в списке
         * навсегда — замерено накопление 215 вызовов и контекстов за минуту. */
        ws_hal_mutex_take(xWsApiMutex, WS_HAL_WAIT_FOREVER);
        _vCallTeardown(call);
        LinkedListItem_t *li = LinkedListItem(call);
        if(bLinkedListContains(pxWsApiCall, li)) vLinkedListUnlink(li);
        if(bLinkedListContains(pxWsApiWaitingForResponse, li)) vLinkedListUnlink(li);
        if(bLinkedListContains(pxWsApiCall, li) || bLinkedListContains(pxWsApiWaitingForResponse, li)) {
            ws_hal_mutex_give(xWsApiMutex);
            continue;                      /* ещё где-то в списках — не трогаем */
        }
        if(call->flags & CALL_FLAG_LONG_TERM) {
            /* Подписка: вызов не закрываем, а возвращаем в работу — ядро само
             * повторит запрос (по ARG.RDL или сразу), и обработчик получит его
             * заново как обычный запрос клиента. Данные запроса сохраняем. */
            call->ulNextPollTick = ws_hal_tick() + call->ulPollIntervalTicks;
            vLinkedListInsertLast(&pxWsApiCall, li);
            ws_hal_mutex_give(xWsApiMutex);
            continue;
        }
        free(call->pucReqData);
        free(call);
        ws_hal_mutex_give(xWsApiMutex);
    }
}

static void vWsApiCallWorkerStep(void) {
        /* 0. Такт работы: обработчики получают возможность сделать своё
         * периодическое дело, не блокируя ядро */
        vWsApiWorkCycle();

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
                /* Обработчик завершил вызов (вернул FALSE) — отдаём ему
                 * завершение, чтобы он освободил свой контекст. Без этого
                 * контексты копились: вызов уходил из списков, а память нет. */
                _vCallTeardown(call);
                bApiCallSendStatus(call, API_CALL_STATUS_DELIVERED);
                if(call->session) {
                    void *key = cl_tuple_make((void *)(uintptr_t)call->session->fd,
                                             (void *)call->fid,
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
                    /* The loop above may have freed the call `next` points to
                     * (same fd/FID/SID queued right after this TO_DELETE) —
                     * re-read it; `item` is still linked and never matches
                     * itself since its session was cleared above. */
                    next = pxLinkedListFindNextNoOverlap(item, NULL, NULL);
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

            ws_queue_t queue = ws_get_fid_queue(call->fid);
            if(queue) {
                vForwardCallToPeripheral(call, queue);
                vLinkedListInsertLast(&pxWsApiWaitingForResponse, item);

                if(call->flags & CALL_FLAG_NEW) {
                    ws_hal_log_d(TAG, "send status DELIVERED");
                    bool sent = bApiCallSendStatus(call, API_CALL_STATUS_DELIVERED);
                    if(sent)
                        call->flags &= ~CALL_FLAG_NEW;
                }
            } else if(call->fHandler) {
                /* Обработчик зарегистрирован напрямую (без очереди периферии):
                 * отдаём ему запрос. Дальше вызов живёт по такту работы —
                 * обработчик получает data == NULL и завершается, вернув FALSE. */
                if(call->flags & CALL_FLAG_NEW) {
                    bool sent = bApiCallSendStatus(call, API_CALL_STATUS_DELIVERED);
                    if(sent)
                        call->flags &= ~CALL_FLAG_NEW;
                }
                call->bHandlerStarted = 1;
                /* Переносим в список «в работе»: иначе следующий проход снова
                 * отдаст обработчику тот же запрос. */
                vLinkedListInsertLast(&pxWsApiWaitingForResponse, item);
                /* Зовём обработчик без мьютекса: он отправляет ответ через API.
                 * Мьютекс отпускаем и берём заново — такт работы и удаление
                 * вызовов всё равно в этой же задаче. */
                ws_hal_mutex_give(xWsApiMutex);
                uint8_t alive = call->fHandler(call, &call->pxHandlerContext,
                                               call->pucReqData, call->ulReqDataLen);
                ws_hal_mutex_take(xWsApiMutex, WS_HAL_WAIT_FOREVER);
                if(!alive) {
                    _vCallTeardown(call);
                    if(call->flags & CALL_FLAG_LONG_TERM) {
                        /* Подписка живёт дальше: возвращаем её в работу, ядро
                         * повторит запрос по таймеру переопроса. */
                        call->ulNextPollTick = ws_hal_tick() + call->ulPollIntervalTicks;
                        vLinkedListUnlink(item);
                        vLinkedListInsertLast(&pxWsApiCall, item);
                    } else {
                        vLinkedListUnlink(item);
                        free(call->pucReqData);
                        free(call);
                    }
                }
            } else {
                ws_hal_log_i(TAG, "no handler or queue for FID %s, send status INVALID", call->fid);
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
                        c->fHandler(c, &c->pxHandlerContext, periph_msg.data, periph_msg.len);
                    if(periph_msg.len) {
                        ws_hal_log_i(TAG, "broadcast fid:%s len:%lu", periph_msg.fid, periph_msg.len);
                        _bApiCallSendJson(c, NULL, periph_msg.data, periph_msg.len);
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
                call2->fHandler(call2, &call2->pxHandlerContext, periph_msg.data, periph_msg.len);
            if(periph_msg.len) {
                ws_hal_log_d(TAG, "sent: %s : %lu", periph_msg.data, periph_msg.len);
                _bApiCallSendJson(call2, NULL, periph_msg.data, periph_msg.len);
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

static void vWsApiCallWorker(void *pvParameters) {
    for(;;)
        vWsApiCallWorkerStep();

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
/* Run one worker iteration synchronously — the host has no worker task. */
void ws_server_test_worker_step(void) {
    vWsApiCallWorkerStep();
}

/* Run one work cycle synchronously: обработчики получают свой такт. Гейт по
 * времени обойдён значением тика, которое выставляет сам тест. */
void ws_server_test_work_step(void) {
    vWsApiWorkCycle();
}

/* Reset all global state between unit tests. Never call in production. */
void ws_server_test_reset(void) {
    LinkedListItem_t *item;
    /* Peripheral responses nobody consumed: the worker would free .data */
    webapi_msg_t periph_msg;
    while (ws_hal_queue_receive(xWsWorkerQueue, &periph_msg, WS_HAL_WAIT_NONE))
        free(periph_msg.data);
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


uint8_t api_call_register(api_handler_t, const char *, void *) __attribute__ ((alias ("bApiCallRegister")));
uint8_t api_call_unregister(const char *) __attribute__ ((alias ("bApiCallUnregister")));

uint8_t api_call_get_id(void *, uint32_t *) __attribute__ ((alias ("bApiCallGetId")));
void api_call_complete(void *) __attribute__ ((alias ("vApiCallComplete")));

uint8_t api_call_send_status(void *, uint32_t) __attribute__ ((alias ("bApiCallSendStatus")));
uint8_t api_call_send_json(void *, const uint8_t *, uint32_t) __attribute__ ((alias ("bApiCallSendJson")));
uint8_t api_call_send_json_fid_group(const char *, const uint8_t *, uint32_t) __attribute__ ((alias ("bApiCallSendJsonFidGroup")));
