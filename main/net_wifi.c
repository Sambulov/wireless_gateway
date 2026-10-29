/* net_wifi.c — Wi-Fi прибора: подъём STA+AP, обработчики событий драйвера,
 * переподключение станции и подстройка точки доступа под неё.
 *
 * Радио одно на обе половины: станция работает «в авто», точка доступа следует
 * за ней каналом и полосой, свой режим (канал 1, 20 МГц) — только без станции.
 * Заказанные MAC интерфейсов: драйвер принимает их лишь между esp_wifi_init и
 * esp_wifi_start. Учёт клиентов точки доступа — net_link.c, скан — net_scan.c. */

#include <assert.h>
#include <string.h>

#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_netif.h>
#include <esp_log.h>
#include <esp_timer.h>

#include "app.h"
#include "connection.h"
#include "net_internal.h"

#define GW_LOG_MODULE GW_LOG_WIFI
#include "gw_log.h"

/* ── Заказанные MAC адреса интерфейсов ────────────────────────────────────── */

/* Значение из NVS/настроек: NULL или все нули — «заводской», подмены нет. */
static uint8_t mac_is_set(const uint8_t *mac) {
    static const uint8_t zero[6] = {0};
    return mac && (memcmp(mac, zero, sizeof(zero)) != 0);
}

/* Подмена MAC возможна только на остановленном интерфейсе — здесь, до
 * esp_wifi_start(); отказ не смертелен: остаёмся с заводским адресом. */
static void wifi_apply_mac(wifi_interface_t iface, const uint8_t *mac, const char *name) {
    if (!mac_is_set(mac)) {
        LOGI(TAG, "%s MAC: заводской", name);
        return;
    }
    esp_err_t err = esp_wifi_set_mac(iface, mac);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s MAC " MACSTR " не принят: %s", name, MAC2STR(mac), esp_err_to_name(err));
        return;
    }
    LOGI(TAG, "%s MAC заменён на " MACSTR, name, MAC2STR(mac));
}

/* ── Темп попыток станции и окно тишины точки доступа ──────────────────────── */
#define STA_RETRY_FIRST_MS  3000u    /* пауза перед первой повторной попыткой */
#define STA_RETRY_MAX_MS    60000u   /* предел нарастания паузы */
#define AP_QUIET_MS         15000u   /* столько не сканировать после события AP */
#define STA_SCAN_WAIT_MS    200u     /* как часто переспрашивать движок сканирования */

static uint32_t sta_retry_ms = STA_RETRY_FIRST_MS;
static uint32_t ap_quiet_until_ms;
static esp_timer_handle_t sta_retry_timer;
/* 1 — обход эфира запустили мы; второй раз за попытку не просим. */
static uint8_t sta_walk_ours;

/* ── Выбранная точка доступа ──────────────────────────────────────────────── */

static uint8_t bssid_is_set(const uint8_t *bssid) {
    static const uint8_t zero[6] = {0};
    return bssid && (memcmp(bssid, zero, sizeof(zero)) != 0);
}

void net_wifi_sta_set_target(wifi_config_t *sta, const uint8_t *bssid) {
    if (!sta) return;
    sta_retry_ms = STA_RETRY_FIRST_MS;
    if (sta_retry_timer != NULL)
        esp_timer_stop(sta_retry_timer);
    if (bssid_is_set(bssid)) {
        memcpy(sta->sta.bssid, bssid, 6);
        sta->sta.bssid_set = true;
        sta->sta.scan_method = WIFI_FAST_SCAN;
        LOGI(TAG, "STA: выбрана точка " MACSTR, MAC2STR(bssid));
    } else {
        memset(sta->sta.bssid, 0, 6);
        sta->sta.bssid_set = false;
        /* Точка не выбрана: обходим ВСЕ каналы и берём самую сильную с этим именем —
         * fast scan взял бы первую по порядку каналов, а она может быть дальней. */
        sta->sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    }
    sta->sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
}

