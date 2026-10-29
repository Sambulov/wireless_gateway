/* Системный API: точка сборки службы (имена FID-ов — в app.h).
 *
 * Каждый метод зарегистрирован в КОНКРЕТНОМ ядре API, а ядер два: точка доступа и
 * станция. Методы, которые меняют настройки, есть на ОБОИХ ядрах, но с разными
 * контекстами регистрации (sys_service_t): на AP-ядре «администратор», на
 * STA-ядре «только чтение». Маски интерфейсов в ядре нет — «на каком ядре
 * зарегистрирован» и есть «через какой интерфейс пришёл».
 *
 * Ответ отправляется прямо из обработчика (api_call_send_arg): ядро само оборачивает его
 * в конверт {"FID":..,"SID":..,"ARG":<json>} и завершает вызов. Ответы всегда
 * несут "STA" (коды SYS_* из apih_sys_internal.h). Своей очереди и своей задачи у
 * системного API нет.
 *
 * Службы разложены по смыслу по файлам apih_wifi/device/files/fw.c и
 * apih_sys_common.c; здесь остаются echo и порядок регистрации.
 */
#include "app.h"

#include "apih_sys_internal.h"

#define GW_LOG_MODULE GW_LOG_API
#include "gw_log.h"

/* проверочный echo: аргумент возвращается без изменений, пустой запрос — "null" */
static uint8_t sys_echo_handler(void *call, void **context, uint8_t *data, uint32_t data_len) {
    (void)context;
    if (!call) return HDL_FALSE;
    if (data && data_len) api_call_send_arg(call, (const char *)data, data_len);
    else                  api_call_send_arg(call, "null", 4);
    return HDL_FALSE;
}

esp_err_t api_sys_run(app_context_t *app) {
    /* общее состояние службы (буфер сборщика ответов и экземпляры контекста)
     * готовится ДО первой регистрации — подробности в apih_sys_common_init */
    apih_sys_common_init(app);

    /* каждый регистратор возвращает ESP_FAIL, если хоть одна его регистрация не
     * прошла: общая ok-переменная не даёт службе объявить себя готовой наполовину */
    uint8_t ok = 1;
    ok &= (apih_wifi_run(app)   == ESP_OK);
    ok &= (apih_device_run(app) == ESP_OK);
    ok &= (apih_files_run(app)  == ESP_OK);
    ok &= (apih_fw_run(app)     == ESP_OK);

    /* echo — единственный метод без службы: это проба самого ядра, поэтому и
     * регистрируется здесь, без политики и без контекста, по экземпляру на
     * каждом ядре */
    ok &= register_service(app->api_ap,  sys_echo_handler, ESP_WS_API_ECHO_ID, NULL);
    ok &= register_service(app->api_sta, sys_echo_handler, ESP_WS_API_ECHO_ID, NULL);

    if (!ok) return ESP_FAIL;

    LOGI(TAG, "System API ready (echo, wifi.sta, wifi.ap, wifi.mac, wifi.scan, "
                  "sys.info, sys.reboot, sys.files, sys.file.write/delete/rename, sys.net, "
                  "sys.fw.begin/write/end/abort)");
    return ESP_OK;
}
