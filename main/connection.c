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

#include <lwip/sockets.h>
#include <esp_netif.h>
#include <string.h>

#include <lwip/sockets.h>

#include "CodeLib.h"

#include "app.h"

#ifdef CONFIG_QEMU_BUILD
#include "esp_eth.h"
#include "esp_eth_mac.h"
#include "esp_eth_phy.h"
#endif

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


#define LINKS_MAX_AMOUNT  4
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
    /* IDF v6 переименовала событие и его структуру (старые имена помечены
     * deprecated, значения те же): IP_EVENT_AP_STAIPASSIGNED →
     * IP_EVENT_ASSIGNED_IP_TO_CLIENT. */
    if ((event_base == IP_EVENT) && (event_id == IP_EVENT_ASSIGNED_IP_TO_CLIENT)) {
        ip_event_assigned_ip_to_client_t *data = (ip_event_assigned_ip_to_client_t *)event_data;
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
                event_raise(&link->event, NULL, NULL); /* drop connections */
                event_clear(&link->event);
            }
        }
    }
}

/* ── Канал и полоса точки доступа ─────────────────────────────────────────────
 *
 * Радио у прибора одно на обе половины, а режим роутера мы не выбираем: станция
 * работает как есть, «в авто». Поэтому точку доступа подстраиваем под станцию —
 * и канал, и полосу. Сами канал с полосой выбираем только когда станции нет:
 * канал 1 и 20 МГц, самый распространённый и совместимый режим на 2,4 ГГц.
 *
 * Разная полоса у половин радио — худший случай: драйвер переключается между
 * интерфейсами, и отдача проваливается до единиц килобайт в секунду (замерено
 * 6 КБ/с против 190).
 */
#define AP_OWN_CHANNEL 1
#define AP_OWN_BW      WIFI_BW20

static void ap_apply_bandwidth(wifi_bandwidth_t bw, int channel)
{
    /* Уже такая полоса — НЕ трогаем. Запись настроек на работающей точке доступа
     * перезапускает её и рассылает deauth: в логе это «softap is sending deauth»,
     * после чего клиенты (в том числе тот, кто открыл страницу) отваливаются. */
    wifi_bandwidth_t cur = WIFI_BW20;
    if (esp_wifi_get_bandwidth(WIFI_IF_AP, &cur) == ESP_OK && cur == bw) {
        ESP_LOGD(TAG, "AP: полоса уже %s — не трогаем",
                 bw == WIFI_BW40 ? "40 МГц" : "20 МГц");
        return;
    }
    if (esp_wifi_set_bandwidth(WIFI_IF_AP, bw) != ESP_OK) {
        ESP_LOGW(TAG, "AP: не удалось выставить полосу");
        return;
    }
    ESP_LOGI(TAG, "AP: полоса %s%s", bw == WIFI_BW40 ? "40 МГц" : "20 МГц",
             channel ? " (как у станции)" : " (свой режим)");
}

/* Станция подключилась: повторяем за роутером. Канал точку доступа заставит
 * принять сам драйвер (он общий для обеих половин), а полосу выставляем руками. */
static void ap_follow_sta(void)
{
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK) return;
    wifi_bandwidth_t bw = (ap_info.second == WIFI_SECOND_CHAN_NONE) ? WIFI_BW20 : WIFI_BW40;
    ESP_LOGI(TAG, "STA: канал %d, вторичный %d", ap_info.primary, ap_info.second);
    ap_apply_bandwidth(bw, ap_info.primary);
}

/* Станции нет: точка доступа возвращается к своему режиму. Пишем только то, что
 * ДЕЙСТВИТЕЛЬНО отличается: этот путь вызывается на каждом обрыве связи со
 * стороны станции (в том числе когда связь рвём мы сами), а лишняя запись в
 * настройки работающей точки доступа выбивает её клиентов. */
static void ap_own_mode(void)
{
    wifi_config_t ap;
    if (esp_wifi_get_config(WIFI_IF_AP, &ap) == ESP_OK && ap.ap.channel != AP_OWN_CHANNEL) {
        ap.ap.channel = AP_OWN_CHANNEL;
        esp_wifi_set_config(WIFI_IF_AP, &ap);
    }
    ap_apply_bandwidth(AP_OWN_BW, 0);
}

