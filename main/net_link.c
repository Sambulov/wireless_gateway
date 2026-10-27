/* net_link.c — учёт соединений: записи о клиентах точки доступа (MAC и адрес) и
 * связка «сокет ↔ сетевой интерфейс».
 *
 * Запись живёт от выдачи адреса до обрыва линка: сервис подписывает на неё своё
 * соединение и закрывает по событию, не дожидаясь, пока стек заметит пропажу.
 * gw_iface_of_socket отвечает, через какой интерфейс пришло соединение. */

#include <string.h>
#include "net_iface.h"

#include <lwip/sockets.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <esp_log.h>

#include "CodeLib.h"
#include "app.h"
#include "connection.h"
#include "net_internal.h"

#define GW_LOG_MODULE GW_LOG_LINK
#include "gw_log.h"

/* ── Записи о клиентах точки доступа ─────────────────────────────────────────
 * Пул фиксированного размера: своей памяти запись не просит, а исчерпание пула —
 * повод отказать клиенту в линке: без учёта мы не узнаем об обрыве. */
#define LINKS_MAX_AMOUNT  4

typedef struct {
    __linked_list_object__
    esp_netif_t *netif;   /* интерфейс, на котором клиент получил адрес */
    esp_ip4_addr_t ip;    /* адрес, выданный клиенту */
    uint8_t mac[6];       /* MAC подключённого клиента */
    event_t event;        /* подписчики: ждут обрыва этого линка */
} link_t;

static link_t links_pool[LINKS_MAX_AMOUNT];
static linked_list_t links_free = NULL;
static linked_list_t links_active = NULL;

static uint8_t net_link_ip_match(linked_list_item_t *item, void *arg) {
    esp_ip4_addr_t *ip = (esp_ip4_addr_t *)arg;
    link_t *link = linked_list_get_object(link_t, item);
    return mem_cmp(&link->ip, ip, sizeof(esp_ip4_addr_t)) == 0;
}

static uint8_t net_link_mac_match(linked_list_item_t *item, void *arg) {
    uint8_t *mac = (uint8_t *)arg;
    link_t *link = linked_list_get_object(link_t, item);
    return mem_cmp(link->mac, mac, 6) == 0;
}

/* Отключить клиента по MAC: нужен, когда записи нет, а пустить без учёта нельзя. */
static esp_err_t sta_disconnect(uint8_t *mac_addr) {
    uint16_t aid;
    esp_err_t err = esp_wifi_ap_get_sta_aid(mac_addr, &aid);
    if (err != ESP_OK) return err;
    err = esp_wifi_deauth_sta(aid);
    return err;
}

/* Клиент ушёл: будим подписчиков и возвращаем запись в пул. Будить надо ДО
 * возврата записи — иначе следующий клиент занял бы её под живое соединение. */
static void net_link_drop(linked_list_item_t *item) {
    link_t *link = linked_list_get_object(link_t, item);
    LOGI(TAG, "Wi-Fi AP client link down IP: " IPSTR " MAC: " MACSTR,
             IP2STR(&link->ip), MAC2STR(link->mac));
    event_raise(&link->event, NULL, NULL);
    event_clear(&link->event);
    linked_list_insert_last(&links_free, item);
}

/* Адрес выдан клиенту точки доступа: заводим запись, а если адрес у того же MAC
 * сменился — обновляем и будим подписчиков, чтобы старые соединения закрылись. */
static void net_link_assigned(esp_netif_t *netif, esp_ip4_addr_t *ip, uint8_t *mac) {
    linked_list_t item = linked_list_find_first(links_active, &net_link_mac_match, mac);

    if (!item) {
        if (!links_free) {
            /* we must never get here */
            LOGI(TAG, "Drop Wi-Fi AP client MAC: " MACSTR, MAC2STR(mac));
            sta_disconnect(mac);
            return;
        }
        link_t *link = linked_list_get_object(link_t, links_free);
        link->netif = netif;
        mem_cpy(&link->ip, ip, sizeof(esp_ip4_addr_t));
        mem_cpy(link->mac, mac, 6);
        linked_list_insert_last(&links_active, linked_list_item(link));
        return;
    }

    link_t *link = linked_list_get_object(link_t, item);
    if (mem_cmp(&link->ip, ip, sizeof(esp_ip4_addr_t))) {
        LOGI(TAG, "Wi-Fi AP client new IP assignned");
        mem_cpy(&link->ip, ip, sizeof(esp_ip4_addr_t));
        event_raise(&link->event, NULL, NULL); /* drop connections */
        event_clear(&link->event);
    }
}

/* События линков точки доступа: от драйвера (клиент подключился/ушёл) и от esp_netif
 * (клиенту выдан адрес). Записи ведёт только этот обработчик. */
