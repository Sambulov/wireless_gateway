#pragma once

#include <esp_wifi.h>

#ifdef __cplusplus
extern "C" {
#endif

void wifi_init_ap_sta(wifi_config_t *ap_cnf, wifi_config_t *sta_cnf);

#ifdef __cplusplus
}
#endif
