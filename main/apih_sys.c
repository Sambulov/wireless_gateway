#include "app.h"
#include "uart.h"
#include "cJSON.h"
#include "cJSON_helpers.h"
#include "web_api.h"

static const uint8_t json_null[] = "\"null\"";

uint8_t bApiHandlerEcho(void *pxApiCall, void **ppxContext, uint32_t ulPending, uint8_t *pucData, uint32_t ulDataLen) {
    if(!ulPending) 
        return 1;
    if(pucData == NULL) {
        pucData = (uint8_t *)json_null;
        ulDataLen = sizeof(json_null) - 1 /* length of "Null" except terminator */;
    }
    ESP_LOGI(TAG, "WS echo handler call with arg: %s", pucData);
    bApiCallSendJson(pxApiCall, pucData, ulDataLen);
    //int fd;
    //if(bApiCallGetSockFd(pxApiCall, &fd)) {
        //esp_netif_t *nif = pxGetNetIfFromSocket(fd);
        //if(nif != NULL) {
        //    ESP_LOGI(TAG, "Echo api call from: %s", esp_netif_get_ifkey(nif));
        //}
    //}
    return 1;
}

uint8_t bApiHandlerCont(void *pxApiCall, void **ppxContext, uint32_t ulPending, uint8_t *pucData, uint32_t ulDataLen) {
    if(!ulPending) 
        return 1;
    if(pucData == NULL) pucData = (uint8_t *)json_null;
    uint32_t ulCallId;
    bApiCallGetId(pxApiCall, &ulCallId);
    if(ulPending == 1) {
        ESP_LOGI(TAG, "WS first cont handler call %lu, with arg:%s", ulCallId, pucData);
        bApiCallSendStatus(pxApiCall, API_CALL_STATUS_EXECUTING);
    }
    else {
        bApiCallSendStatus(pxApiCall, API_CALL_STATUS_CANCELED);
        vApiCallComplete(pxApiCall);
        ESP_LOGI(TAG, "WS cont handler canceled call %lu, with arg:%s", ulCallId, pucData);
        return 1;
    }
    return 0;
}

static void vAsyncPublisher( void * pvParameters ) {
    uint32_t counter = 0;
    while (1) {
        uint8_t buf[64];
        uint32_t len = sprintf((char *)buf, "{\"data\":\"Async test\",\"cnt\":\"0x%08lx\"}", counter);
        buf[len] = '\0';
        bApiCallSendJsonFidGroup(ESP_WS_API_ASYNC_ID, buf, len);
        vTaskDelay(1000);
        counter++;
    }
    vTaskDelete(NULL);
}

uint8_t bApiHandlerSubs(void *pxApiCall, void **ppxContext, uint32_t ulPending, uint8_t *pucData, uint32_t ulDataLen) {
    if(!ulPending) 
        return 1;
    uint32_t ulCallId; 
    bApiCallGetId(pxApiCall, &ulCallId);
    if(pucData == NULL) pucData = (uint8_t *)json_null;
    if(ulPending == 1) {
        ESP_LOGI(TAG, "WS subscribtion handler call %lu, with arg:%s", ulCallId, pucData);
        bApiCallSendStatus(pxApiCall, API_CALL_STATUS_EXECUTING);
    }
    else {
        bApiCallSendStatus(pxApiCall, API_CALL_STATUS_CANCELED);
        vApiCallComplete(pxApiCall);
        ESP_LOGI(TAG, "WS subscribtion handler canceled call %lu, with arg:%s", ulCallId, pucData);
        return 1;
    }
    return 0;
}

void api_handler_system_work(app_context_t *app) {
    static uint8_t init = 0;
    if(!init) {
        xTaskCreate(vAsyncPublisher, "ApiAyncWork", 4096, NULL, uxTaskPriorityGet(NULL), NULL);
        bApiCallRegister(&bApiHandlerEcho, ESP_WS_API_ECHO_ID, NULL);
        bApiCallRegister(&bApiHandlerCont, ESP_WS_API_CONT_ID, NULL);
        bApiCallRegister(&bApiHandlerSubs, ESP_WS_API_ASYNC_ID, NULL);
        init = 1;
    }
//     //TODO: add system heartbit api
//     uint32_t heap = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
}
