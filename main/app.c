#include "app.h"
#include "onewire_bus.h"
#include "ds18b20.h"

static void vDallasSensorTask( void * pvParameters ) {
    #define EXAMPLE_ONEWIRE_BUS_GPIO    0
    #define EXAMPLE_ONEWIRE_MAX_DS18B20 2

    /**
     * NOTE: This code adopted that is why it can be a bit
     */
    onewire_bus_handle_t bus = {NULL};
    ds18b20_config_t ds_cfg = {};

    int ds18b20_device_num = 1;
    ds18b20_device_handle_t ds18b20s[EXAMPLE_ONEWIRE_MAX_DS18B20];

    if (ds18b20_new_device_from_bus(bus, &ds_cfg, &ds18b20s[0]) == ESP_OK) {
        ESP_LOGI(TAG, "DS18B20 device created");
    } else {
        ESP_LOGI(TAG, "No DS18B20 device found on the bus");
        vTaskDelete(NULL);
        return;
    }

    for (int i = 0; i < ds18b20_device_num; i++) {
        if (ds18b20_set_resolution(ds18b20s[i], DS18B20_RESOLUTION_12B) != ESP_OK) {
            ESP_LOGI(TAG, "Failed to set resolution for DS18B20[%d]", i);
        }
    }

    float temperature;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(200));

        for (int i = 0; i < ds18b20_device_num; i ++) {
            if (ds18b20_trigger_temperature_conversion(ds18b20s[i]) != ESP_OK) {
                ESP_LOGI(TAG, "Failed to trigger temperature conversion for DS18B20[%d]", i);
                continue;
            }
            if (ds18b20_get_temperature(ds18b20s[i], &temperature) != ESP_OK) {
                ESP_LOGI(TAG, "Failed to get temperature from DS18B20[%d]", i);
                continue;
            }
            ESP_LOGI(TAG, "temperature read from DS18B20[%d]: %.2fC", i, temperature);
        }
    }
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
        xTaskCreatePinnedToCore(vDallasSensorTask, "DallasSensorTask", 4096, NULL, 5, NULL, tskNO_AFFINITY);
        api_handler_system_work(&app_context);
        api_handler_uart_work(&app_context);
        //api_handler_modbus_work(&app_context);
        /* give other tasks to work, also idle task to reset wdt */
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
