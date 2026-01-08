#include <stdio.h>
#include <string.h>
#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_http_server.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "cJSON.h"

#include <lwip/sockets.h>
#include <esp_netif.h>
#include <string.h>

#include <lwip/sockets.h>

#include "CodeLib.h"

#include "app.h"

extern httpd_handle_t server;

#define MACSTR "%02x:%02x:%02x:%02x:%02x:%02x"
#define MAC2STR(mac) mac[0], mac[1], mac[2], mac[3], mac[4], mac[5] 

#define NET_IF_AMOUNT  4

#define NET_IF_STA_IND  0
#define NET_IF_AP_IND   1
#define NET_IF_ETH_IND  2
#define NET_IF_PPP_IND  3

esp_netif_t *apxNetIf[NET_IF_AMOUNT] = {NULL, NULL, NULL, NULL};

static inline uint8_t __is_ipv4_to_v6_mapped(uint16_t *ipv6) {
    return (ipv6[0] == 0) && (ipv6[1] == 0) && (ipv6[2] == 0) && (ipv6[3] == 0) && 
           (ipv6[4] == 0) && (ipv6[5] == 0xffff);
}

// esp_netif_t* pxGetNetIfFromSocket(int sock) {
//     struct sockaddr addr;
//     socklen_t len = sizeof(addr);
//     if (getsockname(sock, &addr, &len) != 0) return NULL;

//     esp_ip6_addr *ip6_addr = NULL;
//     esp_ip4_addr *ip4_addr = NULL;
//     if(addr.sa_family == AF_INET) {
//         ip4_addr = (uint8_t *)(&((struct sockaddr_in*)&addr)->sin_addr.s_addr);
//     }
//     else if(addr.sa_family == AF_INET6) {
//         ip6_addr = (uint16_t *)(((struct sockaddr_in6*)&addr)->sin6_addr.s6_addr);
//         if(__is_ipv4_to_v6_mapped(ip6_addr)) {
//             ip4_addr = (uint8_t *)&ip6_addr[6];
//         }
//     }
//     else return NULL;

//     if (ip4_addr != NULL) {
//         ESP_LOGI(TAG, "Socket IP v4 " IPSTR, IP2STR(ip4_addr));
//         esp_netif_ip_info_t ip_info;
//         for (int i = 0; i < NET_IF_AMOUNT; i++) {
//             if (esp_netif_get_ip_info(apxNetIf[i], &ip_info) != ESP_OK) continue;
//             uint8_t *ipi = (uint8_t *)&ip_info.ip;
//             if (memcmp(ipi, ip4_addr, 4) == 0) {
//                 ESP_LOGI(TAG, "NetIf IP v4 " IPSTR, IP2STR(ipi));
//                 return apxNetIf[i];
//             }
//         }
//     }
//     else {
//         esp_netif_htonl()
//         ESP_LOGI(TAG, "Socket IP v6 "IPV6STR, IPV62STR((esp_ip6_addr *)ip6_addr));
//         esp_ip6_addr_t if_ip6_addrs[5];
//         int ip6_count;        
//         for (int i = 0; i < NET_IF_AMOUNT; i++) {
//             ip6_count = esp_netif_get_all_ip6(apxNetIf[i], if_ip6_addrs);
//             for (int i = 0; i < ip6_count; i++) {
//                 ESP_LOGI(TAG, "NetIf IP v6 "IPV6STR, IPV62STR(if_ip6_addrs[i]));
//                 if (memcmp(if_ip6_addrs[i].addr, ip6_addr, 16) == 0) return apxNetIf[i];
//             }
//         }
//     }

//     return NULL;
// }

// static void kill_socks_on_dead_interaface(httpd_handle_t server)
// {
// 	const int MAX_SOCKS = 20; //hd->config.max_open_sockets
// 	size_t try_socks = MAX_SOCKS;
// 	int fds[MAX_SOCKS];
// 	if (httpd_get_client_list(server, &try_socks, fds) != ESP_OK)
// 		goto err;
// 	for (int i = 0; i < try_socks; ++i)
// 		if (pxGetNetIfFromSocket(fds[i]) == NULL) //if is dead
// 			if (httpd_sess_trigger_close(server, fds[i]) != ESP_OK)
// 				goto err;
// err:
//         ESP_LOGI(TAG, "Can't kill socks");
// }