/* ── Выбранная точка доступа ──────────────────────────────────────────────── */

static uint8_t bssid_is_set(const uint8_t *bssid) {
    static const uint8_t zero[6] = {0};
    return bssid && (memcmp(bssid, zero, sizeof(zero)) != 0);
}

void wifi_sta_set_target(wifi_config_t *sta, const uint8_t *bssid) {
    if (!sta) return;
    if (bssid_is_set(bssid)) {
        memcpy(sta->sta.bssid, bssid, 6);
        sta->sta.bssid_set = true;
        sta->sta.scan_method = WIFI_FAST_SCAN;
        ESP_LOGI(TAG, "STA: выбрана точка " MACSTR, MAC2STR(bssid));
    } else {
        memset(sta->sta.bssid, 0, 6);
        sta->sta.bssid_set = false;
        /* Точка не выбрана: обходим ВСЕ каналы и берём самую сильную из найденных
         * с этим именем. Fast scan нашёл бы первую в порядке обхода каналов — при
         * двух точках с одним именем это может быть дальняя. */
        sta->sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    }
    sta->sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
}

/* Сколько неудачных попыток искать ВЫБРАННУЮ точку терпим, прежде чем снять
 * привязку и подключиться к любой другой с тем же именем. */
#define STA_PIN_MISS_MAX 3
static uint8_t sta_pin_miss;

/* Привязка снята — говорим об этом в логе один раз, чтобы не сыпать строками. */
static uint8_t sta_pin_dropped;

/* ── Заказанные MAC адреса интерфейсов ────────────────────────────────────── */

/* Значение из NVS/настроек: NULL или все нули — «заводской», подмены нет. */
static uint8_t mac_is_set(const uint8_t *mac) {
    static const uint8_t zero[6] = {0};
    return mac && (memcmp(mac, zero, sizeof(zero)) != 0);
}

/* Подмена MAC возможна только на остановленном интерфейсе, то есть здесь —
 * до esp_wifi_start(). Ошибку не считаем смертельной: неподходящий адрес
 * (например, уже занятый другим интерфейсом) — повод остаться с заводским,
 * а не уйти в перезагрузку по ESP_ERROR_CHECK. */
static void wifi_apply_mac(wifi_interface_t iface, const uint8_t *mac, const char *name) {
    if (!mac_is_set(mac)) {
        ESP_LOGI(TAG, "%s MAC: заводской", name);
        return;
    }
    esp_err_t err = esp_wifi_set_mac(iface, mac);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s MAC " MACSTR " не принят: %s", name, MAC2STR(mac), esp_err_to_name(err));
        return;
    }
    ESP_LOGW(TAG, "%s MAC заменён на " MACSTR, name, MAC2STR(mac));
}

/* ── Сканирование эфира ───────────────────────────────────────────────────── */

/* Состояние скана: 0 — простаивает, 1 — идёт, 2 — результат готов и ждёт
 * читателя. Ставится и снимается из обработчика события Wi-Fi, поэтому volatile:
 * сервис wifi.scan спрашивает состояние из задачи ядра API. */
#define SCAN_IDLE      0
#define SCAN_RUNNING   1
#define SCAN_DONE      2

static volatile uint8_t scan_state = SCAN_IDLE;

uint8_t wifi_scan_busy(void) {
    return scan_state == SCAN_RUNNING;
}

uint8_t wifi_scan_ready(void) {
    return scan_state == SCAN_DONE;
}

