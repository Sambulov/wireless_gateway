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
#include "ws_api.h"
#include "esp_littlefs.h"
#include <tftp_server_wg.h>

#include <esp_http_server.h>
#include <stdio.h>

static const char *TAG = "file_system_helpers";



/* Littlefs */
esp_vfs_littlefs_conf_t conf = {
        .base_path = "/lfs",
        .partition_label = "storage",
        .format_if_mount_failed = true,
        .dont_mount = false,
};

bool directory_exists(const char *path) {
    struct stat st;
    if(stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode);
    }
    return false;
}

esp_err_t setup_littlefs(void)
{
    // Use settings defined above to initialize and mount LittleFS filesystem.
    // Note: esp_vfs_littlefs_register is an all-in-one convenience function.
    esp_err_t ret = esp_vfs_littlefs_register(&conf);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize LittleFS (%s)", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Initialize LittleFS success!!!(%s)", esp_err_to_name(ret));
	    size_t total = 0, used = 0;
	    ret = esp_littlefs_info(conf.partition_label, &total, &used);
	    if (ret != ESP_OK) {
	        ESP_LOGE(TAG, "Failed to get LittleFS partition information (%s)", esp_err_to_name(ret));
	        esp_littlefs_format(conf.partition_label);
	    } else {
	        ESP_LOGI(TAG, "Partition size: total: %d, used: %d", total, used);
	    }
    }

    return ret;
}
