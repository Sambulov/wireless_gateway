#pragma once

/* Заглушка OTA и раздела обновления для хостовых тестов (stubs/esp_ota_stub.c):
 * вместо flash образ складывается в память, а все отказы можно вызвать по
 * одному. Так проверяется разбор ARG и порядок порций службы sys.fw.*
 * (main/apih_fw.c) — то, что на приборе видно только по журналу. */

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Размер раздела обновления по умолчанию: как ota_1 у прибора (958464). */
#define ESP_OTA_STUB_PART_SIZE 958464u
/* Сколько байт образа стаб согласен запомнить. */
#define ESP_OTA_STUB_DATA_MAX  (1024u * 1024u)

void esp_ota_stub_reset(void);

/* Размер следующего OTA-раздела; 0 — раздела нет (get_next_update вернёт NULL). */
void esp_ota_stub_set_partition_size(uint32_t size);
/* Отказ, который вернёт соответствующая операция (0 — успех). */
void esp_ota_stub_set_begin_error(int err);
void esp_ota_stub_set_write_error(int err);
void esp_ota_stub_set_end_error(int err);
void esp_ota_stub_set_boot_error(int err);

int      esp_ota_stub_begin_calls(void);
int      esp_ota_stub_write_calls(void);
int      esp_ota_stub_end_calls(void);
int      esp_ota_stub_abort_calls(void);
int      esp_ota_stub_set_boot_calls(void);
/* Размер, с которым позвали esp_ota_begin (0 — begin не звали). */
size_t   esp_ota_stub_begin_size(void);
/* Сколько байт дошло до esp_ota_write и что именно (для сверки содержимого). */
uint32_t esp_ota_stub_bytes(void);
const uint8_t *esp_ota_stub_data(void);
/* 1 — OTA-сессия открыта (begin был, end/abort ещё нет). */
int      esp_ota_stub_active(void);

#ifdef __cplusplus
}
#endif