uint8_t wifi_scan_start(void) {
    if (scan_state != SCAN_IDLE)
        return 0;

    /* Активный скан: beacon'ов ждать не нужно, ответ приходит за доли секунды на
     * канал. Верхняя граница времени на канал — 1500 мс, выше драйвер не
     * советует: станция начнёт терять свою сеть. Скрытые сети не показываем:
     * имя такой сети всё равно вводится руками, а строка без имени в таблице
     * только мешает. */
    wifi_scan_config_t cfg = {
        .ssid        = NULL,
        .bssid       = NULL,
        .channel     = 0,
        .show_hidden = 0,
        .scan_type   = WIFI_SCAN_TYPE_ACTIVE,
    };
    cfg.scan_time.active.min = 120;
    cfg.scan_time.active.max = 1500;

    scan_state = SCAN_RUNNING;
    esp_err_t err = esp_wifi_scan_start(&cfg, false);
    if (err != ESP_OK) {
        scan_state = SCAN_IDLE;
        ESP_LOGW(TAG, "Скан не запустился: %s", esp_err_to_name(err));
        return 0;
    }
    ESP_LOGI(TAG, "Скан эфира запущен");
    return 1;
}

void wifi_scan_cancel(void) {
    if (scan_state == SCAN_RUNNING)
        esp_wifi_scan_stop();
    /* Список драйвера живёт в его куче: если скан уже закончился, но результат
     * никто не забрал (вызов сняли), освобождаем его здесь — иначе каждый такой
     * скан оставлял бы за собой память. */
    if (scan_state != SCAN_IDLE)
        esp_wifi_clear_ap_list();
    scan_state = SCAN_IDLE;
}

uint16_t wifi_scan_take(wifi_ap_record_t *out, uint16_t max, uint16_t *found) {
    uint16_t num = 0;
    if (found) *found = 0;
    if (!out || !max || scan_state != SCAN_DONE) {
        scan_state = SCAN_IDLE;
        return 0;
    }

    if (esp_wifi_scan_get_ap_num(&num) != ESP_OK)
        num = 0;
    if (found) *found = num;

    uint16_t take = (num > max) ? max : num;
    if (take && esp_wifi_scan_get_ap_records(&take, out) != ESP_OK)
        take = 0;

    /* Список драйвера живёт в его куче: освобождаем сразу после чтения, иначе
     * каждый скан оставлял бы за собой память. */
    esp_wifi_clear_ap_list();
    scan_state = SCAN_IDLE;
    ESP_LOGI(TAG, "Скан эфира: найдено %u, забрано %u", (unsigned)num, (unsigned)take);
    return take;
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
            event_raise(&link->event, NULL, NULL);
            event_clear(&link->event);
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


    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_SCAN_DONE) {
        /* Скан закончился (сам или по wifi_scan_stop): результат ждёт читателя.
         * Отменённый скан сюда тоже попадает — состояние тогда уже SCAN_IDLE,
         * и трогать его не нужно. */
        if (scan_state == SCAN_RUNNING) {
            scan_state = SCAN_DONE;
            ESP_LOGI(TAG, "Скан эфира завершён");
        }
    }
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        /* Выбранной точки нет в эфире: после нескольких попыток снимаем привязку,
         * иначе станция так и не подключится, хотя другая точка с тем же именем
         * рядом. В NVS выбор остаётся — следующая загрузка снова ищет её. */
        wifi_event_sta_disconnected_t *dd = (wifi_event_sta_disconnected_t *)event_data;
        wifi_config_t sta_live;
        uint8_t pinned = (esp_wifi_get_config(WIFI_IF_STA, &sta_live) == ESP_OK)
                         && sta_live.sta.bssid_set;
        if (pinned && dd && dd->reason == WIFI_REASON_NO_AP_FOUND) {
            if (++sta_pin_miss >= STA_PIN_MISS_MAX) {
                ESP_LOGW(TAG, "Выбранной точки нет в эфире (%u попытки) — подключаемся к любой с этим именем",
                         sta_pin_miss);
                wifi_sta_set_target(&sta_live, NULL);
                if (esp_wifi_set_config(WIFI_IF_STA, &sta_live) == ESP_OK)
                    sta_pin_dropped = 1;
                sta_pin_miss = 0;
            }
        }
	    ESP_LOGI(TAG, "Wi-Fi STA disconnected, kill all sockets...");
	    //kill_socks_on_dead_interaface(server);
        ap_own_mode();                 /* роутера нет — работаем своим режимом */
        ESP_LOGI(TAG, "Wi-Fi STA disconnected, trying to reconnect...");
        esp_wifi_connect();
    }
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        sta_pin_miss = 0;              /* нашли — счётчик промахов сбрасываем */
        ap_follow_sta();               /* подтянуть канал и полосу за роутером */
    }
    if ((event_base == IP_EVENT) && (event_id == IP_EVENT_STA_GOT_IP)) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Wi-Fi STA assigned IP: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