static void net_link_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id,
                               void *event_data) {
    if ((event_base == WIFI_EVENT) && (event_id == WIFI_EVENT_AP_STADISCONNECTED)) {
        wifi_event_ap_stadisconnected_t *data = (wifi_event_ap_stadisconnected_t *)event_data;
        LOGI(TAG, "Wi-Fi AP client disconnect MAC: " MACSTR, MAC2STR(data->mac));
        linked_list_item_t *item = linked_list_find_first(links_active, &net_link_mac_match, data->mac);
        if (item != NULL)
            net_link_drop(item);
    }

    if ((event_base == WIFI_EVENT) && (event_id == WIFI_EVENT_AP_STACONNECTED)) {
        wifi_event_ap_staconnected_t *data = (wifi_event_ap_staconnected_t *)event_data;
        /* Второй линк того же MAC или клиент без записи (пул занят) — отключаем. */
        if (!links_free ||
            linked_list_find_first(links_active, &net_link_mac_match, data->mac)) {
            LOGI(TAG, "Reject Wi-Fi AP client MAC: " MACSTR, MAC2STR(data->mac));
            esp_wifi_deauth_sta(data->aid);
        }
    }

    if ((event_base == IP_EVENT) && (event_id == IP_EVENT_ASSIGNED_IP_TO_CLIENT)) {
        ip_event_assigned_ip_to_client_t *data = (ip_event_assigned_ip_to_client_t *)event_data;
        LOGI(TAG, "Wi-Fi AP client assigned IP: " IPSTR " MAC: " MACSTR,
                 IP2STR(&data->ip), MAC2STR(data->mac));
        net_link_assigned(data->esp_netif, &data->ip, data->mac);
    }
}

void net_link_init(void) {
    for (uint8_t i = 0; i < LINKS_MAX_AMOUNT; i++)
        linked_list_insert_last(&links_free, linked_list_item(&links_pool[i]));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &net_link_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID,
                                               &net_link_event_handler, NULL));
}

/* ── Интерфейс, через который пришло соединение ────────────────────────────── */

/* Адрес IPv4 в сетевом порядке байт из адреса сокета. lwIP здесь dual-stack:
 * IPv4 приходит и как AF_INET, и как ::FFFF:a.b.c.d (AF_INET6). 0 — это не IPv4. */
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

/* Через какой интерфейс пришло соединение с этим сокетом. Локальный адрес сокета —
 * адрес того интерфейса, с которого нас достигли: клиент точки доступа попал на адрес
 * AP, клиент сети станции — на адрес STA. Спрашивается один раз, ответ запоминается
 * в дескрипторе. */
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

/* ── Подписка сокета на обрыв линка ────────────────────────────────────────── */

/* Подписывает соединение на запись его клиента: как только линк оборвётся (или
 * клиенту выдадут другой адрес), делегат получит событие и транспорт закроет
 * соединение сам. Адрес берём у peer'а: клиент точки доступа известен по адресу,
 * который выдал ему DHCP. 0 — соединение не от клиента точки доступа. */
uint8_t net_link_subscribe(int sock, delegate_t *delegate) {
    LOGI(TAG, "Socket try get peer");

    struct sockaddr_storage peer_addr;
    socklen_t peer_addr_len = sizeof(peer_addr);
    if (getpeername(sock, (struct sockaddr *)&peer_addr, &peer_addr_len) != 0) {
        ESP_LOGE(TAG, "Failed to get peer name");
        return false;
    }

    char ip_str[INET6_ADDRSTRLEN];
    if (peer_addr.ss_family == AF_INET) {
        const struct sockaddr_in *addr4 = (const struct sockaddr_in *)&peer_addr;
        inet_ntop(AF_INET, &addr4->sin_addr, ip_str, sizeof(ip_str));
        LOGI(TAG, "Socket IPv4: %s", ip_str);
    } else if (peer_addr.ss_family == AF_INET6) {
        const struct sockaddr_in6 *addr6 = (const struct sockaddr_in6 *)&peer_addr;
        inet_ntop(AF_INET6, &addr6->sin6_addr, ip_str, sizeof(ip_str));
        LOGI(TAG, "Socket IPv6: %s", ip_str);
    }

    uint32_t peer_ip4 = sockaddr_ip4(&peer_addr);
    if (!peer_ip4)
        return false;

    esp_ip4_addr_t peer_esp = { .addr = peer_ip4 };
    LOGI(TAG, "Socket peer IPv4: " IPSTR, IP2STR(&peer_esp));

    LOGI(TAG, "Try find socket link");
    linked_list_item_t *item = linked_list_find_first(links_active, &net_link_ip_match, &peer_esp);
    if (item == NULL)
        return false;

    LOGI(TAG, "Link found, subscribe event");
    link_t *link = linked_list_get_object(link_t, item);
    event_subscribe(&link->event, delegate);
    return true;
}
