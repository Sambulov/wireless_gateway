#pragma once

#include <esp_wifi.h>
#include "CodeLib.h"
#include "ws_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Интерфейсы, к которым привязываются сервисы, описаны в ws_transport.h:
 * GW_IFACE_NONE / GW_IFACE_AP / GW_IFACE_STA / GW_IFACE_ETH и маска
 * GW_IFACE_MASK(iface) для регистрации (bApiCallRegister). */

void wifi_init_ap_sta(wifi_config_t *ap_cnf, wifi_config_t *sta_cnf);

#ifdef CONFIG_QEMU_BUILD
void eth_init_openeth(void);
#endif

//esp_netif_t* pxGetNetIfFromSocket(int sock);
//uint8_t get_client_ip4(int sock, esp_ip4_addr_t *out_addr);
uint8_t socket_link_subscribe(int sock, delegate_t *delegate);

/* Через какой сетевой интерфейс пришло соединение: локальный адрес сокета
 * сравнивается с адресами интерфейсов. Спрашивается один раз при установлении
 * соединения (транспорт запоминает ответ в дескрипторе) — на каждом событии
 * сеть не дёргается. GW_IFACE_NONE — определить не удалось. */
uint32_t gw_iface_of_socket(int fd);

#ifdef __cplusplus
}
#endif