/* Stations currently associated with our softAP. Used as a guard for AP
 * credential changes: the gateway only accepts a new AP password while exactly
 * one station is associated (the administrator configuring it). */
uint8_t wifi_ap_clients_count(void) {
    wifi_sta_list_t sta_list = {0};
    if (esp_wifi_ap_get_sta_list(&sta_list) != ESP_OK)
        return 0;
    if (sta_list.num < 0)
        return 0;
    return (uint8_t)sta_list.num;
}

/* ── IPv4 configuration of both interfaces ───────────────────────────────── */

/* Pulls a plain IPv4 address out of a socket address. lwIP sockets here are
 * dual-stack AF_INET6, so an IPv4 peer shows up as ::FFFF:a.b.c.d — the same
 * shape socket_link_subscribe() already deals with below. */
static uint32_t sockaddr_ip4(const struct sockaddr_storage *ss) {
    if (ss->ss_family == AF_INET)
        return ((const struct sockaddr_in *)ss)->sin_addr.s_addr;

    if (ss->ss_family == AF_INET6) {
        static const uint8_t mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
        const struct sockaddr_in6 *a6 = (const struct sockaddr_in6 *)ss;
        if (memcmp(a6->sin6_addr.s6_addr, mapped, sizeof(mapped)) == 0) {
            uint32_t ip4;
            memcpy(&ip4, a6->sin6_addr.s6_addr + 12, sizeof(ip4));
            return ip4;
        }
    }
    return 0;
}

/* Через какой интерфейс пришло соединение с этим сокетом. Локальный адрес
 * сокета — адрес того интерфейса, с которого нас достигли: клиент точки доступа
 * попал на адрес AP, клиент из сети, куда подключена наша станция, — на адрес
 * STA. Спрашивается один раз при установлении соединения: транспорт запоминает
 * ответ в дескрипторе и больше сеть не дёргает. */
uint32_t gw_iface_of_socket(int fd) {
    if (fd < 0) return GW_IFACE_NONE;

    struct sockaddr_storage local;
    socklen_t len = sizeof(local);
    memset(&local, 0, sizeof(local));
    if (getsockname(fd, (struct sockaddr *)&local, &len) != 0) {
        ESP_LOGW(TAG, "getsockname(%d) failed", fd);
        return GW_IFACE_NONE;
    }

    uint32_t local_ip = sockaddr_ip4(&local);
    if (!local_ip) return GW_IFACE_NONE;

    /* Порядок важен только для читаемости: адреса интерфейсов не совпадают. */
    static const struct {
        const char *ifkey;
        uint32_t    iface;
    } ifaces[] = {
        { "WIFI_AP_DEF",  GW_IFACE_AP  },
        { "WIFI_STA_DEF", GW_IFACE_STA },
        { "ETH_DEF",      GW_IFACE_ETH },
    };

    for (size_t i = 0; i < sizeof(ifaces) / sizeof(ifaces[0]); i++) {
        esp_netif_t *nif = esp_netif_get_handle_from_ifkey(ifaces[i].ifkey);
        esp_netif_ip_info_t ip;
        if (!nif || esp_netif_get_ip_info(nif, &ip) != ESP_OK) continue;
        if (ip.ip.addr == local_ip) return ifaces[i].iface;
    }

    return GW_IFACE_NONE;
}

uint8_t net_parse_ip4(const char *str, uint32_t *out) {
    /* Note: ESP-IDF v6 turned esp_netif_str_to_ip4() from bool into esp_err_t */
    esp_ip4_addr_t addr;
    if (!str || esp_netif_str_to_ip4(str, &addr) != ESP_OK)
        return 1;
    if (out) *out = addr.addr;
    return 0;
}

