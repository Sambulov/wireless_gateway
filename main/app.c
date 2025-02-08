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
//#include "ws_api.h"
#include "esp_littlefs.h"
#include <tftp_server_wg.h>

#include <esp_http_server.h>
#include <stdio.h>

#include "app.h"

static const char *TAG = "app";

void app_main(void)
{
    static httpd_handle_t server = NULL;
    esp_err_t ret = ESP_FAIL;
    //NOTE: just a test to check component build system. Delete it as soon as possible
    //ws_api_inc_test();

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    tftp_example_init_server();

    ret = setup_littlefs();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize LittleFS (%s)", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Initialize LittleFS success!!!(%s)", esp_err_to_name(ret));

	    // ESP_LOGI(TAG, "Opening file");
	    // FILE *f = fopen("/littlefs/hello.txt", "w");
	    // if (f == NULL) {
	    //     ESP_LOGE(TAG, "Failed to open file for writing");
	    //     return;
	    // }
	    // fprintf(f, "Hello World!\n");
	    // fclose(f);
	    // ESP_LOGI(TAG, "File written");

        ESP_LOGI(TAG, "Test dir exists");
        if(directory_exists("/littlefs/")) {
          ESP_LOGI(TAG, "Dir /littlefs/ exists");
        }
        else {
          ESP_LOGI(TAG, "Dir /littlefs/ not found");
        }


	    size_t total = 0, used = 0;
	    ret = esp_littlefs_info(conf.partition_label, &total, &used);
	    if (ret != ESP_OK) {
	        ESP_LOGE(TAG, "Failed to get LittleFS partition information (%s)", esp_err_to_name(ret));
	        esp_littlefs_format(conf.partition_label);
	    } else {
	        ESP_LOGI(TAG, "Partition size: total: %d, used: %d", total, used);
	    }
    }

    /* This helper function configures Wi-Fi or Ethernet, as selected in menuconfig.
     * Read "Establishing Wi-Fi or Ethernet Connection" section in
     * examples/protocols/README.md for more information about this function.
     */
    ESP_ERROR_CHECK(example_connect());

    /* Register event handlers to stop the server when Wi-Fi or Ethernet is disconnected,
     * and re-start it upon connection.
     */
#ifdef CONFIG_EXAMPLE_CONNECT_WIFI
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &connect_handler, &server));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &disconnect_handler, &server));
#endif // CONFIG_EXAMPLE_CONNECT_WIFI
#ifdef CONFIG_EXAMPLE_CONNECT_ETHERNET
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &connect_handler, &server));
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ETHERNET_EVENT_DISCONNECTED, &disconnect_handler, &server));
#endif // CONFIG_EXAMPLE_CONNECT_ETHERNET

    // /* Start the server for the first time */
    server = start_webserver();
}
