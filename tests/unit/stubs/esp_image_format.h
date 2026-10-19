#pragma once
/* Shadow of the ESP-IDF esp_image_format.h — host unit tests only.
 *
 * Службе прошивки нужен только размер заголовка образа: им ограничен снизу
 * допустимый размер файла (меньше заголовка — заведомо не образ). Настоящий
 * заголовок тянет за собой esp_app_format.h и esp_flash_partitions.h, которых
 * на хосте нет. */

#include <stdint.h>

/* На esp32 esp_image_header_t — 24 байта: байтовые поля и битовые наборы,
 * упакованные в слова. Для проверки размера важна только величина. */
typedef struct {
    uint8_t data[24];
} esp_image_header_t;
