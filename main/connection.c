/* connection.c — точка сборки сети: порядок подъёма её частей и IPv4-адреса
 * интерфейсов.
 *
 * Части друг о друге не знают, но порядок подъёма важен: линки и скан
 * подписываются на события драйвера ДО esp_wifi_start, иначе первые события
 * пропали бы. */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_netif.h>
#include <esp_log.h>

#ifdef CONFIG_QEMU_BUILD
#include <esp_eth.h>
#include <esp_eth_mac.h>
#include <esp_eth_phy.h>
#endif

#include "app.h"
#include "connection.h"
#include "net_internal.h"

#define GW_LOG_MODULE GW_LOG_LINK
#include "gw_log.h"

/* ── Подъём сети ─────────────────────────────────────────────────────────── */

void net_init(app_context_t *app) {
    /* База сети — ДО подписок: обработчики Wi-Fi регистрируются на уже
     * существующем цикле событий, иначе регистрация отвергается. */
    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t loop_err = esp_event_loop_create_default();
    if (loop_err != ESP_OK && loop_err != ESP_ERR_INVALID_STATE)
        ESP_ERROR_CHECK(loop_err);

    /* Учёт линков и скан — только подписки на события: своё состояние они заводят
     * вместе с подпиской. */
    net_link_init();
    net_scan_init();

#ifdef CONFIG_QEMU_BUILD
    /* В сборке для QEMU единственный сетевой интерфейс — проводной. */
    eth_init_openeth();
#else
    net_wifi_init(&app->ap_cnf, &app->sta_cnf,
                  app->mac_sta, app->mac_ap, app->sta_bssid);
#endif

    /* Статические адреса (когда заказаны) должны стоять до того, как серверы
     * начнут на них отвечать; DHCP и умолчания esp_netif при этом не трогаются. */
    net_apply_config(app);
}

/* ── IPv4 обоих интерфейсов ─────────────────────────────────────────────── */

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
 * is meant to get an address on its own. */
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
            LOGI(TAG, "Station static address %s", buf);
        } else {
            esp_err_t err = esp_netif_dhcpc_start(sta);
            if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED)
                ESP_LOGW(TAG, "dhcpc start: %s", esp_err_to_name(err));
        }
    }

    esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap && app->net_ap.ip) {
        /* The DHCP server has to be stopped while the address changes: the AP is
         * its own gateway for the clients. */
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
        LOGI(TAG, "Access point address %s", buf);
    }
}

/* ── Проводной интерфейс (сборка для QEMU) ───────────────────────────────── */

#ifdef CONFIG_QEMU_BUILD
static void eth_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
    if (event_base == IP_EVENT && event_id == IP_EVENT_ETH_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        LOGI(TAG, "Ethernet got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

void eth_init_openeth(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *netif_eth = esp_netif_new(&netif_cfg);
    assert(netif_eth);

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
    ESP_ERROR_CHECK(esp_netif_attach(netif_eth, eth_glue));

    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP,
                                               &eth_event_handler, NULL));

    ESP_ERROR_CHECK(esp_eth_start(eth_handle));
    LOGI(TAG, "OpenEth Ethernet started, waiting for DHCP...");
}
#endif /* CONFIG_QEMU_BUILD */
