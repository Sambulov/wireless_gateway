/* net_wifi.c — Wi-Fi прибора: подъём STA+AP, обработчики событий драйвера,
 * переподключение станции и подстройка точки доступа под неё.
 *
 * Радио одно на обе половины, а режим роутера мы не выбираем: станция работает
 * «в авто», точка доступа следует за ней каналом и полосой. Свой режим (канал 1,
 * 20 МГц) у точки доступа только пока станция не подключена.
 *
 * Сюда же относятся заказанные MAC адреса интерфейсов: драйвер принимает их лишь
 * на остановленном интерфейсе, то есть ровно между esp_wifi_init и
 * esp_wifi_start. Учёт клиентов точки доступа — в net_link.c, скан — в
 * net_scan.c. */

#include <assert.h>
#include <string.h>

#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_netif.h>
#include <esp_log.h>

#include "app.h"
#include "connection.h"
#include "net_internal.h"

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

/* ── Выбранная точка доступа ──────────────────────────────────────────────── */

static uint8_t bssid_is_set(const uint8_t *bssid) {
    static const uint8_t zero[6] = {0};
    return bssid && (memcmp(bssid, zero, sizeof(zero)) != 0);
}

void net_wifi_sta_set_target(wifi_config_t *sta, const uint8_t *bssid) {
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

/* ── События драйвера ────────────────────────────────────────────────────────
 *
 * Здесь только то, что относится к самому радио: подключение и обрыв станции,
 * полученный ею адрес. События клиентов точки доступа ведёт net_link.c, конец
 * скана — net_scan.c: обработчиков у события несколько, и каждый берёт своё.
 */
static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id,
                               void *event_data) {
    ESP_LOGI(TAG, "Event id: %ld, base: %ld", event_id, (uint32_t)event_base);

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
                net_wifi_sta_set_target(&sta_live, NULL);
                if (esp_wifi_set_config(WIFI_IF_STA, &sta_live) == ESP_OK)
                    sta_pin_dropped = 1;
                sta_pin_miss = 0;
            }
        }
        ESP_LOGI(TAG, "Wi-Fi STA disconnected, kill all sockets...");
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

/* ── Клиенты точки доступа ───────────────────────────────────────────────── */

/* Stations currently associated with our softAP. Used as a guard for AP
 * credential changes: the gateway only accepts a new AP password while exactly
 * one station is associated (the administrator configuring it). */
uint8_t net_wifi_clients_count(void) {
    wifi_sta_list_t sta_list = {0};
    if (esp_wifi_ap_get_sta_list(&sta_list) != ESP_OK)
        return 0;
    if (sta_list.num < 0)
        return 0;
    return (uint8_t)sta_list.num;
}

/* ── Подъём ──────────────────────────────────────────────────────────────── */

void net_wifi_init(wifi_config_t *ap_cnf, wifi_config_t *sta_cnf,
                   const uint8_t *mac_sta, const uint8_t *mac_ap,
                   const uint8_t *sta_bssid) {

    ESP_ERROR_CHECK(esp_netif_init());
    /* Цикл событий создаёт net_init; повторное создание — не ошибка. */
    esp_err_t loop_err = esp_event_loop_create_default();
    if (loop_err != ESP_OK && loop_err != ESP_ERR_INVALID_STATE)
        ESP_ERROR_CHECK(loop_err);

    /* У каждой половины радио свой esp_netif: адресами и DHCP у них управляет он
     * же, и его же обработчик приносит интерфейс клиента. Ручка нужна только
     * чтобы убедиться, что интерфейс создан. */
    esp_netif_t *netif_sta = esp_netif_create_default_wifi_sta();
    assert(netif_sta);
    (void)netif_sta;

#if CONFIG_WIFI_AP_ENABLE
    esp_netif_t *netif_ap = esp_netif_create_default_wifi_ap();
    assert(netif_ap);
    (void)netif_ap;
#endif

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));

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
        net_wifi_sta_set_target(sta_cnf, sta_bssid);
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
