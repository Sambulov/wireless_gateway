#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stddef.h>

#include "GwJson.h"
#include "CodeLib.h"
#include "ws_hal.h"
#include "web_api.h"
#include "ws_transport.h"

static const char *TAG = "ws_server";

typedef struct {
    __LinkedListObject__
    ApiHandler_t fHandler;
    char fid[API_FID_NAME_MAX];
    void *xHandlerContext;
} ApiHandlerItem_t;

/* Сессия ядра — соединение транспорта, к которому привязаны вызовы. Дескриптор
 * соединения (self) для ядра непрозрачен: это ключ, который транспорт вернёт в
 * событиях, и больше ничего — тип соединения транспорта ядру неизвестен. */
typedef struct {
    __LinkedListObject__
    const void *self;
    /* Числовой идентификатор соединения, который даёт транспорт (для сокетного
     * транспорта — номер сокета). Ядро кладёт его в webapi_msg_t.fd: обработчики
     * по нему отличают запрос из softAP (request_from_softap). */
    int nPeerId;
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

/* Блок ответа. Счётчик получателей лежит ПЕРЕД полезной нагрузкой не случайно:
 * транспорт в событии завершения возвращает ровно тот указатель, который получил
 * в begin_send (то есть payload), — по нему и находится счётчик. Кадр ответа
 * собирает транспорт, ядру о кадрах знать нечего. */
typedef struct {
    uint32_t counter;
    uint32_t ulLen;
    uint8_t  payload[1];
} ApiData_t;

/* Из указателя, вернувшегося в событии завершения, — к своему блоку ответа. */
static ApiData_t *pxApiDataFromPayload(const void *payload) {
    return (ApiData_t *)(uintptr_t)((const uint8_t *)payload - offsetof(ApiData_t, payload));
}

/* Транспорт, к которому подключено ядро. Получаем его один раз
 * (ws_server_transport_attach) и дальше работаем только через него: обращений к
 * платформе в ядре нет — всё платформенное живёт в транспорте. */
typedef struct {
    const ws_transport_iface_t *iface;
    void *module;   /* self для операций транспорта как модуля */
} ws_server_transport_t;

static ws_server_transport_t xTransport = { NULL, NULL };

static ws_mutex_t xWsApiMutex = NULL;
static LinkedList_t pxWsApiHandlers = NULL;
static LinkedList_t pxWsApiCall = NULL;
static LinkedList_t pxWsApiWaitingForResponse = NULL;
static LinkedList_t pxWsSessions = NULL;
static ws_queue_t xWsWorkerQueue = NULL;

static ws_queue_t ws_get_fid_queue(const char *name);

/* --- Соединения ---------------------------------------------------------- */

/* Поиск сессии по дескриптору соединения. Дескриптор сравнивается как ключ:
 * ядро в него не заглядывает, тип соединения транспорта ему неизвестен. */
static ApiSession_t *pxSessionFind(const void *self) {
    LinkedListItem_t *item = pxLinkedListFindFirst(pxWsSessions, NULL, NULL);
    while(item != NULL) {
        ApiSession_t *sess = LinkedListGetObject(ApiSession_t, item);
        if(sess->self == self) return sess;
        item = pxLinkedListFindNextNoOverlap(item, NULL, NULL);
    }
    return NULL;
}

static void vBreakApiCallBySession(LinkedListItem_t *item, void *arg) {
    ApiCall_t *call = LinkedListGetObject(ApiCall_t, item);
    if(call->session == (ApiSession_t *)arg) {
        ws_hal_log_i(TAG, "Break call id:%lu", call->ulId);
        call->session = NULL;
    }
}

/* Вызовы снятого соединения: клиент ушёл — отвечать некому. Сами вызовы
 * освободит такт работы, и лишь после FALSE обработчика. */
static void vBreakApiCallsBySession(ApiSession_t *sess) {
    ulLinkedListDoForeach(pxWsApiCall, vBreakApiCallBySession, (void *)sess);
    ulLinkedListDoForeach(pxWsApiWaitingForResponse, vBreakApiCallBySession, (void *)sess);
}

void *get_ws_worker_queue(void) {
    return xWsWorkerQueue;
}

static uint8_t bHandlerFidMatch(LinkedListItem_t *item, void *arg) {
    const char *name = (const char *)arg;
    return (name != NULL) && (strcmp(LinkedListGetObject(ApiHandlerItem_t, item)->fid, name) == 0);
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

static void vBreakApiCallByFid(LinkedListItem_t *item, void *arg) {
    const char *name = (const char *)arg;
    ApiCall_t *call = LinkedListGetObject(ApiCall_t, item);
    if((name != NULL) && (strcmp(call->fid, name) == 0)) {
        ws_hal_log_i(TAG, "Break call id:%lu", call->ulId);
        call->session = NULL;
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
static uint8_t _bCallTeardown(ApiCall_t *call);

/* Такт обслуживания вызова. Ядро больше не пингует клиента: управляющие кадры —
 * дело транспорта, а живость соединения видна по событиям TRANSFER_ERR
 * (отправка не прошла) и CLOSED (соединение разорвано). */
static void vServeApiCall(LinkedListItem_t *item, void *arg) {
    (void)arg;
    ApiCall_t *call = LinkedListGetObject(ApiCall_t, item);
    if(call->session) return;   /* соединение живо — делать нечего */

    /* Клиента нет (отключился или ответ уже отправлен): просим снять вызов.
     * Сам вызов освободит такт работы — и только после того, как обработчик
     * вернёт FALSE. Раньше его снимали здесь, не дожидаясь обработчика. */
    ws_hal_log_d(TAG, "Api call complete, id: %lu", call->ulId);
    call->flags |= CALL_FLAG_TO_DELETE;
}

/* Принудительное завершение вызова: по контракту обработчик получает call == NULL
 * и обязан освободить свой контекст, возвращая FALSE.
 *
 * ЯДРО НЕ СНИМАЕТ ВЫЗОВ, ПОКА ОБРАБОТЧИК НЕ ВЕРНУЛ РЕЗУЛЬТАТ. Одна попытка за
 * такт: если обработчик завершается пошагово (вернул TRUE), вызов остаётся в
 * списках и его спросят снова следующим тактом — освобождать память вызова,
 * пока обработчик ещё держит своё состояние, нельзя. */
static uint8_t _bCallTeardown(ApiCall_t *call) {
    if(!call || !call->fHandler || !call->bHandlerStarted) return 1;  /* завершать нечего */
    return call->fHandler(NULL, &call->pxHandlerContext, NULL, 0) ? 0 : 1;
    /* pxHandlerContext НЕ обнуляем: обработчик сам решает, что там оставить.
     * Для подписки (FLAGS:4) вызов после тел-дауна живёт дальше и при
     * следующем переопросе должен снова получить свой контекст регистрации —
     * иначе он не знает, какой периферией управляет. */
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

/* Отправка через транспорт: ядро отдаёт СООБЩЕНИЕ, кадры строит транспорт.
 * Результат отправки приходит событием TRANSFER_COMPLETE/TRANSFER_ERR, поэтому
 * блок ответа живёт до него (в событии транспорт вернёт тот же указатель). */
static void _send_to_session(ApiCall_t *sub, ApiData_t *resp, ws_transport_result_t *res) {
    *res = xTransport.iface->begin_send(sub->session->self, resp->payload, resp->ulLen);
    ws_hal_log_d(TAG, "Api call %lu json sending with result: %d", sub->ulId, *res);
    if(*res != WS_TRANSPORT_OK) {
        /* Транспорт отправку не принял — события не будет, освобождаем сами. */
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

    ws_transport_result_t res = WS_TRANSPORT_INVALID;
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
        /* Длину берём по факту: шаблон без SID короче, и объявлять сообщение по
         * длинному шаблону нельзя — клиент получит мусор в хвосте. */
        resp->ulLen          = (uint32_t)offset + 1;

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
        res = WS_TRANSPORT_FAULT;
    }

    ws_hal_mutex_give(xWsApiMutex);
    return (res == WS_TRANSPORT_OK);
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

/* --- События транспорта --------------------------------------------------- */

/* Сколько токенов отдаём разбору входящего сообщения и разбору ARG. Токен — это
 * ключ, значение или скобка структуры; конверт протокола укладывается в
 * единицы, ARG запроса регистров — в десятки. Выход за предел означает отказ
 * разбора, а не рост памяти: токены лежат на стеке задачи, jsmn не рекурсивен. */
#define WS_FRAME_MAX_TOKENS 48
#define WS_ARG_MAX_TOKENS   48

static void vSessionOnMessage(ApiSession_t *sess, const void *data, uint32_t len);

/* Принято новое соединение: заводим сессию ядра, привязанную к дескриптору
 * транспорта. Больше про соединение ядро ничего не знает. */
static ws_transport_result_t bSessionOpen(const void *self) {
    ApiSession_t *sess = malloc(sizeof(ApiSession_t));
    if(sess == NULL) {
        ws_hal_log_w(TAG, "No memory for a session, connection %p", self);
        return WS_TRANSPORT_FAULT;   /* транспорту нечем это соединение обслужить */
    }
    sess->self    = self;
    sess->nPeerId = ((xTransport.iface != NULL) && (xTransport.iface->peer_id != NULL))
                  ? (int)xTransport.iface->peer_id(self) : -1;

    ws_hal_mutex_take(xWsApiMutex, WS_HAL_WAIT_FOREVER);
    vLinkedListInsertLast(&pxWsSessions, LinkedListItem(sess));
    ws_hal_mutex_give(xWsApiMutex);

    ws_hal_log_i(TAG, "Connection established (id=%d), session=%p", sess->nPeerId, (void *)sess);
    return WS_TRANSPORT_OK;
}

/* Соединение разорвано: сессию освобождаем, вызовы этого соединения снимаем —
 * отвечать некому. Сами вызовы освободит такт работы, и лишь после того, как
 * обработчик вернёт FALSE. */
static void vSessionClose(const void *self) {
    ws_hal_mutex_take(xWsApiMutex, WS_HAL_WAIT_FOREVER);
    ApiSession_t *sess = pxSessionFind(self);
    if(sess != NULL) {
        vLinkedListUnlink(LinkedListItem(sess));
        vBreakApiCallsBySession(sess);
    }
    ws_hal_mutex_give(xWsApiMutex);

    if(sess == NULL) {
        /* Транспорт вправе сообщить о закрытии соединения, которого ядро не
         * принимало (например, закрытие после неудачной отправки). */
        ws_hal_log_d(TAG, "Closed unknown connection %p", self);
        return;
    }
    ws_hal_log_i(TAG, "Connection closed (id=%d), session=%p released", sess->nPeerId, (void *)sess);
    free(sess);
}

/* Отправка завершилась — успешно или нет. Транспорт вернул ровно тот указатель,
 * который получил в begin_send, поэтому по нему находим блок ответа и
 * освобождаем его, когда последний получатель рассылки закончил отправку. */
static ws_transport_result_t vTransferDone(const void *self, const ws_transport_event_trigger_t *ev) {
    if(ev->data.ptr == NULL) {
        ws_hal_log_w(TAG, "Transfer event without data, connection %p", self);
        return WS_TRANSPORT_BAD_CALL;
    }

    if(ev->event == WS_TRANSPORT_TRANSFER_ERR) {
        /* Отправка не прошла — соединение считаем мёртвым: вызовы снимаем, а
         * соединение закрываем. Живость соединения определяется именно так:
         * ядро больше не пингует клиента (это дело транспорта). */
        ws_hal_log_w(TAG, "Send failed, closing connection %p", self);
        ws_hal_mutex_take(xWsApiMutex, WS_HAL_WAIT_FOREVER);
        ApiSession_t *sess = pxSessionFind(self);
        if(sess != NULL) vBreakApiCallsBySession(sess);
        ws_hal_mutex_give(xWsApiMutex);
        if((xTransport.iface != NULL) && (xTransport.iface->close != NULL))
            xTransport.iface->close(self);
        /* self транспорту больше не принадлежит: дальше им не пользуемся */
    } else {
        ws_hal_log_d(TAG, "Transfer complete, connection %p", self);
    }

    ApiData_t *resp = pxApiDataFromPayload(ev->data.ptr);
    if((resp->counter == 0) || (--resp->counter == 0))
        free(resp);
    return WS_TRANSPORT_OK;
}

/* Единственная точка входа транспорта в ядро: ядро не знает ни платформы, ни
 * WebSocket — только этот интерфейс. */
static ws_transport_result_t wsServerTransportEvent(const void *self,
                                                    const ws_transport_event_trigger_t *ev) {
    if((self == NULL) || (ev == NULL)) return WS_TRANSPORT_BAD_CALL;

    switch(ev->event) {
    case WS_TRANSPORT_ESTABLISHED:
        return bSessionOpen(self);

    case WS_TRANSPORT_CLOSED:
        vSessionClose(self);
        return WS_TRANSPORT_OK;

    case WS_TRANSPORT_RECEIVED: {
        /* Транспорт отдаёт УРОВЕНЬ СООБЩЕНИЙ: фрагменты он уже склеил сам, и
         * буфер принадлежит ему — ядро только читает его до возврата отсюда. */
        ws_hal_mutex_take(xWsApiMutex, WS_HAL_WAIT_FOREVER);
        ApiSession_t *sess = pxSessionFind(self);
        ws_hal_mutex_give(xWsApiMutex);
        if(sess == NULL) {
            ws_hal_log_w(TAG, "Message for unknown connection %p, dropped", self);
            return WS_TRANSPORT_BAD_CALL;
        }
        vSessionOnMessage(sess, ev->data.ptr, ev->data.length);
        return WS_TRANSPORT_OK;
    }

    case WS_TRANSPORT_TRANSFER_COMPLETE:
    case WS_TRANSPORT_TRANSFER_ERR:
        return vTransferDone(self, ev);

    default:
        /* OPEN и TRANSFER_BUFFER ядру не нужны: слушает транспорт, буферов он
         * ядру не выдаёт. */
        ws_hal_log_d(TAG, "Unhandled transport event 0x%x", (unsigned)ev->event);
        return WS_TRANSPORT_BAD_CALL;
    }
}

/* Подключение транспорта — единственное место, где ядро о нём узнаёт. */
ws_transport_result_t ws_server_transport_attach(const ws_transport_iface_t *iface, void *module) {
    if((iface == NULL) || (iface->begin_send == NULL) ||
       (iface->close == NULL) || (iface->subscribe == NULL)) return WS_TRANSPORT_BAD_CALL;

    xTransport.iface  = iface;
    xTransport.module = module;

    ws_transport_result_t res = iface->subscribe(module, wsServerTransportEvent, NULL);
    if(res != WS_TRANSPORT_OK) {
        xTransport.iface  = NULL;
        xTransport.module = NULL;
        ws_hal_log_w(TAG, "Transport subscribe failed: %d", (int)res);
        return res;
    }
    ws_hal_log_i(TAG, "Transport attached, module=%p", module);
    return WS_TRANSPORT_OK;
}

/* Разбор принятого СООБЩЕНИЯ (JSON). Транспорт уже склеил фрагменты, так что
 * здесь всегда целое сообщение; буфер принадлежит транспорту и живёт до возврата
 * отсюда — токены jsmn указывают внутрь него, поэтому ядро его не освобождает. */
static void vSessionOnMessage(ApiSession_t *sess, const void *data, uint32_t len) {
    if(!data || !len) {
        ws_hal_log_i(TAG, "Got packet with empty message");
        return;
    }
    ws_hal_log_d(TAG, "Got message (%lu bytes): %.*s", len, (int)len, (const char *)data);

    ApiCall_t *wscd = malloc(sizeof(ApiCall_t));
    if(!wscd) {
        ws_hal_log_w(TAG, "Failed to malloc memory for ws api call");
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
        ws_hal_log_w(TAG, "Сообщение не разобрано как JSON или слишком сложно (%u токенов)",
                     (unsigned)WS_FRAME_MAX_TOKENS);
        free(wscd);
        return;
    }

    /* FID — только строка: имя вида "<подсистема>.<действие>". Числовой код
     * отклоняем: протокол переведён на имена целиком, и принимать оба вида
     * значило бы держать два протокола в одном месте. */
    if (!gw_json_str(&json, "FID", wscd->fid, sizeof(wscd->fid))) {
        ws_hal_log_w(TAG, "Api call bad FID property (нужно имя строкой, например \"uart2.modbus\")");
        free(wscd);
        return;
    }

    /* ARG отдаём обработчику как есть — срезом исходного сообщения, без дерева и
     * без печати в строку (у прежнего сборщика JSON это была вторая
     * аллокация на запрос). */
    const char *arg_s = NULL;
    size_t      arg_n = 0;
    if (gw_json_raw(&json, "ARG", &arg_s, &arg_n)) {
        wscd->pucReqData = (uint8_t *)malloc(arg_n + 1);
        if (!wscd->pucReqData) {
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

    /* Токены jsmn указывают внутрь принятого сообщения, и оно живёт до конца
     * этого вызова: буфер принадлежит транспорту и освобождается им после
     * возврата из обработчика события. */

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
    /* Числовой идентификатор соединения, на котором пришёл запрос: обработчики
     * по нему отличают клиентов AP от станционных. У снятого вызова соединения
     * уже нет — тогда -1. */
    msg->fd  = (call->session != NULL) ? call->session->nPeerId : -1;

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

/* Вызовы, которые просят снять (клиент ушёл, пришёл FLAGS с флагом удаления):
 * их надо довести до конца — спросить обработчика, и только после FALSE
 * освобождать память вызова. */
static ApiCall_t *apxFinishBatch[WS_API_WORK_BATCH];

static int vWsApiCollectFinish(LinkedList_t list, int from) {
    int n = from;
    LinkedListItem_t *item = pxLinkedListFindFirst(list, NULL, NULL);
    while(item != NULL && n < WS_API_WORK_BATCH) {
        LinkedListItem_t *next = pxLinkedListFindNextNoOverlap(item, NULL, NULL);
        ApiCall_t *call = LinkedListGetObject(ApiCall_t, item);
        if(call->flags & CALL_FLAG_TO_DELETE)
            apxFinishBatch[n++] = call;
        item = next;
    }
    return n;
}

static void vWsApiFinishCalls(void) {
    ws_hal_mutex_take(xWsApiMutex, WS_HAL_WAIT_FOREVER);
    int n = vWsApiCollectFinish(pxWsApiCall, 0);
    n = vWsApiCollectFinish(pxWsApiWaitingForResponse, n);
    ws_hal_mutex_give(xWsApiMutex);

    for(int i = 0; i < n; i++) {
        ApiCall_t *call = apxFinishBatch[i];
        /* Флаг мог поставить клиент (FLAGS с битом удаления) — тогда он ждёт
         * подтверждения. Если клиента уже нет, отправлять некуда. */
        if(call->session)
            bApiCallSendStatus(call, API_CALL_STATUS_DELIVERED);
        if(!_bCallTeardown(call))
            continue;            /* обработчик ещё не завершился — спросим тактом позже */

        ws_hal_mutex_take(xWsApiMutex, WS_HAL_WAIT_FOREVER);
        LinkedListItem_t *li = LinkedListItem(call);
        if(bLinkedListContains(pxWsApiCall, li)) vLinkedListUnlink(li);
        if(bLinkedListContains(pxWsApiWaitingForResponse, li)) vLinkedListUnlink(li);
        free(call->pucReqData);
        free(call);
        ws_hal_mutex_give(xWsApiMutex);
    }
}

static void vWsApiWorkCycle(void) {
    uint32_t now = ws_hal_tick();
    if((now - ulLastWorkTick) < ws_hal_ms_to_ticks(WS_API_WORK_TICK_MS)) return;
    ulLastWorkTick = now;

    /* Сначала те, кого просят снять: пока обработчик не вернул результат,
     * вызов остаётся жить в списках. */
    vWsApiFinishCalls();

    ws_hal_mutex_take(xWsApiMutex, WS_HAL_WAIT_FOREVER);
    int n = vWsApiCollectHandled(pxWsApiCall, 0);
    n = vWsApiCollectHandled(pxWsApiWaitingForResponse, n);
    ws_hal_mutex_give(xWsApiMutex);

    for(int i = 0; i < n; i++) {
        ApiCall_t *call = apxWorkBatch[i];
        /* data == NULL — такт работы, а не запрос клиента */
        if(call->fHandler(call, &call->pxHandlerContext, NULL, 0)) continue;

        /* Обработчик вернул FALSE — это и есть результат: вызов завершён
         * (контекст он освободил сам), можно снимать. */
        ws_hal_mutex_take(xWsApiMutex, WS_HAL_WAIT_FOREVER);
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

        /* 1. У вызовов без соединения просим завершения (клиент ушёл или ответ
         * уже отправлен): живость соединения видит транспорт, а не ядро. */
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
                /* Вызов просят снять. Ничего не раздаём и не освобождаем здесь:
                 * завершением займётся такт работы, и лишь после того, как
                 * обработчик вернёт FALSE (см. _bCallTeardown). */
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
                    /* false от обработчика — вызов завершён (его результат) */
                    if(call->flags & CALL_FLAG_LONG_TERM) {
                        /* Подписка живёт дальше: вызов и так лежит в списке
                         * работы, ядро повторит запрос по таймеру переопроса.
                         * Переставлять его в конец списка нельзя: проход шёл бы
                         * по одному и тому же вызову бесконечно (клиент вправе
                         * подписаться на любой FID, в том числе на обработчик,
                         * который отвечает сразу и возвращает FALSE). */
                        call->ulNextPollTick = ws_hal_tick() + call->ulPollIntervalTicks;
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
    /* Same priority as the peripheral tasks (5) and the web server's default
     * task priority. Used to run one level above (6) "so responses are
     * forwarded immediately", but under sustained CALL_FLAG_LONG_TERM load
     * this task rarely blocks (its queue-receive keeps finding work), and
     * being strictly higher priority than the web server starved it of CPU
     * entirely rather than just delaying it. Matching priority lets the
     * scheduler's time-slicing give the web server (and the peripheral tasks)
     * a fair share even while this task is continuously busy. */
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
    /* Соединения освобождаем после вызовов: вызовы уже сняты вместе со своими
     * ссылками на сессии. */
    while ((item = pxLinkedListFindFirst(pxWsSessions, NULL, NULL)) != NULL) {
        ApiSession_t *s = LinkedListGetObject(ApiSession_t, item);
        vLinkedListUnlink(item);
        free(s);
    }
    xTransport.iface  = NULL;
    xTransport.module = NULL;
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
