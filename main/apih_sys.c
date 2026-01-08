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

// uint8_t bApiHandlerCont(void *pxApiCall, void **ppxContext, uint32_t ulPending, uint8_t *pucData, uint32_t ulDataLen) {
//     if(!ulPending) 
//         return 1;
//     if(pucData == NULL) pucData = (uint8_t *)json_null;
//     uint32_t ulCallId;
//     bApiCallGetId(pxApiCall, &ulCallId);
//     if(ulPending == 1) {
//         ESP_LOGI(TAG, "WS first cont handler call %lu, with arg:%s", ulCallId, pucData);
//         bApiCallSendStatus(pxApiCall, API_CALL_STATUS_EXECUTING);
//     }
//     else {
//         bApiCallSendStatus(pxApiCall, API_CALL_STATUS_CANCELED);
//         vApiCallComplete(pxApiCall);
//         ESP_LOGI(TAG, "WS cont handler canceled call %lu, with arg:%s", ulCallId, pucData);
//         return 1;
//     }
//     return 0;
// }

// static void vAsyncPublisher( void * pvParameters ) {
//     uint32_t counter = 0;
//     while (1) {
//         uint8_t buf[64];
//         uint32_t len = sprintf((char *)buf, "{\"data\":\"Async test\",\"cnt\":\"0x%08lx\"}", counter);
//         buf[len] = '\0';
//         bApiCallSendJsonFidGroup(ESP_WS_API_ASYNC_ID, buf, len);
//         vTaskDelay(1000);
//         counter++;
//     }
//     vTaskDelete(NULL);
// }

// uint8_t bApiHandlerSubs(void *pxApiCall, void **ppxContext, uint32_t ulPending, uint8_t *pucData, uint32_t ulDataLen) {
//     if(!ulPending) 
//         return 1;
//     uint32_t ulCallId; 
//     bApiCallGetId(pxApiCall, &ulCallId);
//     if(pucData == NULL) pucData = (uint8_t *)json_null;
//     if(ulPending == 1) {
//         ESP_LOGI(TAG, "WS subscribtion handler call %lu, with arg:%s", ulCallId, pucData);
//         bApiCallSendStatus(pxApiCall, API_CALL_STATUS_EXECUTING);
//     }
//     else {
//         bApiCallSendStatus(pxApiCall, API_CALL_STATUS_CANCELED);
//         vApiCallComplete(pxApiCall);
//         ESP_LOGI(TAG, "WS subscribtion handler canceled call %lu, with arg:%s", ulCallId, pucData);
//         return 1;
//     }
//     return 0;
// }

uint8_t api_handler_sta(void *call, void **context, uint32_t pending, uint8_t *arg, uint32_t arg_len) {
    if(!pending) return 1;
    uint32_t status = API_CALL_STATUS_COMPLETE;
    app_context_t *app = (app_context_t *)*context;
    if(arg) {
        ESP_LOGI(TAG, "WS STA handler call with arg: %s", arg);
        status = API_CALL_ERROR_STATUS_NO_MEM;
        cJSON *json = json_parse_with_length_opts((char *)arg, arg_len, 0, 0);
        if(json) {
            status = API_CALL_ERROR_STATUS_BAD_ARG;
            char *ssid, *pass;
            if(json_string_value(json, "SSID", &ssid) &&
               json_string_value(json, "PASS", &pass)) {
                strn_cpy((char *)app->sta_cnf.sta.ssid, sizeof(app->sta_cnf.sta.ssid), ssid);
                strn_cpy((char *)app->sta_cnf.sta.password, sizeof(app->sta_cnf.sta.password), pass);
                save_wifi_sta_config((char *)app->sta_cnf.sta.ssid, (char *)app->sta_cnf.sta.password);
                esp_wifi_set_config(ESP_IF_WIFI_STA, &app->sta_cnf);
                esp_wifi_connect();
                status = API_CALL_STATUS_COMPLETE;
            }
        }
    }
    else {
        uint8_t tmpbuf[128];
        uint32_t len = sprintf((char *)tmpbuf, "{\"SSID\":\"%s\",\"PASS\":\"%s\"}", 
                    app->sta_cnf.sta.ssid, app->sta_cnf.sta.password);
        api_call_send_json(call, tmpbuf, len);
    }
    api_call_send_status(call, status);
    return 1;
}

void api_handler_system_work(app_context_t *app) {
    static uint8_t init = 0;
    if(!init) {
        //xTaskCreate(vAsyncPublisher, "ApiAyncWork", 4096, NULL, uxTaskPriorityGet(NULL), NULL);
        bApiCallRegister(&bApiHandlerEcho, ESP_WS_API_ECHO_ID, NULL);
        //bApiCallRegister(&bApiHandlerCont, ESP_WS_API_CONT_ID, NULL);
        //bApiCallRegister(&bApiHandlerSubs, ESP_WS_API_ASYNC_ID, NULL);
        bApiCallRegister(&api_handler_sta, ESP_WS_API_SYS_WIFI_STA, app);
        
        init = 1;
    }
//     //TODO: add system heartbit api
//     uint32_t heap = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
}
