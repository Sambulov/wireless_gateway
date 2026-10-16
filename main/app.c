#include "app.h"
#include "driver/gpio.h"

#define LED_D2_GPIO GPIO_NUM_2

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
#if CONFIG_WIFI_AP_ENABLE
    /* The default softAP secret is derived from the MAC, so it has to be
     * discoverable: it is printed here and shown on /config.html. */
    ESP_LOGI(TAG, "SoftAP '%s' password '%s'",
             app_context.ap_cnf.ap.ssid, app_context.ap_cnf.ap.password);
#endif

    gpio_reset_pin(LED_D2_GPIO);
    gpio_set_direction(LED_D2_GPIO, GPIO_MODE_OUTPUT);

    /* system HW */
    if(gw_uart_init(&(app_context.uart.port[0].desc) , GW_UART_PORT_0, 2048))
        ESP_LOGI("app", "Uart0 ok");
    if(gw_uart_init(&(app_context.uart.port[1].desc), GW_UART_PORT_2, 2048))
        ESP_LOGI("app", "Uart2 ok");

    setup_littlefs();

#ifdef CONFIG_QEMU_BUILD
    eth_init_openeth();
#else
    wifi_init_ap_sta(&app_context.ap_cnf, &app_context.sta_cnf,
                     app_context.mac_sta, app_context.mac_ap, app_context.sta_bssid);
#endif

    /* Static addresses (when configured) must be in place before the servers
     * start answering on them; DHCP/esp_netif defaults stay untouched. */
    net_apply_config(&app_context);

    // /* Start the server for the first time */
    app_context.web_server = start_webserver();
    app_context.ws_server  = start_ws_server();

    ESP_LOGI(TAG, "Registering URI handlers");
    ws_server_init();
    webserver_register_handler(app_context.ws_server, ws_transport_httpd_init("/ws"));
    webserver_register_handler(app_context.web_server, &ota_update);
    /* Короткий адрес интерпретатора и вшитая страница — до catch-all file_server */
    webserver_register_handler(app_context.web_server, &modbus_alias);

    /* The built-in configuration page must be registered before the
     * catch-all file_server */
    webserver_register_handler(app_context.web_server, &config_page);
    webserver_register_handler(app_context.web_server, &config_page_html);
    webserver_register_handler(app_context.web_server, &file_server);

    ESP_LOGI(TAG, "Run");

    ret = ws_uart_run(&app_context);
    if (ret != ESP_OK)
        ESP_LOGE("app", "can't run ws_uart task: %s", esp_err_to_name(ret));

    if (ws_modbus_run(&app_context) != ESP_OK)
        ESP_LOGI("app", "can't run ws_modbus tasks");

    if (api_sys_run(&app_context) != ESP_OK)
        ESP_LOGE("app", "can't run system api task");

    uint32_t led_tick = 0;
    uint8_t led_state = 0;
    while (1) {
        if (++led_tick >= CONFIG_FREERTOS_HZ) {
            led_tick = 0;
            led_state ^= 1;
            gpio_set_level(LED_D2_GPIO, led_state);
        }
        /* give other tasks to work, also idle task to reset wdt */
        task_delay(1);
    }
}