void net_format_ip4(uint32_t ip, char *out, size_t out_size) {
    if (!ip) {
        out[0] = '\0';
        return;
    }
    esp_ip4_addr_t addr = { .addr = ip };
    snprintf(out, out_size, IPSTR, IP2STR(&addr));
}

/* Applies a static address, or (re)starts the DHCP client where the interface
 * is meant to obtain an address on its own. */
void net_apply_config(const app_context_t *app) {
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta) {
        if ((app->net_sta.mode == NET_MODE_STATIC) && app->net_sta.ip) {
            /* The DHCP client has to be stopped before the address can be set */
            esp_err_t err = esp_netif_dhcpc_stop(sta);
            if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED)
                ESP_LOGW(TAG, "dhcpc stop: %s", esp_err_to_name(err));

            esp_netif_ip_info_t ip = {
                .ip      = { .addr = app->net_sta.ip },
                .netmask = { .addr = app->net_sta.mask },
                .gw      = { .addr = app->net_sta.gw },
            };
            err = esp_netif_set_ip_info(sta, &ip);
            if (err != ESP_OK)
                ESP_LOGE(TAG, "station static address failed: %s", esp_err_to_name(err));

            esp_netif_dns_info_t dns = {0};
            dns.ip.type = ESP_IPADDR_TYPE_V4;
            if (app->net_sta.dns1) {
                dns.ip.u_addr.ip4.addr = app->net_sta.dns1;
                esp_netif_set_dns_info(sta, ESP_NETIF_DNS_MAIN, &dns);
            }
            if (app->net_sta.dns2) {
                dns.ip.u_addr.ip4.addr = app->net_sta.dns2;
                esp_netif_set_dns_info(sta, ESP_NETIF_DNS_BACKUP, &dns);
            }
            char buf[16];
            net_format_ip4(app->net_sta.ip, buf, sizeof(buf));
            ESP_LOGI(TAG, "Station static address %s", buf);
        } else {
            esp_err_t err = esp_netif_dhcpc_start(sta);
            if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED)
                ESP_LOGW(TAG, "dhcpc start: %s", esp_err_to_name(err));
        }
    }

    esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap && app->net_ap.ip) {
        /* The DHCP server has to be stopped while the address changes, and the
         * AP is its own gateway for the clients. */
        esp_err_t err = esp_netif_dhcps_stop(ap);
        if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED)
            ESP_LOGW(TAG, "dhcps stop: %s", esp_err_to_name(err));

        esp_netif_ip_info_t ip = {
            .ip      = { .addr = app->net_ap.ip },
            .netmask = { .addr = app->net_ap.mask },
            .gw      = { .addr = app->net_ap.ip },
        };
        err = esp_netif_set_ip_info(ap, &ip);
        if (err != ESP_OK)
            ESP_LOGE(TAG, "AP address failed: %s", esp_err_to_name(err));

        err = esp_netif_dhcps_start(ap);
        if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED)
            ESP_LOGW(TAG, "dhcps start: %s", esp_err_to_name(err));

        char buf[16];
        net_format_ip4(app->net_ap.ip, buf, sizeof(buf));
        ESP_LOGI(TAG, "Access point address %s", buf);
    }
}

void wifi_init_ap_sta(wifi_config_t *ap_cnf, wifi_config_t *sta_cnf,
                      const uint8_t *mac_sta, const uint8_t *mac_ap,
                      const uint8_t *sta_bssid) {

    for(uint8_t i = 0; i < LINKS_MAX_AMOUNT; i++)
        linked_list_insert_last(&links_free, linked_list_item(&links_pool[i]));
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    apxNetIf[NET_IF_STA_IND] = esp_netif_create_default_wifi_sta();
    assert(apxNetIf[NET_IF_STA_IND]);

#if CONFIG_WIFI_AP_ENABLE
    apxNetIf[NET_IF_AP_IND] = esp_netif_create_default_wifi_ap();
    assert(apxNetIf[NET_IF_AP_IND]);
#endif

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, &ip_event_handler, NULL));

