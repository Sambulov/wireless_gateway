/* Общая обвязка системных сервисов — то, что заводится ОДИН раз на прибор:
 * единый буфер ответа со сборщиком, экземпляры контекста службы и
 * register_service. Обработчиков здесь нет: службы лежат по смыслу в
 * apih_wifi.c, apih_device.c и apih_files.c, точка сборки — apih_sys.c.
 */
#include <stdio.h>
#include <string.h>

#include "esp_mac.h"

#include "apih_sys_internal.h"

/* буфер ответа и сборщик — те самые, что объявлены в apih_sys_internal.h:
 * копии быть не должно */
static char       sys_resp_buf[SYS_RESP_MAX];
gw_jsonw_t        sys_w;

/* экземпляры контекста: поля заполняет apih_sys_common_init */
sys_service_t sys_svc_ap;
sys_service_t sys_svc_ro;
sys_service_t sys_svc_mut;

void resp_begin(void) {
    gw_jsonw_reset(&sys_w);
    gw_jsonw_obj(&sys_w, NULL);
}

/* короткий отказ для страницы конфигурации: STA, поле, текст */
void resp_error(const char *sta, const char *err, const char *msg) {
    gw_jsonw_str(&sys_w, "STA", sta);
    if (err) gw_jsonw_str(&sys_w, "ERR", err);
    if (msg) gw_jsonw_str(&sys_w, "MSG", msg);
}

void resp_ok(void) {
    resp_error(SYS_OK, NULL, NULL);
}

void send_response(void *call) {
    if (!gw_jsonw_ok(&sys_w)) {
        ESP_LOGE(TAG, "Ответ не влез в %d байт — отвечаю отказом", SYS_RESP_MAX);
        resp_begin();
        resp_error(SYS_NO_MEM, "NOMEM", NULL);
    }
    size_t len = gw_jsonw_finish(&sys_w);
    if (!api_call_send_arg(call, (const char *)sys_resp_buf, (uint32_t)len))
        ESP_LOGW(TAG, "Ответ клиенту не ушёл");
}

uint8_t config_allowed(const sys_service_t *svc, uint8_t clients) {
    return svc->from_ap && (clients == 1);
}

uint8_t change_allowed(const sys_service_t *svc, uint8_t clients) {
    return svc->may_change && config_allowed(svc, clients);
}

void resp_denied(const sys_service_t *svc, uint8_t clients) {
    if (svc->from_ap)
        resp_error(SYS_NO_ACCESS, "AP_CLIENTS",
                   "Exactly one client must be connected to the softAP");
    else
        resp_error(SYS_NO_ACCESS, "AP_ONLY",
                   "Wi-Fi and network settings can only be changed from the softAP");
    gw_jsonw_int(&sys_w, "CLIENTS", clients);
}

const sys_service_t *svc_of(void **context) {
    return (context != NULL) ? (const sys_service_t *)*context : NULL;
}

uint8_t register_service(api_t *api, api_handler_t handler, const char *fid,
                         void *context) {
    if (api_call_register(api, handler, fid, context)) return 1;
    ESP_LOGE(TAG, "Не удалось зарегистрировать %s (ядро %p)", fid, (void *)api);
    return 0;
}

/* разбор и печать MAC-адреса живут здесь, а не в apih_wifi.c: печать нужна и
 * Wi-Fi, и службе прибора (sys.info), а формат у них обязан быть один. */
static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

uint8_t mac_parse(const char *s, uint8_t *out) {
    uint8_t nib = 0;
    if (!s) return 0;
    for (; *s; s++) {
        if (*s == ':' || *s == '-' || *s == '.' || *s == ' ') continue;
        int v = hex_nibble(*s);
        if (v < 0 || nib >= 12) return 0;
        if ((nib & 1) == 0) out[nib >> 1] = (uint8_t)(v << 4);
        else                out[nib >> 1] |= (uint8_t)v;
        nib++;
    }
    return nib == 12;
}

void mac_text(const uint8_t *mac, char *out, size_t out_size) {
    snprintf(out, out_size, MACSTR, MAC2STR(mac));
}

/* Текущий адрес интерфейса спрашиваем у драйвера, а не у своих записей: после
 * перезагрузки истина только там, а наши записи — это «что заказано». */
void mac_runtime(wifi_interface_t iface, const uint8_t *factory, char *out, size_t out_size) {
    uint8_t mac[6];
    if (esp_wifi_get_mac(iface, mac) == ESP_OK) mac_text(mac, out, out_size);
    else                                        mac_text(factory, out, out_size);
}

void apih_sys_common_init(app_context_t *app) {
    /* сборщик должен знать свой буфер до первого ответа: иначе он считает себя
     * переполненным и на КАЖДЫЙ запрос отвечает отказом */
    gw_jsonw_init(&sys_w, sys_resp_buf, sizeof(sys_resp_buf));

    /* один и тот же контекст описывает «кто спрашивает»: приложение, интерфейс
     * (из точки доступа или нет) и что этому клиенту можно */
    sys_svc_ap.app      = app; sys_svc_ap.from_ap      = 1; sys_svc_ap.may_change      = 1;
    sys_svc_ro.app      = app; sys_svc_ro.from_ap      = 0; sys_svc_ro.may_change      = 0;
    sys_svc_mut.app     = app; sys_svc_mut.from_ap     = 0; sys_svc_mut.may_change     = 1;
}
