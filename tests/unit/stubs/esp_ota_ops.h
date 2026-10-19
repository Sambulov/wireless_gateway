#pragma once
/* Shadow of the ESP-IDF esp_ota_ops.h — host unit tests only.
 *
 * Службе прошивки (main/apih_fw.c) от OTA нужны: дескриптор сессии, раздел (его
 * метка и размер), четыре операции приёма образа и выбор загрузочного раздела.
 * Всё остальное настоящего заголовка (esp_partition.h, esp_app_desc.h,
 * soc_caps.h) на хосте ни к чему: тест подменяет сам OTA-раздел и пишет вместо
 * flash в память (stubs/esp_ota_stub.c). */

#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OTA_SIZE_UNKNOWN 0xffffffffu

/* Значения те же, что у IDF: на них смотрит обработчик, различая «не образ»
 * (ошибка клиента) и отказ записи (ошибка прибора). */
#define ESP_ERR_OTA_BASE                         0x1500
#define ESP_ERR_OTA_PARTITION_CONFLICT           (ESP_ERR_OTA_BASE + 0x01)
#define ESP_ERR_OTA_SELECT_INFO_INVALID          (ESP_ERR_OTA_BASE + 0x02)
#define ESP_ERR_OTA_VALIDATE_FAILED              (ESP_ERR_OTA_BASE + 0x03)

typedef uint32_t esp_ota_handle_t;

/* Раздел: из esp_partition_t здесь только то, что читает служба прошивки. */
typedef struct {
    uint32_t    address;
    uint32_t    size;
    const char *label;
} esp_partition_t;

esp_err_t esp_ota_begin(const esp_partition_t *partition, size_t image_size,
                        esp_ota_handle_t *out_handle);
esp_err_t esp_ota_write(esp_ota_handle_t handle, const void *data, size_t size);
esp_err_t esp_ota_end(esp_ota_handle_t handle);
esp_err_t esp_ota_abort(esp_ota_handle_t handle);
esp_err_t esp_ota_set_boot_partition(const esp_partition_t *partition);
const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *start_from);
const esp_partition_t *esp_ota_get_running_partition(void);

#ifdef __cplusplus
}
#endif
