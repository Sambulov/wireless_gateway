/* Системный API: точка сборки службы.
 *
 * FID-ы (имена — в app.h): echo, wifi.sta/ap/mac/scan, sys.info/reboot/net,
 * sys.files, файловый менеджер sys.file.write/delete/rename/mkdir/rmdir и
 * прошивка sys.fw.begin/write/end/abort (она зарегистрирована только на AP-ядре).
 *
 * Каждый метод — обработчик, зарегистрированный в КОНКРЕТНОМ ядре API (у
 * прошивки их два: точка доступа и станция). Методы, которые меняют настройки,
 * зарегистрированы на ОБОИХ ядрах, но с разными контекстами регистрации
 * (sys_service_t): на AP-ядре — «администратор» (может менять), на STA-ядре —
 * «только чтение». Обработчик по своему контексту и понимает, что этому клиенту
 * можно; маски интерфейсов в ядре нет — «на каком ядре зарегистрирован»
 * и есть «через какой интерфейс пришёл».
 * Политика «по номеру сокета» здесь не нужна: сокет — дело транспорта.
 *
 * Ответ отправляется прямо из обработчика (api_reply): ядро само
 * оборачивает его в конверт {"FID":..,"SID":..,"ARG":<json>} и завершает вызов.
 * Ответы всегда несут "STA", коды — свои (SYS_* из apih_sys_internal.h): статус
 * службы ядру неизвестен, и отправлять его нечем — ядро само отвечает только за
 * приём, разбор и отсутствие обработчика. Своей очереди и задачи у системного
 * API нет: вызов идёт по тому же контракту, что у Modbus.
 *
 * Сами службы разложены по смыслу — этот файл их только собирает:
 *   apih_wifi.c   — wifi.sta, wifi.ap, wifi.mac, wifi.scan (Wi-Fi как таковой);
 *   apih_device.c — sys.info, sys.net, sys.reboot (состояние прибора, его адреса,
 *                   перезагрузка);
 *   apih_files.c  — sys.files и sys.file.* (файловый менеджер);
 *   apih_fw.c     — sys.fw.begin/write/end/abort (прошивка: только AP-ядро);
 *   apih_sys_common.c — общая обвязка: единый буфер ответа со сборщиком,
 *                   экземпляры контекста, политика, register_service.
 * В apih_sys.c остаются echo (проба самого ядра, ни к какой службе не
 * относится) и api_sys_run — порядок регистрации. */
#include "app.h"

#include "apih_sys_internal.h"

/* Проверочный echo: аргумент возвращается без изменений. Пустой запрос —
 * ответ "null". */
static uint8_t sys_echo_handler(void *call, void **context, uint8_t *data, uint32_t data_len) {
    (void)context;
    if (!call) return 0;
    if (data && data_len) api_reply(call, data, data_len);
    else                  api_reply(call, (const uint8_t *)"null", 4);
    return 0;
}

esp_err_t api_sys_run(app_context_t *app) {
    /* Общее состояние службы (буфер сборщика ответов и экземпляры контекста)
     * готовится ДО первой регистрации — подробности в apih_sys_common_init. */
    apih_sys_common_init(app);

    /* Каждый регистратор отвечает за свои методы на обоих ядрах и возвращает
     * ESP_FAIL, если хоть одна его регистрация не прошла: общая ok-переменная
     * не даёт службе объявить себя готовой наполовину. */
    uint8_t ok = 1;
    ok &= (apih_wifi_run(app)   == ESP_OK);
    ok &= (apih_device_run(app) == ESP_OK);
    ok &= (apih_files_run(app)  == ESP_OK);
    ok &= (apih_fw_run(app)     == ESP_OK);

    /* echo — единственный метод без службы: это проба самого ядра, поэтому и
     * регистрируется здесь, без политики и без контекста, по экземпляру на
     * каждом ядре. */
    ok &= register_service(app->api_ap,  sys_echo_handler, ESP_WS_API_ECHO_ID, NULL);
    ok &= register_service(app->api_sta, sys_echo_handler, ESP_WS_API_ECHO_ID, NULL);

    if (!ok) return ESP_FAIL;

    ESP_LOGI(TAG, "System API ready (echo, wifi.sta, wifi.ap, wifi.mac, wifi.scan, "
                  "sys.info, sys.reboot, sys.files, sys.file.write/delete/rename, sys.net, "
                  "sys.fw.begin/write/end/abort)");
    return ESP_OK;
}