// static void kill_socks_on_dead_con(httpd_handle_t server)
// {
// 	const int MAX_SOCKS = 20; //hd->config.max_open_sockets
// 	size_t try_socks = MAX_SOCKS;
// 	int fds[MAX_SOCKS];
// 	if (httpd_get_client_list(server, &try_socks, fds) != ESP_OK)
// 		goto err;
// 	for (int i = 0; i < try_socks; ++i)
// 		if (pxGetNetIfFromSocket(fds[i]) == NULL) //if is dead
// 			if (httpd_sess_trigger_close(server, fds[i]) != ESP_OK)
// 				goto err;
// err:
//         ESP_LOGI(TAG, "Can't kill socks");
// }

typedef struct {
    __linked_list_object__
    esp_netif_t *netif; /*!< Pointer to the associated netif handle */
    esp_ip4_addr_t ip; /*!< IP address which was assigned to the client */
    uint8_t mac[6]; /*!< MAC address of the connected client */
    event_t event;
} link_t;


#define LINKS_MAX_AMOUNT  1
link_t links_pool[LINKS_MAX_AMOUNT];
linked_list_t links_free = NULL;
linked_list_t links_active = NULL;

static uint8_t link_ip_match(linked_list_item_t *item, void *arg) {
    esp_ip4_addr_t *ip = (esp_ip4_addr_t *)arg;
    link_t *link = linked_list_get_object(link_t, item);
    return mem_cmp(&link->ip, ip, sizeof(esp_ip4_addr_t)) == 0;
}

static uint8_t link_mac_match(linked_list_item_t *item, void *arg) {
    uint8_t *mac = (uint8_t *)arg;
    link_t *link = linked_list_get_object(link_t, item);
    return mem_cmp(link->mac, mac, 6) == 0;
}

// uint8_t socket_link_subscribe(int sock, delegate_t *delegate) {
//     ESP_LOGI(TAG, "Socket try get peer");
//     struct sockaddr_in peer_addr;
//     socklen_t peer_addr_len = sizeof(peer_addr);
//     if (getpeername(sock, (struct sockaddr *)&peer_addr, &peer_addr_len) == 0) {
//         // Convert the IP address from network to presentation format (string)
//         char ip_str[INET_ADDRSTRLEN];
//         inet_ntoa_r(peer_addr.sin_addr, ip_str, sizeof(ip_str));
//         // Convert the port number from network to host byte order
//         int port = ntohs(peer_addr.sin_port);
//         // Log or use the retrieved IP and port
//         ESP_LOGI("HTTP_SERVER", "Client IP: %s, Port: %d", ip_str, port);
//         // ... continue with HTTP response ...
//     } else {
//         ESP_LOGE("HTTP_SERVER", "Failed to get peer name");
//         return false;
//     }
//     //uint16_t *ip6_addr = NULL;
//     esp_ip6_addr_t *ip6_addr = NULL;
//     esp_ip4_addr_t *ip4_addr = NULL;
//     ESP_LOGI(TAG, "Socket family %u", peer_addr.sa_family);
//     ip4_addr = (esp_ip4_addr_t *)(&((struct sockaddr_in*)&peer_addr)->sin_addr.s_addr);
//     ESP_LOGI(TAG, "Socket IP " IPSTR, IP2STR(ip4_addr));
//     if(peer_addr.sa_family == AF_INET) {
//         ip4_addr = (esp_ip4_addr_t *)(&((struct sockaddr_in*)&peer_addr)->sin_addr.s_addr);
//     }
//     else if(peer_addr.sa_family == AF_INET6) {
//         ip6_addr = (esp_ip6_addr_t *)(&((struct sockaddr_in6*)&peer_addr)->sin6_addr.s6_addr);
//         ESP_LOGI(TAG, "Socket IP v6 " IPV6STR, IPV62STR(*ip6_addr));
//         if(__is_ipv4_to_v6_mapped((uint16_t *)ip6_addr)) {
//             ip4_addr = (esp_ip4_addr_t *)&((uint16_t *)ip6_addr)[6];
//         }
//     }
//     if(ip4_addr) {
//         ESP_LOGI(TAG, "Socket IP v4 " IPSTR, IP2STR(ip4_addr));
//         linked_list_item_t *item = linked_list_find_first(links_active, &link_ip_match, ip4_addr);
//         if(item != NULL) {
//             link_t *link = linked_list_get_object(link_t, item);
//             event_subscribe(&link->event, delegate);
//             return true;
//         }
//     }
//     return false;
// }

