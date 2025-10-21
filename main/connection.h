#pragma once

#include <esp_wifi.h>
#include "CodeLib.h"

#ifdef __cplusplus
extern "C" {
#endif

void wifi_init_ap_sta(wifi_config_t *ap_cnf, wifi_config_t *sta_cnf);

//esp_netif_t* pxGetNetIfFromSocket(int sock);
//uint8_t get_client_ip4(int sock, esp_ip4_addr_t *out_addr);
uint8_t socket_link_subscribe(int sock, delegate_t *delegate);

#ifdef __cplusplus
}
#endif
