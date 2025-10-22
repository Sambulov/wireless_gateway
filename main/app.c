#include "app.h"

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

    /* system HW */
    if(gw_uart_init(&(app_context.uart.port[0].desc) , GW_UART_PORT_0, 2048))
        ESP_LOGI("app", "Uart0 ok");
    if(gw_uart_init(&(app_context.uart.port[1].desc), GW_UART_PORT_2, 2048))
        ESP_LOGI("app", "Uart2 ok");
    app_context.uart.port[0].mode = UART_PORT_MODE_RAW;
    app_context.uart.port[1].mode = UART_PORT_MODE_RAW;

    setup_littlefs();

    wifi_init_ap_sta(&app_context.ap_cnf, &app_context.sta_cnf);

    tftp_example_init_server();

    // /* Start the server for the first time */
    app_context.web_server = start_webserver();

    ESP_LOGI(TAG, "Registering URI handlers");
    webserver_register_handler(app_context.web_server, pxWsServerInit("/ws"));
    webserver_register_handler(app_context.web_server, &dir_list);
    webserver_register_handler(app_context.web_server, &file_upload);
    webserver_register_handler(app_context.web_server, &file_delete);
    webserver_register_handler(app_context.web_server, &file_server);

    ESP_LOGI(TAG, "Run");

    while (1) {
        api_handler_system_work(&app_context);
        api_handler_uart_work(&app_context);
        //api_handler_modbus_work(&app_context);
        /* give other tasks to work, also idle task to reset wdt */
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
