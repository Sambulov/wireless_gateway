#pragma once
/* Shadow of the ESP-IDF esp_mac.h (и той части esp_wifi.h, которую тянут службы)
 * — host unit tests only.
 *
 * apih_sys_common.c печатает MAC (MACSTR/MAC2STR) и спрашивает текущий адрес у
 * драйвера (esp_wifi_get_mac), а apih_sys_internal.h называет тип интерфейса
 * (wifi_interface_t). Настоящий esp_wifi.h на хосте не нужен: из него взят
 * только этот тип. Сама esp_wifi_get_mac в тестах отвечает отказом — службы,
 * которые её зовут (sys.info), в хостовой сборке не собираются. */

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WIFI_IF_STA = 0,
    WIFI_IF_AP  = 1,
} wifi_interface_t;

esp_err_t esp_wifi_get_mac(wifi_interface_t iface, uint8_t *mac);

#define MACSTR "%02x:%02x:%02x:%02x:%02x:%02x"
#define MAC2STR(a) (a)[0], (a)[1], (a)[2], (a)[3], (a)[4], (a)[5]

#ifdef __cplusplus
}
#endif