/* Сколько промахов по ВЫБРАННОЙ точке терпим, прежде чем снять привязку. */
#define STA_PIN_MISS_MAX 3
static uint8_t sta_pin_miss;

/* Привязка снята — говорим об этом в логе один раз, чтобы не сыпать строками. */
static uint8_t sta_pin_dropped;

/* Радио одно: скан станции снимает точку доступа с эфира, и рукопожатие WPA2 нового
 * клиента рвётся по таймауту («leave, reason = 15»). Отсюда редкие попытки станции
 * и окно тишины вокруг сопряжения клиентов точки доступа. */

static uint32_t wifi_now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* У точки доступа появился или пропал клиент: сопряжение — не время для скана. */
static void ap_quiet_hold(void) {
    ap_quiet_until_ms = wifi_now_ms() + AP_QUIET_MS;
}

static void sta_retry_cb(void *arg);

/* Отложить следующую попытку станции: не раньше, чем кончится окно тишины. */
static void sta_retry_schedule(void) {
    int32_t wait = (int32_t)(ap_quiet_until_ms - wifi_now_ms());
    if (wait < (int32_t)sta_retry_ms)
        wait = (int32_t)sta_retry_ms;
    esp_timer_stop(sta_retry_timer);         /* не запущен — не ошибка */
    esp_timer_start_once(sta_retry_timer, (uint64_t)wait * 1000u);
}

static void sta_retry_cb(void *arg) {
    (void)arg;

    if ((int32_t)(ap_quiet_until_ms - wifi_now_ms()) > 0) {
        sta_retry_schedule();
        return;
    }

    if (net_scan_busy()) {
        esp_timer_stop(sta_retry_timer);
        esp_timer_start_once(sta_retry_timer, (uint64_t)STA_SCAN_WAIT_MS * 1000u);
        return;
    }

    wifi_config_t sta;
    if ((esp_wifi_get_config(WIFI_IF_STA, &sta) != ESP_OK) || (sta.sta.ssid[0] == '\0'))
        return;                        /* станция не настроена — молчим */

    /* Сеть ищем в кэше движка сканирования: своего скана станция не делает. */
    wifi_ap_record_t rec;
    uint32_t age_ms = 0;
    const uint8_t *pinned = sta.sta.bssid_set ? sta.sta.bssid : NULL;

    if (net_scan_find((const char *)sta.sta.ssid, pinned, &rec, &age_ms)) {
        sta_walk_ours = 0;
        /* Запись свежая: BSSID и канал известны — подключаемся точечно, без обхода. */
        sta.sta.bssid_set = true;
        memcpy(sta.sta.bssid, rec.bssid, 6);
        sta.sta.channel = rec.primary;
        if (esp_wifi_set_config(WIFI_IF_STA, &sta) == ESP_OK) {
            LOGI(TAG, "STA: " MACSTR " (канал %u, %d дБм, кэш %lu мс) — подключаемся",
                     MAC2STR(rec.bssid), rec.primary, rec.rssi, (unsigned long)age_ms);
            esp_wifi_connect();
            return;
        }
        ESP_LOGW(TAG, "STA: настройки точки не приняты");
    }

    /* Сети в кэше нет. Обход просим ОДИН раз на попытку: иначе станция сканирует
     * непрерывно и душит точку доступа (а клиенту API достаётся BUSY). */
    if (!sta_walk_ours) {
        if (net_scan_start()) {
            sta_walk_ours = 1;
            LOGI(TAG, "STA: %s в кэше нет, запускаю обход эфира", sta.sta.ssid);
        } else {
            LOGD(TAG, "STA: движок занят обходом, ждём");
        }
        esp_timer_stop(sta_retry_timer);
        esp_timer_start_once(sta_retry_timer, (uint64_t)STA_SCAN_WAIT_MS * 1000u);
        return;
    }

    /* Обход прошёл, а сети в эфире не было: пауза по backoff, потом новая попытка. */
    sta_walk_ours = 0;
    sta_retry_ms = (sta_retry_ms < STA_RETRY_MAX_MS / 2) ? sta_retry_ms * 2 : STA_RETRY_MAX_MS;
    LOGI(TAG, "STA: %s в эфире не нашлась, следующая попытка через %u мс",
             sta.sta.ssid, (unsigned)sta_retry_ms);
    sta_retry_schedule();
}

