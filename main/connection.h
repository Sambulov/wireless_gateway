#pragma once

#include <esp_wifi.h>
#include "CodeLib.h"
#include "ws_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Интерфейсы, к которым привязываются сервисы, описаны в ws_transport.h:
 * GW_IFACE_NONE / GW_IFACE_AP / GW_IFACE_STA / GW_IFACE_ETH и маска
 * GW_IFACE_MASK(iface) для регистрации (bApiCallRegister). */

/* Поднимает Wi-Fi и применяет ЗАКАЗАННЫЕ MAC адреса интерфейсов: mac_sta и
 * mac_ap — по шесть байт, NULL или все нули значат «заводской». Драйвер
 * принимает новый адрес только на остановленном интерфейсе, поэтому подмена
 * делается здесь — между esp_wifi_init и esp_wifi_start. */
void wifi_init_ap_sta(wifi_config_t *ap_cnf, wifi_config_t *sta_cnf,
                      const uint8_t *mac_sta, const uint8_t *mac_ap,
                      const uint8_t *sta_bssid);

/* Выбранная пользователем точка доступа (BSSID): вписывает её в настройки
 * станции. bssid пуст — привязки нет, и тогда станция обязана брать СИЛЬНЕЙШУЮ
 * точку с этим именем (all-channel scan + сортировка по уровню), а не первую
 * попавшуюся, как делает fast scan. Если точка выбрана, а её нет в эфире,
 * обработчик событий через несколько попыток снимает привязку сам. */
void wifi_sta_set_target(wifi_config_t *sta, const uint8_t *bssid);

/* ── Сканирование эфира ───────────────────────────────────────────────────────
 *
 * Запуск неблокирующий: список сетей драйвер собирает сам, а признак готовности
 * ставит событие WIFI_EVENT_SCAN_DONE (см. wifi_event_handler). Обработчик API
 * вызывается и в такте работы, поэтому ждать внутри нельзя: сервис wifi.scan
 * запускает скан и в следующих тактах спрашивает состояние.
 *
 * Забрать результат можно один раз: wifi_scan_take освобождает список драйвера. */
uint8_t  wifi_scan_start(void);   /* 0 — не запустился (уже идёт или идёт подключение) */
uint8_t  wifi_scan_ready(void);   /* 1 — результат готов, его ждёт wifi_scan_take */
uint8_t  wifi_scan_busy(void);    /* 1 — скан идёт */
void     wifi_scan_cancel(void);  /* погасить скан (вызов сняли) */
/* Сколько записей удалось забрать (не больше max); found — сколько нашлось всего */
uint16_t wifi_scan_take(wifi_ap_record_t *out, uint16_t max, uint16_t *found);

#ifdef CONFIG_QEMU_BUILD
void eth_init_openeth(void);
#endif

//esp_netif_t* pxGetNetIfFromSocket(int sock);
//uint8_t get_client_ip4(int sock, esp_ip4_addr_t *out_addr);
uint8_t socket_link_subscribe(int sock, delegate_t *delegate);

/* Через какой сетевой интерфейс пришло соединение: локальный адрес сокета
 * сравнивается с адресами интерфейсов. Спрашивается один раз при установлении
 * соединения (транспорт запоминает ответ в дескрипторе) — на каждом событии
 * сеть не дёргается. GW_IFACE_NONE — определить не удалось. */
uint32_t gw_iface_of_socket(int fd);

#ifdef __cplusplus
}
#endif
