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

static const char *TAG = "app";

uint8_t bWsApiHandlerEcho(uint32_t ulCallId, void **ppxInOutCallContext, char *pucData, uint32_t ulLen) {
    ESP_LOGI(TAG, "WS echo handler call %lu, with arg:%s", ulCallId, pucData);
    return 1;
}

uint8_t bWsApiHandlerContinues(uint32_t ulCallId, void **ppxInOutCallContext, char *pucData, uint32_t ulLen) {
    uint32_t *tmp = (uint32_t *)ppxInOutCallContext;
    if(*tmp == 0) {
        ESP_LOGI(TAG, "WS first cont handler call %lu, with arg:%s", ulCallId, pucData);
        bWebApiResponseStatus(ulCallId, WEB_API_STATUS_OK);
        *tmp = 1;
    }
    else {
        bWebApiResponseStatus(ulCallId, WEB_API_STATUS_BUSY);
        ESP_LOGI(TAG, "WS second cont handler call %lu, with arg:%s", ulCallId, pucData);
    }
    return 0;
}

#define ESP_EVENT_WS_API_ECHO_ID    1000
WsApiHandler_t ws_api_call_echo = {.pfHandler = bWsApiHandlerEcho, .ulFunctionId = ESP_EVENT_WS_API_ECHO_ID};
#define ESP_EVENT_WS_API_CONT_ID    1001
WsApiHandler_t ws_api_call_cont = {.pfHandler = bWsApiHandlerContinues, .ulFunctionId = ESP_EVENT_WS_API_CONT_ID};


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

    
    vWebApiRegisterHandler(&ws_api_call_echo);
    vWebApiRegisterHandler(&ws_api_call_cont);

}
