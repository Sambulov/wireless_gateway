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

static const uint8_t json_null[] = "\"null\"";

#define ESP_WS_API_ECHO_ID    1000
uint8_t bApiHandlerEcho(void *pxApiCall, void **ppxContext, uint32_t ulPending, uint8_t *pucData, uint32_t ulDataLen) {
    if(!ulPending) 
        return 1;
    if(pucData == NULL) {
        pucData = (uint8_t *)json_null;
        ulDataLen = sizeof(json_null) - 1 /* length of "Null" except terminator */;
    }
    ESP_LOGI(TAG, "WS echo handler call with arg: %s", pucData);
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

#define ESP_WS_API_CONT_ID    1001
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

#define ESP_WS_API_ASYNC_ID    1002

static void vAsyncTestWorker( void * pvParameters ) {
    const char data[] = "{\"data\":\"Async test\"}";
    while (1) {
        bApiCallSendJsonFidGroup(ESP_WS_API_ASYNC_ID, (uint8_t *)data, sizeof(data));
        vTaskDelay(1000);
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






#include "cJSON.h"

uint8_t cJSON_ParseInt(cJSON *pxJson, const char *pcName, uint32_t *pulOutVal) {
    uint32_t val = 0;
    if(pcName != NULL) pxJson = cJSON_GetObjectItem(pxJson, pcName);
    if (cJSON_IsNumber(pxJson)) val = pxJson->valueint;
    else if (cJSON_IsString(pxJson)) val = (uint32_t)strtol(pxJson->valuestring, NULL, 16);
    else return 0;
    *pulOutVal = val;
    return 1;
}

typedef struct {
    Modbus_t *pxModbus;
    QueueHandle_t xCmdQueue;
} ApiContextModbus_t;

typedef struct {
    __LinkedListObject__
    uint32_t ulTaskID;
    void *pxApiCall;
    uint32_t ulTimestamp;
    uint32_t ulRepeatDelay;
    uint32_t ulAwaiteTimeout;
    uint32_t ulTransferError;
    ModbusFrame_t *xMbFrame;
} ApiCmdModbus_t;

#define ESP_WS_API_MODBUS_ID    2000

uint8_t bApiHandlerModbus(void *pxApiCall, void **ppxContext, uint32_t ulPending, uint8_t *pucData, uint32_t ulDataLen) {
    if(!ulPending) 
        return 1; /* todo: stop modbus polling */
    static uint32_t taskId = 0;
    if(!ulPending || (pucData == NULL)) return 1;
    ApiContextModbus_t *context = (ApiContextModbus_t *)*ppxContext;
    ESP_LOGI(TAG, "Modbus call, with arg:%s", pucData);
    cJSON *json = cJSON_ParseWithLengthOpts((char *)pucData, ulDataLen, 0, 0);
    uint32_t status = API_CALL_ERROR_STATUS_NO_MEM;
    ApiCmdModbus_t *pxCmd = malloc(sizeof(ApiCmdModbus_t));
    do {
        if(pxCmd == NULL) { break; }
        memset(pxCmd, 0, sizeof(ApiCmdModbus_t));
        pxCmd->pxApiCall = pxApiCall;
        pxCmd->ulTimestamp = xTaskGetTickCount();
        if(!cJSON_ParseInt(json, "TIDC", &pxCmd->ulTaskID)) {
            status = API_CALL_ERROR_STATUS_NO_MEM;
            pxCmd->xMbFrame = malloc(sizeof(ModbusFrame_t));
            if(pxCmd->xMbFrame == NULL) break;
            memset(pxCmd->xMbFrame, 0, sizeof(ModbusFrame_t));
            status = API_CALL_ERROR_STATUS_BAD_ARG;
            uint32_t val;
            pxCmd->ulAwaiteTimeout = 100;
            pxCmd->ulRepeatDelay = 0;
            if(cJSON_ParseInt(json, "AWT", &val)) pxCmd->ulAwaiteTimeout = max(val, pxCmd->ulAwaiteTimeout);
            //ESP_LOGI(TAG, "mb req AWT:%lu", pxCmd->ulAwaiteTimeout);
            if(cJSON_ParseInt(json, "RDL", &val)) pxCmd->ulRepeatDelay = val;
            pxCmd->ulTimestamp -= pxCmd->ulRepeatDelay;
            //ESP_LOGI(TAG, "mb req RDL:%lu", pxCmd->ulRepeatDelay);
            if(!cJSON_ParseInt(json, "FN", &val) || (val > 127)) { break; } pxCmd->xMbFrame->ucFunc = val;
            //ESP_LOGI(TAG, "mb req FN:%u", pxCmd->xMbFrame->ucFunc);
            if(!cJSON_ParseInt(json, "ADR", &val) || (val > 255)) { break; } pxCmd->xMbFrame->ucAddr = val;
            //ESP_LOGI(TAG, "mb req ADR:%u", pxCmd->xMbFrame->ucAddr);
            if(cJSON_ParseInt(json, "CV", &val)) pxCmd->xMbFrame->ucLengthCode = val;
            //ESP_LOGI(TAG, "mb req CV:%u", pxCmd->xMbFrame->ucLengthCode);
            if(cJSON_ParseInt(json, "RA", &val)) pxCmd->xMbFrame->usRegAddr = val;
            //ESP_LOGI(TAG, "mb req RA:%u", pxCmd->xMbFrame->usRegAddr);
            if(cJSON_ParseInt(json, "RVC", &val)) pxCmd->xMbFrame->usRegValueCount = val;
            //ESP_LOGI(TAG, "mb req RV:%u", pxCmd->xMbFrame->usRegValueCount);
            cJSON *jobj = cJSON_GetObjectItem(json, "RD");
            if(jobj != NULL) {
                if (!cJSON_IsArray(jobj)) { break; }
                uint32_t size = cJSON_GetArraySize(jobj);
                if(size > 0) {
                    status = API_CALL_ERROR_STATUS_NO_MEM;
                    int k = 2;
                    if(pxCmd->xMbFrame->ucFunc == MB_FUNC_WRITE_COILS) k = 1;
                    //pxCmd->xMbFrame->usRegValueCount = size;
                    pxCmd->xMbFrame->ucBufferSize = size * k;
                    pxCmd->xMbFrame->pucData = malloc(pxCmd->xMbFrame->ucBufferSize);
                    if(pxCmd->xMbFrame->pucData == NULL) { break; }
                    cJSON *jvalue = jobj->child;
                    uint8_t *value = pxCmd->xMbFrame->pucData;
                    //ESP_LOGI(TAG, "mb req RD amount:%lu", size);
                    status = API_CALL_ERROR_STATUS_BAD_ARG;
                    for(int i = 0; i < size; i++) {
                        if(!cJSON_ParseInt(jvalue, NULL, &val)) { break; }
                        if(k == 2) *((uint16_t *)value) = val;
                        else *value = val;
                        //ESP_LOGI(TAG, "mb req RD[%d]:%lu", i, val);
                        value += k;
                        jvalue = jvalue->next;
                    }
                }
            }
            pxCmd->ulTaskID = taskId++;
        }
        status = API_CALL_ERROR_STATUS_NO_FREE_DESCRIPTORS;
        if(xQueueSend(context->xCmdQueue, (void *)&pxCmd, pdMS_TO_TICKS(0)) != pdPASS) { 
            ESP_LOGI(TAG, "Mb req drop");
            break; 
        }
        ESP_LOGI(TAG, "Mb req enqueued");
        status = API_CALL_STATUS_EXECUTING;
        break;
    } while (1);
    bApiCallSendStatus(pxApiCall, status);
    if(status != API_CALL_STATUS_EXECUTING) {
        if(pxCmd != NULL) {
            if(pxCmd->xMbFrame != NULL) {
                if(pxCmd->xMbFrame->pucData != NULL) free(pxCmd->xMbFrame->pucData);
                free(pxCmd->xMbFrame);
            }
            free(pxCmd);
        }
    }
    cJSON_Delete(json);
    return 0;
}

#include "uart.h"

typedef struct {
    gw_uart_t *pxUart;
    QueueHandle_t xCmdQueue;
} ApiContextUart_t;

typedef struct {
    __LinkedListObject__
    uint32_t ulTaskID;
    void *pxApiCall;
    uint32_t ulTimestamp;
    uint32_t ulRepeatDelay;
    uint32_t ulAwaiteTimeout;
    uint8_t bWordLenSet : 1,
            bBoudSet    : 1,
            bParitySet  : 1,
            bStopBitsSet: 1;
    uint8_t ucWordLen;
    uint8_t ucParity;
    uint8_t ucStopBits;
    uint32_t ulBoud;
} ApiCmdUart_t;

#define ESP_WS_API_UART_ID    3000

uint8_t bApiHandlerUart(void *pxApiCall, void **ppxContext, uint32_t ulPending, uint8_t *pucData, uint32_t ulDataLen) {
    if(!ulPending) 
        return 1;
    static uint32_t taskId = 0;
    if(!ulPending) return 1;
    if(pucData == NULL) {
        if(ulPending == 1) {
            bApiCallSendStatus(pxApiCall, API_CALL_STATUS_EXECUTING);
            return 0;
        }
        else {
            bApiCallSendStatus(pxApiCall, API_CALL_STATUS_CANCELED);
            vApiCallComplete(pxApiCall);
            return 1;
        }
    }
    ApiContextUart_t *context = (ApiContextUart_t *)*ppxContext;
    ESP_LOGI(TAG, "Uart call, with arg:%s", pucData);
    cJSON *json = cJSON_ParseWithLengthOpts((char *)pucData, ulDataLen, 0, 0);
    uint32_t status = API_CALL_ERROR_STATUS_NO_MEM;
    ApiCmdUart_t *pxCmd = malloc(sizeof(ApiCmdUart_t));
    do {
        if(pxCmd == NULL) { break; }
        memset(pxCmd, 0, sizeof(ApiCmdUart_t));
        pxCmd->pxApiCall = pxApiCall;
        pxCmd->ulTimestamp = xTaskGetTickCount();
        status = API_CALL_ERROR_STATUS_BAD_ARG;
        uint32_t val;
        pxCmd->ulAwaiteTimeout = 0;
        pxCmd->ulRepeatDelay = 0;
        if(cJSON_ParseInt(json, "AWT", &val)) pxCmd->ulAwaiteTimeout = max(val, pxCmd->ulAwaiteTimeout);
        ESP_LOGI(TAG, "mb req AWT:%lu", pxCmd->ulAwaiteTimeout);
        if(cJSON_ParseInt(json, "RDL", &val)) pxCmd->ulRepeatDelay = val;
        pxCmd->ulTimestamp -= pxCmd->ulRepeatDelay;
        ESP_LOGI(TAG, "mb req RDL:%lu", pxCmd->ulRepeatDelay);

        if(cJSON_ParseInt(json, "WL", &val)) {
            if(val >= 2) { break; }
            pxCmd->bWordLenSet = 1;
            pxCmd->ucWordLen = val;
        }
        if(cJSON_ParseInt(json, "BR", &val)) {
            if(val > 1000000) { break; }
            pxCmd->bBoudSet = 1;
            pxCmd->ulBoud = val;
        }
        if(cJSON_ParseInt(json, "PAR", &val)) {
            if(val >= 3) { break; }
            pxCmd->bParitySet = 1;
            pxCmd->ucParity = val;
        }
        if(cJSON_ParseInt(json, "SB", &val)) {
            if((val >= 4) || (val == GW_UART_STOP_BITS0_5)) { break; }
            pxCmd->bStopBitsSet = 1;
            pxCmd->ucStopBits = val;
        }
        pxCmd->ulTaskID = taskId++;
        status = API_CALL_ERROR_STATUS_NO_FREE_DESCRIPTORS;
        if(xQueueSend(context->xCmdQueue, (void *)&pxCmd, pdMS_TO_TICKS(0)) != pdPASS) { 
            ESP_LOGI(TAG, "Uart req dropped");
            break; 
        }
        ESP_LOGI(TAG, "Uart req enqueued");
        status = API_CALL_STATUS_EXECUTING;
        break;
    } while (1);
    bApiCallSendStatus(pxApiCall, status);
    if(status != API_CALL_STATUS_EXECUTING) {
        if(pxCmd != NULL) free(pxCmd);
    }
    cJSON_Delete(json);
    return 0;
}


static inline uint32_t mb_timer(const void *timer) {
    (void)timer;
	return xTaskGetTickCount();
}

const ModbusIface_t mb_iface = {
    .pfRead = &gw_uart_read,
    .pfWrite = &gw_uart_write,
    .pfTimer = &mb_timer
};


static void vModbusCb(modbus_t *mb, void *context, modbus_frame_t *frame) {
    (void)mb;
    ApiCmdModbus_t *task = (ApiCmdModbus_t *)context;
    uint8_t amount, size, code;
    uint8_t *regs = pucModbusResponseFrameData(frame, &code, &amount, &size);
    uint32_t buf_size = sizeof("{\"TID\":\"0x00000000\",\"ADR\":\"0x00\",\"FN\":\"0x00\",\"CV\":\"0x00\",\"RA\":\"0x0000\",\"RC\":\"0x0000\",\"RD\":[]}");
    if(regs != NULL) {
        if(size == 2) buf_size += sizeof("\"0x0000\",") * amount;
        else buf_size += sizeof("\"0x00\",") * amount;
    }
    uint8_t *response = malloc(buf_size);
    if(response == NULL) {
        uint8_t buf[40];
        uint32_t len = sprintf((char *)buf, "{\"STA\":\"0x%08x\",\"TID\":\"0x%08lx\"}", API_CALL_ERROR_STATUS_NO_MEM, task->ulTaskID);
        if(!bApiCallSendJson(task->pxApiCall, buf, len)) 
            task->ulTransferError++;
        else 
            task->ulTransferError = 0;
        return;
    }
    uint32_t offset = sprintf((char *)response, 
        "{\"TID\":\"0x%08lx\",\"ADR\":\"0x%02x\",\"FN\":\"0x%02x\",\"CV\":\"0x%02x\",\"RA\":\"0x%04x\",\"RC\":\"0x%02x\",\"RD\":[", 
        task->ulTaskID, frame->ucAddr, frame->ucFunc, code, frame->usRegAddr, amount);
    if(regs != NULL) {
        for(int i = 0; i < amount; i++) {
            if(size == 1) {
                uint8_t x = regs[i];
                offset += sprintf((char *)(response + offset),"\"0x%02x\",", x);
            }
            else {
                uint16_t x = *((uint16_t *)&regs[i*2]);
                offset += sprintf((char *)(response + offset),"\"0x%04x\",", x);
            }
        }
        offset--;
    }
    response[offset++] = ']';
    response[offset++] = '}';
    response[offset] = '\0';
    if(!bApiCallSendJson(task->pxApiCall, response, offset)) 
        task->ulTransferError++;
    else 
        task->ulTransferError = 0;
    free(response);
}

static uint8_t bCmdExecuteReady(LinkedListItem_t *item, void *arg) {
    (void)arg;
    ApiCmdModbus_t *xCmdTask = LinkedListGetObject(ApiCmdModbus_t, item);
    return (xTaskGetTickCount() - xCmdTask->ulTimestamp) >= xCmdTask->ulRepeatDelay;
}

static uint8_t bCmdApiCallTidMatch(LinkedListItem_t *item, void *arg) {
    uint32_t tid = (uint32_t)((void **)arg)[0];
    void *apiCall = ((void **)arg)[1];
    ApiCmdModbus_t *xCmdTask = LinkedListGetObject(ApiCmdModbus_t, item);
    return (xCmdTask->pxApiCall == apiCall) && (xCmdTask->ulTaskID == tid);
}

// #include "esp_freertos_hooks.h"

// static SemaphoreHandle_t xIdleSemaphore = NULL;

// bool esp_freertos_idle_cb() {
//     xSemaphoreGive(xIdleSemaphore);
//     return true; /* true to continue using hook */
// }

void vCmdModbusFree(ApiCmdModbus_t *pxCmd) {
    if(pxCmd != NULL) {
        vApiCallComplete(pxCmd->pxApiCall);
        if(pxCmd->xMbFrame != NULL) {
            if(pxCmd->xMbFrame->pucData != NULL) free(pxCmd->xMbFrame->pucData);
            free(pxCmd->xMbFrame);
        }
        ESP_LOGI(TAG, "mb task free, %lu", pxCmd->ulTaskID);
        free(pxCmd);
    }
}

void app_main(void)
{
    static app_context_t app_context;

    ESP_LOGI(TAG, "App start");

    //NOTE: just a test to check component build system. Delete it as soon as possible
    //ws_api_inc_test();

    static gw_uart_t mb_uart;
    if(gw_uart_init(&mb_uart, GW_UART_PORT_2, 1024)) {
        ESP_LOGI("app", "Uart ok");
    }
    gw_uart_config_t xUartCnf = gw_uart_config_default;

    static Modbus_t pxMb;
	#define BUF_SIZE  255
    static uint8_t mb_buf[BUF_SIZE];

    ModbusConfig_t mb_config = {
        .bAsciiMode = 0,
        .rx_timeout = 500,
        .tx_timeout = 500,
        .pxIface = &mb_iface,
        .pxTimerContext = NULL,
        .pxRxContext = &mb_uart,
        .pxTxContext = &mb_uart,
        .ucPayLoadBufferSize = BUF_SIZE,
        .pucPayLoadBuffer = mb_buf
    };
	bModbusInit(&pxMb, &mb_config); /* default config */

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


    ESP_LOGI(TAG, "Registering URI handlers");
    webserver_register_handler(app_context.web_server, pxWsServerInit("/ws"));
    webserver_register_handler(app_context.web_server, &dir_list);
    webserver_register_handler(app_context.web_server, &file_upload);
    webserver_register_handler(app_context.web_server, &file_delete);
    webserver_register_handler(app_context.web_server, &file_server);

    

    bApiCallRegister(&bApiHandlerEcho, ESP_WS_API_ECHO_ID, NULL);
    bApiCallRegister(&bApiHandlerCont, ESP_WS_API_CONT_ID, NULL);
    bApiCallRegister(&bApiHandlerSubs, ESP_WS_API_ASYNC_ID, NULL);

    static ApiContextModbus_t mbApiCtxMb;
    mbApiCtxMb.pxModbus = &pxMb;
    mbApiCtxMb.xCmdQueue = xQueueCreate(20, sizeof(void *));
    bApiCallRegister(&bApiHandlerModbus, ESP_WS_API_MODBUS_ID, &mbApiCtxMb);

    static ApiContextUart_t mbApiCtxUart;
    mbApiCtxUart.pxUart = &mb_uart;
    mbApiCtxUart.xCmdQueue = xQueueCreate(5, sizeof(void *));
    bApiCallRegister(&bApiHandlerUart, ESP_WS_API_UART_ID, &mbApiCtxUart);

    xTaskCreate(vAsyncTestWorker, "ApiAyncWork", 4096, NULL, uxTaskPriorityGet(NULL), NULL);

    ESP_LOGI(TAG, "Run");

    ApiCmdModbus_t *pxCmdMb = NULL;
    LinkedList_t pxCmdStackModbus = NULL;
    LinkedListItem_t *pxCmdCurrentModbus = NULL;

    ApiCmdUart_t *pxCmdUart = NULL;
    LinkedList_t pxCmdStackUart = NULL;
    //LinkedListItem_t *pxCmdCurrentUart = NULL;


    //xIdleSemaphore = xSemaphoreCreateBinary();
    //esp_register_freertos_idle_hook(&esp_freertos_idle_cb);

    //uint32_t timer = xTaskGetTickCount();
    //uint32_t cycles_count = 0;
    while (1) {
// #if 1
//     //TODO: add to debug build, remove from release
//     uint32_t heap = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
//     if(heap < 30000)
//         ESP_LOGE(TAG, "Heap left:%lu", heap);
// #endif

        uint8_t rep = 100;
        while (rep--) {
            uint32_t now = xTaskGetTickCount();

            if(xQueueReceive(mbApiCtxMb.xCmdQueue, &pxCmdMb, pdMS_TO_TICKS(0)) == pdPASS ) {
                vLinkedListInsertLast(&pxCmdStackModbus, LinkedListItem(pxCmdMb));
                ESP_LOGI(TAG, "Modbus cmd added, %lu", pxCmdMb->ulTaskID);
            }
            if(xQueueReceive(mbApiCtxUart.xCmdQueue, &pxCmdUart, pdMS_TO_TICKS(0)) == pdPASS ) {
                vLinkedListInsertLast(&pxCmdStackUart, LinkedListItem(pxCmdUart));
                ESP_LOGI(TAG, "Uart cmd added, %lu", pxCmdUart->ulTaskID);
            }
            if(!bModbusBusy(&pxMb)) {
                if(pxCmdStackUart != NULL) {
                    pxCmdUart = LinkedListGetObject(ApiCmdUart_t, pxCmdStackUart);
                    if(pxCmdUart->bBoudSet)xUartCnf.boud = pxCmdUart->ulBoud;
                    if(pxCmdUart->bParitySet)xUartCnf.parity = pxCmdUart->ucParity;
                    if(pxCmdUart->bStopBitsSet)xUartCnf.stop = pxCmdUart->ucStopBits;
                    if(pxCmdUart->bWordLenSet)xUartCnf.bits = pxCmdUart->ucWordLen;
                    gw_uart_set(&mb_uart, &xUartCnf);
                    uint8_t tmpbuf[64];
                    uint32_t len = sprintf((char *)tmpbuf, "{\"BR\":\"0x%08lx\",\"WL\":\"0x%02x\",\"PAR\":\"0x%02x\",\"SB\":\"0x%02x\"}", xUartCnf.boud, xUartCnf.bits, xUartCnf.parity, xUartCnf.stop);
                    ESP_LOGI(TAG, "Uart new config %s", tmpbuf);
                    bApiCallSendJsonFidGroup(ESP_WS_API_UART_ID, tmpbuf, len);
                    vApiCallComplete(pxCmdUart->pxApiCall);
                    vLinkedListUnlink(pxCmdStackUart);
                    free(pxCmdUart);
                }
                if(pxCmdCurrentModbus != NULL) {
                    vLinkedListUnlink(pxCmdCurrentModbus);
                    pxCmdMb = LinkedListGetObject(ApiCmdModbus_t, pxCmdCurrentModbus);
                    if((!pxCmdMb->ulRepeatDelay) || (pxCmdMb->ulTransferError)) 
                        vCmdModbusFree(pxCmdMb);
                    else {
                        pxCmdMb->ulTimestamp += pxCmdMb->ulRepeatDelay;
                        if((now - pxCmdMb->ulTimestamp) > pxCmdMb->ulRepeatDelay) pxCmdMb->ulTimestamp = now - pxCmdMb->ulRepeatDelay;
                        vLinkedListInsertLast(&pxCmdStackModbus, LinkedListItem(pxCmdMb));
                    }
                }
                pxCmdCurrentModbus = pxLinkedListFindFirst(pxCmdStackModbus, &bCmdExecuteReady, NULL);
                if(pxCmdCurrentModbus != NULL) {
                    pxCmdMb = LinkedListGetObject(ApiCmdModbus_t, pxCmdCurrentModbus);
                    if(pxCmdMb->xMbFrame == NULL) { /* cancel request */
                        vLinkedListUnlink(pxCmdCurrentModbus);
                        pxCmdCurrentModbus = NULL;
                        ApiCmdModbus_t *pxCacelingCmd = 
                            LinkedListGetObject(ApiCmdModbus_t, pxLinkedListFindFirst(pxCmdStackModbus, &bCmdApiCallTidMatch, 
                                (void *)((void *[]){ (void *)pxCmdMb->ulTaskID, pxCmdMb->pxApiCall})));
                        if(pxCacelingCmd != NULL) {
                            vLinkedListUnlink(LinkedListItem(pxCacelingCmd));
                            vCmdModbusFree(pxCacelingCmd);
                            bApiCallSendStatus(pxCmdMb->pxApiCall, API_CALL_STATUS_CANCELED);
                        }
                        else bApiCallSendStatus(pxCmdMb->pxApiCall, API_CALL_ERROR_STATUS_BAD_ARG);
                        vCmdModbusFree(pxCmdMb);
                    }
                    else {
                        mb_config.rx_timeout = pxCmdMb->ulAwaiteTimeout;
                        bModbusInit(&pxMb, &mb_config);
                        ulModbusRequest(&pxMb, pxCmdMb->xMbFrame, &vModbusCb, pxCmdMb);
                    }
                }
            }
            vModbusWork(&pxMb);

            // cycles_count++;
            // if((now - timer) >= 1000) {
            //     timer += 1000;
            //     ESP_LOGI(TAG, "Work sycles: %lu/s", cycles_count);
            //     cycles_count = 0;
            // }
        }
        /* give other tasks to work, also idle task to reset wdt */
        //xSemaphoreTake(xIdleSemaphore, pdMS_TO_TICKS(1));
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    

}
