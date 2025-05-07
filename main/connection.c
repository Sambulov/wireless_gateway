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

#include "app.h"

static const char *TAG = "connection";

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

esp_netif_t* pxGetNetIfFromSocket(int sock) {
    struct sockaddr_storage addr_storage;
    socklen_t len = sizeof(addr_storage);
    if (getsockname(sock, (struct sockaddr*)&addr_storage, &len) != 0) return NULL;

    uint16_t *ip6_addr = NULL;
    uint8_t *ip4_addr = NULL;
    if(addr_storage.ss_family == AF_INET) {
        ip4_addr = (uint8_t *)(&((struct sockaddr_in*)&addr_storage)->sin_addr.s_addr);
    }
    else if(addr_storage.ss_family == AF_INET6) {
        ip6_addr = (uint16_t *)(((struct sockaddr_in6*)&addr_storage)->sin6_addr.s6_addr);
        if(__is_ipv4_to_v6_mapped(ip6_addr)) {
            ip4_addr = (uint8_t *)&ip6_addr[6];
        }
    }
    else return NULL;

    if (ip4_addr != NULL) {
        ESP_LOGI(TAG, "Socket IP v4 %d.%d.%d.%d", ip4_addr[0], ip4_addr[1], ip4_addr[2], ip4_addr[3]);
        esp_netif_ip_info_t ip_info;
        for (int i = 0; i < NET_IF_AMOUNT; i++) {
            if (esp_netif_get_ip_info(apxNetIf[i], &ip_info) != ESP_OK) continue;
            uint8_t *ipi = (uint8_t *)&ip_info.ip;
            if (memcmp(ipi, ip4_addr, 4) == 0) {
                ESP_LOGI(TAG, "NetIf IP v4 %d.%d.%d.%d", ipi[0], ipi[1], ipi[2], ipi[3]);
                return apxNetIf[i];
            }
        }
    }
    else {
        ESP_LOGI(TAG, "Socket IP v6 "IPV6STR, ip6_addr[0], ip6_addr[1], ip6_addr[2], ip6_addr[3], 
                                              ip6_addr[4], ip6_addr[5], ip6_addr[6], ip6_addr[7]);
        esp_ip6_addr_t if_ip6_addrs[5];
        int ip6_count;        
        for (int i = 0; i < NET_IF_AMOUNT; i++) {
            ip6_count = esp_netif_get_all_ip6(apxNetIf[i], if_ip6_addrs);
            for (int i = 0; i < ip6_count; i++) {
                ESP_LOGI(TAG, "NetIf IP v6 "IPV6STR, IPV62STR(if_ip6_addrs[i]));
                if (memcmp(if_ip6_addrs[i].addr, ip6_addr, 16) == 0) return apxNetIf[i];
            }
        }
    }

    return NULL;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    //app_context_t* app = (app_context_t*) arg;

    ESP_LOGI(TAG, "Event id: %ld, base: %ld", event_id, (uint32_t)event_base);
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "Wi-Fi disconnected, trying to reconnect...");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Wi-Fi connected, IP: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

void wifi_init_ap_sta(wifi_config_t *ap_cnf, wifi_config_t *sta_cnf) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    apxNetIf[NET_IF_STA_IND] = esp_netif_create_default_wifi_sta();
    assert(apxNetIf[NET_IF_STA_IND]);

    apxNetIf[NET_IF_AP_IND] = esp_netif_create_default_wifi_ap();
    assert(apxNetIf[NET_IF_AP_IND]);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

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
