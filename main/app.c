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
#include "esp_littlefs.h"
#include <tftp_server_wg.h>

#include <esp_http_server.h>
#include <stdio.h>

#include "web_api.h"

#include "app.h"

#include "connection.h"

#include "CodeLib.h"

static const char *TAG = "app";

#define ESP_EVENT_WS_API_ECHO_ID    1000
uint8_t bApiHandlerEcho(void *pxApiCall, void **ppxContext, ApiCallReason_t eReason, uint8_t *pucData, uint32_t ulDataLen) {
    ESP_LOGI(TAG, "WS echo handler call with arg:%s", pucData);
    bApiCallSendJson(pxApiCall, pucData, ulDataLen);
    int fd;
    if(bApiCallGetSockFd(pxApiCall, &fd)) {
        esp_netif_t *nif = pxGetNetIfFromSocket(fd);
        if(nif != NULL) {
            ESP_LOGI(TAG, "Echo api call from: %s", esp_netif_get_ifkey(nif));
        }
    }
    return 1;
}

#define ESP_EVENT_WS_API_CONT_ID    1001
uint8_t bApiHandlerCont(void *pxApiCall, void **ppxContext, ApiCallReason_t eReason, uint8_t *pucData, uint32_t ulDataLen) {
    uint32_t ulCallId; 
    bApiCallGetId(pxApiCall, &ulCallId);
    uint32_t *tmp = (uint32_t *)ppxContext;
    if(*tmp == 0) {
        ESP_LOGI(TAG, "WS first cont handler call %lu, with arg:%s", ulCallId, pucData);
        bApiCallSendStatus(pxApiCall, API_CALL_STATUS_OK);
        *tmp = 1;
    }
    else {
        bApiCallSendStatus(pxApiCall, API_CALL_STATUS_CANCELED);
        ESP_LOGI(TAG, "WS cont handler canceled call %lu, with arg:%s", ulCallId, pucData);
        return 1;
    }
    return 0;
}

#define ESP_EVENT_WS_API_ASYNC_ID    1002

static void vAsyncTestWorker( void * pvParameters ) {
    const char data[] = "{\"data\":\"Async test\"}";
    while (1) {
        bApiCallSendJsonFidGroup(ESP_EVENT_WS_API_ASYNC_ID, (uint8_t *)data, sizeof(data));
        vTaskDelay(100);
    }
    vTaskDelete(NULL);
}

uint8_t bApiHandlerSubs(void *pxApiCall, void **ppxContext, ApiCallReason_t eReason, uint8_t *pucData, uint32_t ulDataLen) {
    uint32_t ulCallId; 
    bApiCallGetId(pxApiCall, &ulCallId);
    uint32_t *tmp = (uint32_t *)ppxContext;
    if(pucData == NULL) pucData = (uint8_t *)"Null";
    if(*tmp == 0) {
        ESP_LOGI(TAG, "WS subscribtion handler call %lu, with arg:%s", ulCallId, pucData);
        bApiCallSendStatus(pxApiCall, API_CALL_STATUS_OK);
        *tmp = 1;
    }
    else {
        bApiCallSendStatus(pxApiCall, API_CALL_STATUS_CANCELED);
        ESP_LOGI(TAG, "WS subscribtion handler canceled call %lu, with arg:%s", ulCallId, pucData);
        return 1;
    }
    return 0;
}


void app_main(void)
{
    static app_context_t app_context;

    ESP_LOGI(TAG, "App start");

    //NOTE: just a test to check component build system. Delete it as soon as possible
    //ws_api_inc_test();

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    load_config(&app_context);

    setup_littlefs();

    tftp_example_init_server();

    /* This helper function configures Wi-Fi or Ethernet, as selected in menuconfig.
     * Read "Establishing Wi-Fi or Ethernet Connection" section in
     * examples/protocols/README.md for more information about this function.
     */
    //ESP_ERROR_CHECK(example_connect());
    wifi_init_ap_sta(&app_context.ap_cnf, &app_context.sta_cnf);

    // /* Start the server for the first time */
    app_context.web_server = start_webserver();
    ws_init(&app_context);

    bApiCallRegister(bApiHandlerEcho, ESP_EVENT_WS_API_ECHO_ID);
    bApiCallRegister(bApiHandlerCont, ESP_EVENT_WS_API_CONT_ID);
    bApiCallRegister(bApiHandlerSubs, ESP_EVENT_WS_API_ASYNC_ID);

    xTaskCreate(vAsyncTestWorker, "ApiAyncWork", 4096, NULL, uxTaskPriorityGet(NULL), NULL);
}
