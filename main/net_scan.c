/* net_scan.c — единый движок сканирования эфира: каналы по одному, кэш сетей.
 *
 * Радио одно: на время скана точка доступа уходит со своего канала, а сплошной
 * обход 13 каналов держит её в отлучке ~3.7 с и клиенты отваливаются. Поэтому
 * каналы обходим ПО ОДНОМУ: короткое выслушивание (30–60 мс), возврат на канал
 * точки доступа и пауза SCAN_GAP_MS, за которую она отдаёт свои beacon'ы.
 *
 * Движок — ЕДИНСТВЕННЫЙ владелец сканирования: запросы API (wifi.scan) и поиск
 * точки для станции идут через него, иначе два владельца одного радио воюют.
 * Результаты копятся в scan_cache (ключ — BSSID): их отдаёт net_scan_take, а станция
 * ищет свою сеть по имени (net_scan_find) и запись старше SCAN_CACHE_TTL_MS считает
 * отсутствующей. Запуск неблокирующий: готовность канала ставит WIFI_EVENT_SCAN_DONE,
 * движение по каналам ведёт таймер обхода — ждать внутри обработчика API нельзя. */

#include <string.h>

#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_timer.h>

#include "app.h"
#include "connection.h"
#include "net_internal.h"

#define GW_LOG_MODULE GW_LOG_SCAN
#include "gw_log.h"

#define SCAN_CH_FIRST     1      /* первый канал 2.4 ГГц */
#define SCAN_CH_LAST      13     /* последний: 14-й разрешён не везде */
#define SCAN_CH_MS_MIN    30     /* выслушивание канала, нижняя граница */
#define SCAN_CH_MS_MAX    60     /* верхняя: дольше — точка доступа молчит слишком долго */
#define SCAN_GAP_MS       150    /* пауза между каналами: AP отдаёт beacon'ы и данные */
#define SCAN_CH_RETRY_MS  400    /* драйвер отказал (станция подключается) — ждём и пробуем */
#define SCAN_CH_RETRIES   5      /* столько отказов подряд терпим на одном канале */
#define SCAN_CACHE_MAX    32     /* сколько сетей помним */
#define SCAN_CACHE_TTL_MS 30000  /* старше — для станции считаем, что сети рядом нет */
#define SCAN_CH_RECS_MAX  16     /* больше сетей на одном канале не встречалось */

/* Состояние трогают обработчик события драйвера и задача ядра API, поэтому volatile. */
typedef enum {
    SCAN_IDLE = 0,     /* простаивает: никто не просил */
    SCAN_RUNNING,      /* драйвер выслушивает текущий канал */
    SCAN_CH_DONE,      /* канал выслушан, записи ещё не забраны */
    SCAN_GAP,          /* пауза перед следующим каналом (или перед повтором) */
    SCAN_ALL_DONE,     /* обход закончен, результат в кэше */
} scan_state_t;

static volatile scan_state_t scan_state = SCAN_IDLE;

static uint8_t  scan_channel;               /* какой канал слушаем сейчас */
static uint8_t  scan_retries;               /* сколько раз драйвер отказал на нём */
static int64_t  scan_gap_until_us;          /* раньше этого канал не трогаем */
static int64_t  scan_walk_start_us;         /* когда начался текущий обход */
static uint16_t scan_walk_n;                /* сколько сетей нашлось за обход */
static wifi_ap_record_t scan_ch_recs[SCAN_CH_RECS_MAX];  /* записи одного канала */

/* Кэш сетей: ключ — BSSID; seen_ms — когда запись видели, по нему станция понимает
 * свежесть (см. SCAN_CACHE_TTL_MS). */
typedef struct {
    wifi_ap_record_t rec;
    uint32_t         seen_ms;
} scan_cache_entry_t;

static scan_cache_entry_t scan_cache[SCAN_CACHE_MAX];
static uint16_t scan_cache_n;

/* Такт обхода. Своей задачи у движка нет: состояние обхода трогает только этот
 * таймер, а сервис API и станция лишь просят обход начать и смотрят готовность. */
#define SCAN_TICK_MS 30
static esp_timer_handle_t scan_timer;

static int64_t scan_now_us(void) {
    return esp_timer_get_time();
}

static uint32_t scan_now_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

uint8_t net_scan_busy(void) {
    return (scan_state != SCAN_IDLE) && (scan_state != SCAN_ALL_DONE);
}