uint8_t socket_link_subscribe(int sock, delegate_t *delegate) {
    ESP_LOGI(TAG, "Socket try get peer");
    struct sockaddr_storage peer_addr;
    socklen_t peer_addr_len = sizeof(peer_addr);
    
    if (getpeername(sock, (struct sockaddr *)&peer_addr, &peer_addr_len) != 0) {
        ESP_LOGE(TAG, "Failed to get peer name");
        return false;
    }

    esp_ip6_addr_t *ip6_addr = NULL;
    esp_ip4_addr_t *ip4_addr = NULL;

    if (peer_addr.ss_family == AF_INET) {
        struct sockaddr_in *addr4 = (struct sockaddr_in*)&peer_addr;
        ip4_addr = (esp_ip4_addr_t*)&addr4->sin_addr;
        
        char ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, ip4_addr, ip_str, sizeof(ip_str));
        ESP_LOGI(TAG, "Socket IPv4: %s", ip_str);
        
    } else if (peer_addr.ss_family == AF_INET6) {
        struct sockaddr_in6 *addr6 = (struct sockaddr_in6*)&peer_addr;
        ip6_addr = (esp_ip6_addr_t*)&addr6->sin6_addr;
        
        char ip_str[INET6_ADDRSTRLEN];
        inet_ntop(AF_INET6, ip6_addr, ip_str, sizeof(ip_str));
        ESP_LOGI(TAG, "Socket IPv6: %s", ip_str);

        // Проверка на IPv4-mapped IPv6
        if (__is_ipv4_to_v6_mapped((uint16_t *)ip6_addr)) {
            ip4_addr = (esp_ip4_addr_t*)(((uint32_t*)ip6_addr) + 3);
            ESP_LOGI(TAG, "Mapped IPv4: " IPSTR, IP2STR(ip4_addr));
        }
    }

    if (ip4_addr) {
        linked_list_item_t *item = linked_list_find_first(links_active, &link_ip_match, ip4_addr);
        ESP_LOGI(TAG, "Try find socket link");
        if (item != NULL) {
            ESP_LOGI(TAG, "Link found, subscribe event");
            link_t *link = linked_list_get_object(link_t, item);
            event_subscribe(&link->event, delegate);
            return true;
        }
    }
    
    return false;
}

esp_err_t disconnect_sta_by_mac(uint8_t *mac_addr) {
    uint16_t aid;
    esp_err_t err = esp_wifi_ap_get_sta_aid(mac_addr, &aid);
    if (err != ESP_OK) return err;
    err = esp_wifi_deauth_sta(aid);
    return err;
}