#if CONFIG_WIFI_AP_ENABLE
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
#else
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
#endif

    /* Заказанные MAC — после init/mode и до старта: интерфейс ещё остановлен,
     * только в этом состоянии драйвер принимает новый адрес. */
    wifi_apply_mac(WIFI_IF_STA, mac_sta, "STA");
#if CONFIG_WIFI_AP_ENABLE
    wifi_apply_mac(WIFI_IF_AP, mac_ap, "AP");
#endif

    if ((sta_cnf != NULL) && (sta_cnf->sta.ssid[0] != '\0')) {
        ESP_LOGI(TAG, "Station connecting: SSID=%s, Password=%s", sta_cnf->sta.ssid, sta_cnf->sta.password);
        wifi_sta_set_target(sta_cnf, sta_bssid);
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, sta_cnf));
    }

#if CONFIG_WIFI_AP_ENABLE
    ESP_LOGI(TAG, "Starting AP: SSID=%s, Password=%s", ap_cnf->ap.ssid, ap_cnf->ap.password);
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, ap_cnf));
#endif

    ESP_ERROR_CHECK(esp_wifi_start());

    /* Шлюз питается от сети, экономить энергию незачем, а умолчание
     * WIFI_PS_MIN_MODEM стоит очень дорого: станция просыпается только к
     * beacon'ам, поэтому КАЖДЫЙ принятый пакет ждёт окна DTIM. Отсюда
     * постоянные 60-300 мс на любой запрос (замерено: ответ INFO без всякого
     * Modbus — 82-297 мс) и провалы скорости отдачи. WIFI_PS_NONE убирает эту
     * задержку. */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    /* Станцию не ограничиваем: роутер сам решает, на какой полосе работать, и
     * наша задача — повторить его режим точкой доступа (см. ap_follow_sta при
     * событии STA_CONNECTED). Пока станции нет, точка доступа работает своим
     * режимом — канал 1 и 20 МГц. */
#if CONFIG_WIFI_AP_ENABLE
    ap_own_mode();
#endif

    if ((sta_cnf != NULL) && (sta_cnf->sta.ssid[0] != '\0')) {
        ESP_ERROR_CHECK(esp_wifi_connect());
        ESP_LOGI(TAG, "STA: Connecting to %s...", sta_cnf->sta.ssid);
    }

#if CONFIG_WIFI_AP_ENABLE
    ESP_LOGI(TAG, "Wi-Fi initialized in AP+STA mode");
#else
    ESP_LOGI(TAG, "Wi-Fi initialized in STA-only mode");
#endif
}

#ifdef CONFIG_QEMU_BUILD
static void eth_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
    if (event_base == IP_EVENT && event_id == IP_EVENT_ETH_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Ethernet got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

void eth_init_openeth(void)
{
    for (uint8_t i = 0; i < LINKS_MAX_AMOUNT; i++)
        linked_list_insert_last(&links_free, linked_list_item(&links_pool[i]));

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    apxNetIf[NET_IF_ETH_IND] = esp_netif_new(&netif_cfg);
    assert(apxNetIf[NET_IF_ETH_IND]);

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.autonego_timeout_ms = 100;
    phy_config.reset_gpio_num = -1;

    esp_eth_mac_t *mac = esp_eth_mac_new_openeth(&mac_config);
    esp_eth_phy_t *phy = esp_eth_phy_new_dp83848(&phy_config);

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth_handle = NULL;
    ESP_ERROR_CHECK(esp_eth_driver_install(&eth_config, &eth_handle));

    esp_eth_netif_glue_handle_t eth_glue = esp_eth_new_netif_glue(eth_handle);
    ESP_ERROR_CHECK(esp_netif_attach(apxNetIf[NET_IF_ETH_IND], eth_glue));

    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP,
                                               &eth_event_handler, NULL));

    ESP_ERROR_CHECK(esp_eth_start(eth_handle));
    ESP_LOGI(TAG, "OpenEth Ethernet started, waiting for DHCP...");
}
#endif /* CONFIG_QEMU_BUILD */