uint8_t net_scan_ready(void) {
    return scan_state == SCAN_ALL_DONE;
}

/* Выслушать один канал. 0 — драйвер не дал запустить (станция подключается). */
static uint8_t scan_channel_start(uint8_t ch) {
    /* Активный скан: ответ на probe приходит за доли выслушивания, ждать beacon'ов
     * не нужно. Верхняя граница 60 мс — чтобы точка доступа скорее вернулась на свой
     * канал, а не про полноту списка. */
    wifi_scan_config_t cfg = {
        .ssid        = NULL,
        .bssid       = NULL,
        .channel     = ch,
        .show_hidden = 0,
        .scan_type   = WIFI_SCAN_TYPE_ACTIVE,
    };
    cfg.scan_time.active.min = SCAN_CH_MS_MIN;
    cfg.scan_time.active.max = SCAN_CH_MS_MAX;

    scan_state = SCAN_RUNNING;
    if (esp_wifi_scan_start(&cfg, false) != ESP_OK) {
        scan_state = SCAN_GAP;
        return 0;
    }
    return 1;
}

static void scan_cache_put(const wifi_ap_record_t *rec) {
    for (uint16_t i = 0; i < scan_cache_n; i++) {
        if (memcmp(scan_cache[i].rec.bssid, rec->bssid, 6) == 0) {
            scan_cache[i].rec     = *rec;
            scan_cache[i].seen_ms = scan_now_ms();
            return;
        }
    }
    if (scan_cache_n < SCAN_CACHE_MAX) {
        scan_cache[scan_cache_n].rec     = *rec;
        scan_cache[scan_cache_n].seen_ms = scan_now_ms();
        scan_cache_n++;
    }
}

/* Записи канала — в кэш, список драйвера освободить: второй раз его не прочитать. */
static void scan_accumulate(void) {
    uint16_t num = 0;
    if (esp_wifi_scan_get_ap_num(&num) == ESP_OK && num) {
        uint16_t take = (num > SCAN_CH_RECS_MAX) ? SCAN_CH_RECS_MAX : num;
        if (esp_wifi_scan_get_ap_records(&take, scan_ch_recs) == ESP_OK) {
            for (uint16_t i = 0; i < take; i++) {
                scan_walk_n++;
                scan_cache_put(&scan_ch_recs[i]);
            }
        }
    }
    esp_wifi_clear_ap_list();
    LOGI(TAG, "Скан: канал %u выслушан, в кэше %u (за обход найдено %u)",
             (unsigned)scan_channel, (unsigned)scan_cache_n, (unsigned)scan_walk_n);
}

static void scan_cache_purge(void) {
    uint32_t started_ms = (uint32_t)(scan_walk_start_us / 1000);
    uint16_t keep = 0;

    for (uint16_t i = 0; i < scan_cache_n; i++) {
        if (scan_cache[i].seen_ms >= started_ms)
            scan_cache[keep++] = scan_cache[i];
    }
    scan_cache_n = keep;
}

uint8_t net_scan_start(void) {
    /* Обход уже идёт — второй не начинаем; готовый результат (SCAN_ALL_DONE) лежит в кэше. */
    if (net_scan_busy())
        return 0;

    scan_walk_n     = 0;
    scan_retries    = 0;
    scan_channel    = SCAN_CH_FIRST;
    scan_walk_start_us = scan_now_us();

    if (!scan_channel_start(scan_channel)) {
        scan_state = SCAN_IDLE;
        ESP_LOGW(TAG, "Скан не запустился: драйвер занят");
        return 0;
    }
    esp_timer_stop(scan_timer);
    esp_timer_start_periodic(scan_timer, (uint64_t)SCAN_TICK_MS * 1000u);
    LOGI(TAG, "Скан эфира запущен: каналы %u..%u по одному, пауза %u мс",
             SCAN_CH_FIRST, SCAN_CH_LAST, SCAN_GAP_MS);
    return 1;
}

