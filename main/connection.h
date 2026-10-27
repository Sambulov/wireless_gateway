#pragma once

/* connection.h — публичный контракт сети: что приложение и сервисы просят у
 * соединений прибора. Сеть разложена по частям (connection.c, net_wifi.c, net_scan.c,
 * net_link.c), но вызывающему знать это не нужно: имена ниже — единственный вход.
 * Функциям, которым нужен весь объект приложения (net_init), — место в app.h. */

#include <stdint.h>
#include <stddef.h>
#include "net_iface.h"

#include <esp_wifi.h>
#include "CodeLib.h"
#include "transport.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Интерфейсы сервисов (GW_IFACE_*) описаны в transport.h; ядру API они больше не
 * нужны — их спрашивают веб-сервер (main/web_httpd.c) и веб-политика
 * (main/web_iface_policy.c). */

/* ── Радио (net_wifi.c) ───────────────────────────────────────────────────────
 * Поднимает Wi-Fi и применяет ЗАКАЗАННЫЕ MAC интерфейсов: mac_sta и mac_ap — по
 * шесть байт, NULL или все нули значат «заводской». Новый адрес драйвер принимает
 * только на остановленном интерфейсе — между esp_wifi_init и esp_wifi_start. */
void net_wifi_init(wifi_config_t *ap_cnf, wifi_config_t *sta_cnf,
                   const uint8_t *mac_sta, const uint8_t *mac_ap,
                   const uint8_t *sta_bssid);

/* Выбранная пользователем точка доступа (BSSID): вписывает её в настройки станции.
 * bssid пуст — станция обязана брать СИЛЬНЕЙШУЮ точку с этим именем, а не первую
 * попавшуюся, как делает fast scan; выбранной нет в эфире — привязка снимается. */
void net_wifi_sta_set_target(wifi_config_t *sta, const uint8_t *bssid);

/* Сколько станций сейчас на нашей точке доступа (0 — точка доступа не поднята).
 * Пропуск на смену её настроек: новый пароль — только при ровно одной станции. */
uint8_t net_wifi_clients_count(void);

/* ── Сканирование эфира (net_scan.c) ──────────────────────────────────────────
 * Движок — единственный владелец радио: запросы API (wifi.scan) и поиск точки для
 * станции идут через него; каналы обходятся ПО ОДНОМУ, иначе сплошной скан снимает
 * точку доступа с эфира. Обход двигает таймер движка, а сервис лишь просит его
 * начать и смотрит net_scan_ready. Результаты лежат в кэше движка (ключ — BSSID):
 * их отдаёт net_scan_take, по ним ищет сеть станция (net_scan_find). */
uint8_t  net_scan_start(void);    /* 0 — не запустился (обход уже идёт) */
uint8_t  net_scan_ready(void);    /* 1 — обход закончен, результат ждёт net_scan_take */
uint8_t  net_scan_busy(void);     /* 1 — скан идёт */
void     net_scan_cancel(void);   /* погасить скан (вызов сняли) */
/* Сколько записей удалось забрать (не больше max); found — сколько нашлось за обход */
uint16_t net_scan_take(wifi_ap_record_t *out, uint16_t max, uint16_t *found);
/* Поиск сети в кэше движка: ssid обязателен, bssid — если точка выбрана руками.
 * 0 — сети нет или запись старше SCAN_CACHE_TTL_MS. */
uint8_t  net_scan_find(const char *ssid, const uint8_t *bssid,
                       wifi_ap_record_t *out, uint32_t *age_ms);

/* ── Учёт соединений (net_link.c) ───────────────────────────────────────────── */

/* Подписать соединение на обрыв линка его клиента: делегат получит событие, когда
 * клиент уйдёт (или получит другой адрес). 1 — запись нашлась, 0 — не от клиента AP. */
uint8_t net_link_subscribe(int sock, delegate_t *delegate);

/* Через какой интерфейс пришло соединение: локальный адрес сокета сравнивается с
 * адресами интерфейсов. По нему сокетный слой выбирает объект веб-сервера
 * (main/web_httpd.c). GW_IFACE_NONE — определить не удалось. */
uint32_t gw_iface_of_socket(int fd);

/* ── Адрес в тексте (connection.c) ──────────────────────────────────────────── */

/* "a.b.c.d" — в сетевой порядок байт; 0 — разобралось. */
uint8_t net_parse_ip4(const char *str, uint32_t *out);

/* Адрес — в строку out; пустой адрес даёт пустую строку. */
void net_format_ip4(uint32_t ip, char *out, size_t out_size);

/* ── Проводной интерфейс (только сборка для QEMU) ──────────────────────────── */
#ifdef CONFIG_QEMU_BUILD
void eth_init_openeth(void);
#endif

#ifdef __cplusplus
}
#endif
