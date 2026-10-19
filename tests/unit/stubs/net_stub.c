#include "app.h"
#include "net_stub.h"

/* Сеть на хосте: радио нет, поэтому счётчик станций точки доступа — значение,
 * которое задаёт тест. По умолчанию 1: «администратор на точке один» — обычное
 * рабочее условие, и большинство случаев проверяет не его. */
static uint8_t stub_clients = 1;

uint8_t net_wifi_clients_count(void) { return stub_clients; }

void net_stub_set_clients(uint8_t clients) { stub_clients = clients; }

void net_stub_reset(void) { stub_clients = 1; }

/* esp_wifi_get_mac зовёт только mac_runtime (apih_sys_common.c). Службы,
 * которые её спрашивают (sys.info, wifi.mac), в хостовой сборке не собираются,
 * а ссылка из объектного файла обязана разрешиться: отказ драйвера — честный
 * ответ «адреса нет». */
esp_err_t esp_wifi_get_mac(wifi_interface_t iface, uint8_t *mac) {
    (void)iface; (void)mac;
    return ESP_FAIL;
}