/* ── Канал и полоса точки доступа ─────────────────────────────────────────────
 * Точку доступа подстраиваем под роутер; свой режим (канал 1, 20 МГц) — только без
 * станции. Разная полоса у половин радио — худший случай: драйвер переключается
 * между интерфейсами, отдача проваливается до единиц КБ/с. */
#define AP_OWN_CHANNEL 1
#define AP_OWN_BW      WIFI_BW20

static void ap_apply_bandwidth(wifi_bandwidth_t bw, int channel)
{
    /* Уже такая полоса — НЕ трогаем: запись настроек работающей точки доступа
     * перезапускает её и рассылает deauth, клиенты отваливаются. */
    wifi_bandwidth_t cur = WIFI_BW20;
    if (esp_wifi_get_bandwidth(WIFI_IF_AP, &cur) == ESP_OK && cur == bw) {
        LOGD(TAG, "AP: полоса уже %s — не трогаем",
                 bw == WIFI_BW40 ? "40 МГц" : "20 МГц");
        return;
    }
    if (esp_wifi_set_bandwidth(WIFI_IF_AP, bw) != ESP_OK) {
        ESP_LOGW(TAG, "AP: не удалось выставить полосу");
        return;
    }
    LOGI(TAG, "AP: полоса %s%s", bw == WIFI_BW40 ? "40 МГц" : "20 МГц",
             channel ? " (как у станции)" : " (свой режим)");
}

/* Станция подключилась: повторяем за роутером. Канал точка доступа примет сама (он
 * общий для половин), а полосу выставляем руками. */
static void ap_follow_sta(void)
{
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK) return;
    wifi_bandwidth_t bw = (ap_info.second == WIFI_SECOND_CHAN_NONE) ? WIFI_BW20 : WIFI_BW40;
    LOGI(TAG, "STA: канал %d, вторичный %d", ap_info.primary, ap_info.second);
    ap_apply_bandwidth(bw, ap_info.primary);
}

/* Станции нет: точка доступа возвращается к своему режиму. Пишем только то, что
 * ДЕЙСТВИТЕЛЬНО отличается, — лишняя запись выбивает клиентов работающей точки. */
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
 * Здесь только радио: подключение и обрыв станции, полученный ею адрес. Клиентов
 * точки доступа ведёт net_link.c, конец скана — net_scan.c. */
