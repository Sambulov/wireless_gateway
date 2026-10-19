/* net_scan.c — скан эфира: запуск, состояние и приём результата.
 *
 * Запуск неблокирующий: список сетей драйвер собирает сам, а признак готовности
 * ставит событие WIFI_EVENT_SCAN_DONE. Обработчик сервиса wifi.scan вызывается и
 * в такте работы, поэтому ждать внутри нельзя: сервис запускает скан и в
 * следующих тактах спрашивает состояние. Список драйвера забирается ровно один
 * раз — net_scan_take освобождает его. */

#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_log.h>

#include "app.h"
#include "connection.h"
#include "net_internal.h"

/* Состояние скана: 0 — простаивает, 1 — идёт, 2 — результат готов и ждёт
 * читателя. Ставится и снимается из обработчика события Wi-Fi, поэтому volatile:
 * сервис wifi.scan спрашивает состояние из задачи ядра API. */
#define SCAN_IDLE      0
#define SCAN_RUNNING   1
#define SCAN_DONE      2

static volatile uint8_t scan_state = SCAN_IDLE;

uint8_t net_scan_busy(void) {
    return scan_state == SCAN_RUNNING;
}

uint8_t net_scan_ready(void) {
    return scan_state == SCAN_DONE;
}

uint8_t net_scan_start(void) {
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

void net_scan_cancel(void) {
    if (scan_state == SCAN_RUNNING)
        esp_wifi_scan_stop();
    /* Список драйвера живёт в его куче: если скан уже закончился, но результат
     * никто не забрал (вызов сняли), освобождаем его здесь — иначе каждый такой
     * скан оставлял бы за собой память. */
    if (scan_state != SCAN_IDLE)
        esp_wifi_clear_ap_list();
    scan_state = SCAN_IDLE;
}

uint16_t net_scan_take(wifi_ap_record_t *out, uint16_t max, uint16_t *found) {
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

/* Конец скана (сам или по net_scan_cancel): результат ждёт читателя. Отменённый
 * скан сюда тоже попадает — состояние тогда уже SCAN_IDLE, и трогать его не
 * нужно. */
static void scan_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id,
                               void *event_data) {
    (void)arg;
    (void)event_base;   /* обработчик подписан только на WIFI_EVENT/SCAN_DONE */
    (void)event_id;
    (void)event_data;

    if (scan_state == SCAN_RUNNING) {
        scan_state = SCAN_DONE;
        ESP_LOGI(TAG, "Скан эфира завершён");
    }
}

void net_scan_init(void) {
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_SCAN_DONE,
                                               &scan_event_handler, NULL));
}
