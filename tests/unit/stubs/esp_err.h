#pragma once
/* Shadow of the ESP-IDF esp_err.h — host unit tests only.
 *
 * Ядро API поднимается через api_init(), а она возвращает esp_err_t: это тип
 * ESP-IDF, и на хосте его нет. Значения те же, что у IDF (ноль — успех), поэтому
 * проверки тестов читаются одинаково с прошивкой. */

typedef int esp_err_t;

#define ESP_OK                  0
#define ESP_FAIL                (-1)

#define ESP_ERR_NO_MEM          0x101
#define ESP_ERR_INVALID_ARG     0x102
#define ESP_ERR_INVALID_STATE   0x103
#define ESP_ERR_INVALID_SIZE    0x104
#define ESP_ERR_NOT_FOUND       0x105
#define ESP_ERR_NOT_SUPPORTED   0x106
#define ESP_ERR_TIMEOUT         0x107

#ifdef __cplusplus
extern "C" {
#endif

/* Обработчик службы прошивки кладёт имя ошибки в сообщение клиенту; на хосте
 * имени нет — важен только текст отказа (stubs/system_stub.c). */
const char *esp_err_to_name(esp_err_t code);

#ifdef __cplusplus
}
#endif