static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id,
                               void *event_data) {
    /* Событий драйвера много: строка только для отладки, на INFO не печатается. */
    LOGD(TAG, "Event id: %ld, base: %ld", event_id, (uint32_t)event_base);

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        /* Выбранной точки нет в эфире: после нескольких попыток снимаем привязку,
         * иначе станция не подключится, хотя точка с этим именем рядом. В NVS
         * выбор остаётся — следующая загрузка снова ищет её. */
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
        LOGI(TAG, "Wi-Fi STA disconnected, kill all sockets...");
        ap_own_mode();                 /* роутера нет — работаем своим режимом */
        /* Попытки не чаще 3 с, 6 с, 12 с … до 60 с: иначе шлюз без внешней сети
         * вообще не даёт к себе подключиться (см. STA_RETRY_FIRST_MS). */
        sta_retry_ms = (sta_retry_ms < STA_RETRY_MAX_MS / 2) ? sta_retry_ms * 2
                                                            : STA_RETRY_MAX_MS;
        LOGI(TAG, "Wi-Fi STA disconnected, retry in %u ms...", (unsigned)sta_retry_ms);
        sta_retry_schedule();
    }
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        sta_pin_miss = 0;
        sta_retry_ms = STA_RETRY_FIRST_MS;
        esp_timer_stop(sta_retry_timer);
        /* Энергосбережение выключено всегда: питание от сети, а WIFI_PS_MIN_MODEM
         * роняет отдачу до одного окна TCP на DTIM. Драйвер возвращает умолчание
         * при переподключении, поэтому настройку повторяем. */
        esp_wifi_set_ps(WIFI_PS_NONE);
        ap_follow_sta();
    }
    /* Сопряжение клиента точки доступа: станция в это время молчит (см. AP_QUIET_MS). */
    if (event_base == WIFI_EVENT
        && (event_id == WIFI_EVENT_AP_STACONNECTED || event_id == WIFI_EVENT_AP_STADISCONNECTED)) {
        ap_quiet_hold();
    }
    if ((event_base == IP_EVENT) && (event_id == IP_EVENT_STA_GOT_IP)) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;
        (void)event;   /* без журнала не используется */
        LOGI(TAG, "Wi-Fi STA assigned IP: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

/* ── Клиенты точки доступа ───────────────────────────────────────────────── */

/* Stations currently associated with our softAP: the gateway accepts a new AP
 * password only while exactly one station is associated (the administrator). */
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

    /* У каждой половины радио свой esp_netif: адресами и DHCP управляет он же, а
     * ручка нужна только чтобы убедиться, что интерфейс создан. */
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

    /* Таймер попыток станции: обрыв не сканирует сразу, а откладывает попытку. */
    const esp_timer_create_args_t sta_timer_args = {
        .callback = sta_retry_cb,
        .name     = "sta_retry",
    };
    ESP_ERROR_CHECK(esp_timer_create(&sta_timer_args, &sta_retry_timer));

#if CONFIG_WIFI_AP_ENABLE
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
#else
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
#endif

    /* Заказанные MAC — после init/mode и до старта: только на остановленном
     * интерфейсе драйвер принимает новый адрес. */
    wifi_apply_mac(WIFI_IF_STA, mac_sta, "STA");
#if CONFIG_WIFI_AP_ENABLE
    wifi_apply_mac(WIFI_IF_AP, mac_ap, "AP");
#endif

    if ((sta_cnf != NULL) && (sta_cnf->sta.ssid[0] != '\0')) {
        LOGI(TAG, "Station connecting: SSID=%s, Password=%s", sta_cnf->sta.ssid, sta_cnf->sta.password);
        net_wifi_sta_set_target(sta_cnf, sta_bssid);
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, sta_cnf));
    }

#if CONFIG_WIFI_AP_ENABLE
    LOGI(TAG, "Starting AP: SSID=%s, Password=%s", ap_cnf->ap.ssid, ap_cnf->ap.password);
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, ap_cnf));
#endif

    ESP_ERROR_CHECK(esp_wifi_start());

    /* Шлюз питается от сети, экономить энергию незачем, а умолчание
     * WIFI_PS_MIN_MODEM дорого: станция просыпается только к beacon'ам, и каждый
     * пакет ждёт окна DTIM — десятки-сотни мс на запрос. WIFI_PS_NONE снимает это. */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    /* Станцию не ограничиваем: полосу выбирает роутер, а мы повторяем его режим
     * точкой доступа (ap_follow_sta). Без станции — свой режим: канал 1, 20 МГц. */
#if CONFIG_WIFI_AP_ENABLE
    ap_own_mode();
#endif

    if ((sta_cnf != NULL) && (sta_cnf->sta.ssid[0] != '\0')) {
        /* Первую попытку тоже ведём через движок сканирования (sta_retry_cb): не
         * полным сканом драйвера — иначе на старте точка доступа уходит с эфира. */
        sta_retry_ms = STA_RETRY_FIRST_MS;
        esp_timer_stop(sta_retry_timer);
        esp_timer_start_once(sta_retry_timer, (uint64_t)STA_SCAN_WAIT_MS * 1000u);
        LOGI(TAG, "STA: ищу %s через движок сканирования", sta_cnf->sta.ssid);
    }

#if CONFIG_WIFI_AP_ENABLE
    LOGI(TAG, "Wi-Fi initialized in AP+STA mode");
#else
    LOGI(TAG, "Wi-Fi initialized in STA-only mode");
#endif
}