/* Шаг обхода. Зовётся ТОЛЬКО из таймера обхода — он единственный владелец состояния. */
static void scan_step(void) {
    switch (scan_state) {
    case SCAN_CH_DONE:
        scan_accumulate();
        if (scan_channel >= SCAN_CH_LAST) {
            scan_cache_purge();
            scan_state = SCAN_ALL_DONE;
            esp_timer_stop(scan_timer);
            LOGI(TAG, "Скан эфира завершён: сетей в кэше %u", (unsigned)scan_cache_n);
            break;
        }
        scan_channel++;
        scan_retries = 0;
        scan_gap_until_us = scan_now_us() + (int64_t)SCAN_GAP_MS * 1000;
        scan_state = SCAN_GAP;
        break;

    case SCAN_GAP:
        if (scan_now_us() < scan_gap_until_us)
            break;
        if (scan_channel_start(scan_channel))
            break;                      /* ушли в SCAN_RUNNING */
        /* Драйвер занят (станция подключается): пробуем ЭТОТ ЖЕ канал снова, а не
         * обрываем обход — иначе клиенту достаётся огрызок списка. */
        if (++scan_retries > SCAN_CH_RETRIES) {
            scan_cache_purge();
            scan_state = SCAN_ALL_DONE;
            esp_timer_stop(scan_timer);
            ESP_LOGW(TAG, "Скан: канал %u не запустился %u раз, отдаём найденное",
                     (unsigned)scan_channel, (unsigned)scan_retries);
            break;
        }
        scan_gap_until_us = scan_now_us() + (int64_t)SCAN_CH_RETRY_MS * 1000;
        break;

    default:
        break;
    }
}

static void scan_timer_cb(void *arg) {
    (void)arg;
    scan_step();
}

void net_scan_cancel(void) {
    if (scan_state == SCAN_RUNNING)
        esp_wifi_scan_stop();
    /* Список драйвера живёт в его куче: не забрали результат — освобождаем здесь. */
    if (net_scan_busy())
        esp_wifi_clear_ap_list();
    scan_state = SCAN_IDLE;
    esp_timer_stop(scan_timer);
}

uint16_t net_scan_take(wifi_ap_record_t *out, uint16_t max, uint16_t *found) {
    if (found) *found = scan_walk_n;
    if (!out || !max || scan_state != SCAN_ALL_DONE) {
        scan_state = SCAN_IDLE;
        return 0;
    }

    uint16_t take = (scan_cache_n > max) ? max : scan_cache_n;
    for (uint16_t i = 0; i < take; i++)
        out[i] = scan_cache[i].rec;

    scan_state = SCAN_IDLE;            /* результат отдан; кэш остаётся для станции */
    LOGI(TAG, "Скан эфира: отдано %u из %u", (unsigned)take, (unsigned)scan_cache_n);
    return take;
}

/* Поиск сети в кэше: имя обязательно, BSSID — если точка выбрана; 0 — сети нет или
 * запись старше TTL. */
uint8_t net_scan_find(const char *ssid, const uint8_t *bssid,
                      wifi_ap_record_t *out, uint32_t *age_ms) {
    const scan_cache_entry_t *best = NULL;
    uint32_t now = scan_now_ms();

    if (!ssid || (ssid[0] == '\0'))
        return 0;

    for (uint16_t i = 0; i < scan_cache_n; i++) {
        const scan_cache_entry_t *e = &scan_cache[i];
        if (bssid && (memcmp(bssid, e->rec.bssid, 6) != 0))
            continue;
        if (strncmp((const char *)e->rec.ssid, ssid, sizeof(e->rec.ssid)) != 0)
            continue;
        if ((now - e->seen_ms) > SCAN_CACHE_TTL_MS)
            continue;                  /* запись устарела — сети рядом уже нет */
        if (!best || (e->rec.rssi > best->rec.rssi))
            best = e;
    }

    if (!best)
        return 0;
    if (out)     *out     = best->rec;
    if (age_ms)  *age_ms  = now - best->seen_ms;
    return 1;
}

/* Канал выслушан (сам или отменой): у отменённого скана состояние уже SCAN_IDLE. */
static void scan_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id,
                               void *event_data) {
    (void)arg;
    (void)event_base;   /* обработчик подписан только на WIFI_EVENT/SCAN_DONE */
    (void)event_id;
    (void)event_data;

    if (scan_state == SCAN_RUNNING)
        scan_state = SCAN_CH_DONE;
}

void net_scan_init(void) {
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_SCAN_DONE,
                                               &scan_event_handler, NULL));

    const esp_timer_create_args_t scan_timer_args = {
        .callback = scan_timer_cb,
        .name     = "scan_tick",
    };
    ESP_ERROR_CHECK(esp_timer_create(&scan_timer_args, &scan_timer));
}