static void ip_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    if ((event_base == IP_EVENT) && (event_id == IP_EVENT_AP_STAIPASSIGNED)) {
        ip_event_ap_staipassigned_t *data= (ip_event_ap_staipassigned_t *)event_data;
        ESP_LOGI(TAG, "Wi-Fi AP client assigned IP: " IPSTR " MAC: " MACSTR, IP2STR(&data->ip), MAC2STR(data->mac));
        linked_list_t item = linked_list_find_first(links_active, &link_mac_match, data->mac);
        if(!item) {
            if(links_free) {
                link_t *link = linked_list_get_object(link_t, links_free);
                link->netif = data->esp_netif;
                mem_cpy(&link->ip, &data->ip, sizeof(esp_ip4_addr_t));
                mem_cpy(link->mac, data->mac, 6);
                linked_list_insert_last(&links_active, linked_list_item(link));
            }
            else {
                /* we must never get here */
                ESP_LOGI(TAG, "Drop Wi-Fi AP client MAC: " MACSTR, MAC2STR(data->mac));
                disconnect_sta_by_mac(data->mac);
            }
        }
        else { /* update IP if changed */
            link_t *link = linked_list_get_object(link_t, item);
            if(mem_cmp(&link->ip, &data->ip, sizeof(esp_ip4_addr_t))) {
                ESP_LOGI(TAG, "Wi-Fi AP client new IP assignned");
                mem_cpy(&link->ip, &data->ip, sizeof(esp_ip4_addr_t));
                event_raise_clear(&link->event, NULL, NULL); /* drop connections */
            }
        }
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    //app_context_t* app = (app_context_t*) arg;
    /* todo AP client disconnected kill all sockets by client */

    ESP_LOGI(TAG, "Event id: %ld, base: %ld", event_id, (uint32_t)event_base);

    if ((event_base == WIFI_EVENT) && (event_id == WIFI_EVENT_AP_STADISCONNECTED)) {
        wifi_event_ap_stadisconnected_t *data = (wifi_event_ap_stadisconnected_t *)event_data;
        ESP_LOGI(TAG, "Wi-Fi AP client disconnect MAC: " MACSTR, MAC2STR(data->mac));
        linked_list_item_t *item = linked_list_find_first(links_active, &link_mac_match, data->mac);
        if(item != NULL) {
            link_t *link = linked_list_get_object(link_t, item);
            ESP_LOGI(TAG, "Wi-Fi AP client link down IP: " IPSTR " MAC: " MACSTR, IP2STR(&link->ip), MAC2STR(link->mac));
            event_raise_clear(&link->event, NULL, NULL);
            linked_list_insert_last(&links_free, item);
        }
    }

    if ((event_base == WIFI_EVENT) && (event_id == WIFI_EVENT_AP_STACONNECTED)) {
        wifi_event_ap_staconnected_t *data = (wifi_event_ap_staconnected_t *)event_data;
        if(!links_free || 
           linked_list_find_first(links_active, &link_mac_match, data->mac)) {
            ESP_LOGI(TAG, "Reject Wi-Fi AP client MAC: " MACSTR, MAC2STR(data->mac));
            esp_wifi_deauth_sta(data->aid);
        }
    }


    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
	    ESP_LOGI(TAG, "Wi-Fi STA disconnected, kill all sockets...");
	    //kill_socks_on_dead_interaface(server);
        ESP_LOGI(TAG, "Wi-Fi STA disconnected, trying to reconnect...");
        esp_wifi_connect();
    }
    if ((event_base == IP_EVENT) && (event_id == IP_EVENT_STA_GOT_IP)) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Wi-Fi STA assigned IP: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

void wifi_init_ap_sta(wifi_config_t *ap_cnf, wifi_config_t *sta_cnf) {

    for(uint8_t i = 0; i < LINKS_MAX_AMOUNT; i++)
        linked_list_insert_last(&links_free, linked_list_item(&links_pool[i]));

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    apxNetIf[NET_IF_STA_IND] = esp_netif_create_default_wifi_sta();
    assert(apxNetIf[NET_IF_STA_IND]);

    apxNetIf[NET_IF_AP_IND] = esp_netif_create_default_wifi_ap();
    assert(apxNetIf[NET_IF_AP_IND]);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, &ip_event_handler, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    if ((sta_cnf != NULL) && (sta_cnf->sta.ssid[0] != '\0') && (sta_cnf->sta.password[0] != '\0')) {
        ESP_LOGI(TAG, "Station connecting: SSID=%s, Password=%s", sta_cnf->sta.ssid, sta_cnf->sta.password);

        ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, sta_cnf));
    }

    ESP_LOGI(TAG, "Starting AP: SSID=%s, Password=%s", ap_cnf->ap.ssid, ap_cnf->ap.password);
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, ap_cnf));

    ESP_ERROR_CHECK(esp_wifi_start());
    if ((sta_cnf != NULL) && (sta_cnf->sta.ssid[0] != '\0')) {
        ESP_ERROR_CHECK(esp_wifi_connect());
    }

    ESP_LOGI(TAG, "Wi-Fi initialized in AP+STA mode");
    if ((sta_cnf != NULL) && (sta_cnf->sta.ssid[0] != '\0')) {
        ESP_LOGI(TAG, "STA: Connecting to %s...", sta_cnf->sta.ssid);
    }
}
