/* Общая обвязка системных сервисов: объявления и коды ответов для apih_wifi.c,
 * apih_device.c, apih_files.c и apih_fw.c. Сами объекты (экземпляры контекста
 * службы, буфер ответа со сборщиком, register_service) заведены ОДИН раз в
 * apih_sys_common.c: копий быть не должно, иначе одни и те же FID-ы отвечали бы
 * по-разному на одном приборе. Сборщик ответа общий сознательно: обработчики ядра
 * API гоняет ОДНА задача (api_worker_task в app.c — по одному вызову за такт), так
 * что двух одновременно собираемых ответов не бывает.
 */
#ifndef APIH_SYS_INTERNAL_H
#define APIH_SYS_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#ifdef APIH_HOST_TEST
/* хостовые тесты собирают apih_fw.c и apih_sys_common.c без ESP-IDF: им нужна
 * тень app.h, а не сама прошивка */
#  include "apih_sys_app_types.h"
#else
#  include "app.h"
#endif
#include "api.h"
#include "GwJson.h"

/* коды ответов этой службы (STA): те же, что в api.h, но текстом — страница
 * показывает их как есть */
#define SYS_OK          "0x00000000"
#define SYS_BAD_ARG     "0x80000003"
#define SYS_NO_MEM      "0x80000006"
#define SYS_NO_ACCESS   "0x80000007"
#define SYS_BUSY        "0x80000008"
#define SYS_TIMEOUT     "0x80000009"
#define SYS_INTERNAL    "0x8000000f"

/* токенов на разбор ARG запросов конфигурации: несколько строковых полей */
#define SYS_ARG_MAX_TOKENS 24

/* "aa:bb:cc:dd:ee:ff" — 17 символов и нуль. Этим форматом адреса печатают и
 * Wi-Fi, и sys.info. */
#define MAC_TEXT_LEN 18

/* ── Экземпляры сервиса ─────────────────────────────────────────────────── *
 *
 * Контекст регистрации: по нему обработчик понимает, какой транспорт его позвал и
 * что этому клиенту можно. Один и тот же обработчик регистрируется по разу на каждом
 * ядре — с разными контекстами.
 *
 * Wi-Fi и сетевые настройки меняет только администратор: клиент точки доступа, и она
 * единственный клиент на радио (пока клиентов двое, не может никто — второй мог бы
 * подглядывать). То же условие решает, показывать ли пароли. «Ровно один клиент»
 * берётся с СЕТЕВОГО ДРАЙВЕРА (net_wifi_clients_count), а не у транспорта.
 */
typedef struct {
    app_context_t *app;
    uint8_t from_ap;     /* вызов пришёл через AP-транспорт (ядро точки доступа) */
    uint8_t may_change;  /* ему разрешено менять состояние (настройки, перезагрузка) */
} sys_service_t;

/* ядро точки доступа: администратор рядом с прибором — менять можно */
extern sys_service_t sys_svc_ap;
/* ядро станции: только чтение */
extern sys_service_t sys_svc_ro;
/* перезагрузка — не настройка, она доступна с любого интерфейса (кнопка на странице
 * не зависит от CAN_CONFIGURE), поэтому на STA-ядре у неё свой контекст: менять
 * можно, но клиент не на точке доступа. Сузить её до точки доступа — зарегистрировать
 * на STA-ядре sys_svc_ro: обработчик ответит AP_ONLY. */
extern sys_service_t sys_svc_mut;

/* ── Ответ клиенту ──────────────────────────────────────────────────────── *
 *
 * Ответ собирается прямо в буфер, без дерева и без узлов: на каждый объект, ключ и
 * значение память не выделяется. Буфер статический, а не на стеке: обработчики
 * обслуживаются по одному, а стек задачи ядра API 8 КБ, тогда как листинг каталога
 * бывает в килобайты (~290 байт на запись). Не влезло — отвечаем NOMEM, за границу
 * не пишем; копию готового текста в конверт ответа делает ядро (api_reply). Объявлен
 * здесь, потому что обработчики служб дописывают поля в него напрямую. */
#define SYS_RESP_MAX 8192
extern gw_jsonw_t sys_w;

/* корень ответа — объект; дальше обработчик дописывает поля */
void resp_begin(void);
void resp_error(const char *sta, const char *err, const char *msg);
void resp_ok(void);
/* отказ «этому клиенту менять нельзя» с указанием, чего именно не хватает */
void resp_denied(const sys_service_t *svc, uint8_t clients);
/* Отдать ответ клиенту. Буфер сборщика остаётся у службы, а ядро получает
 * готовый текст и само кладёт его в конверт вызова ({"FID":..,"SID":..,
 * "ARG":<json>}), после чего завершает вызов. */
void send_response(void *call);

/* Условия на самом приборе: администратор — клиент точки доступа, и он там
 * один. От интерфейса (регистрации) это не зависит. */
uint8_t config_allowed(const sys_service_t *svc, uint8_t clients);
/* Можно ли этому клиенту менять состояние: экземпляр сервиса это позволяет
 * (регистрация на интерфейс) и условия на приборе выполнены. */
uint8_t change_allowed(const sys_service_t *svc, uint8_t clients);

/* Контекст регистрации; у методов без политики (echo, sys.files) его нет — NULL */
const sys_service_t *svc_of(void **context);

/* Регистрация экземпляра сервиса: что за метод, на каком ядре он оказан и с
 * каким контекстом. Этим и выражена политика (см. sys_service_t). */
uint8_t register_service(api_t *api, api_handler_t handler, const char *fid,
                         void *context);

/* "aa:bb:cc:dd:ee:ff", "aa-bb-…", "aabb.ccdd.eeff" или просто 12 hex-цифр.
 * 0 — разобрать не удалось (в out при этом ничего осмысленного). */
uint8_t mac_parse(const char *s, uint8_t *out);
void    mac_text(const uint8_t *mac, char *out, size_t out_size);
void mac_runtime(wifi_interface_t iface, const uint8_t *factory, char *out, size_t out_size);

/* Подготовить общее состояние службы (буфер сборщика ответов и экземпляры
 * контекста). Зовётся ОДИН раз из api_sys_run, до первой регистрации. */
void apih_sys_common_init(app_context_t *app);

/* Регистраторы: каждый файл службы регистрирует свои FID на обоих ядрах и
 * возвращает ESP_OK, только если ВСЕ его регистрации прошли (это решает одна
 * общая ok-переменная). */
esp_err_t apih_wifi_run(app_context_t *app);    /* wifi.sta/ap/mac/scan */
esp_err_t apih_device_run(app_context_t *app);  /* sys.info, sys.net, sys.reboot */
esp_err_t apih_files_run(app_context_t *app);   /* sys.files, sys.file.* */
/* только AP-ядро: прошивка со станции не принимается (см. apih_fw.c) */
esp_err_t apih_fw_run(app_context_t *app);

#endif /* APIH_SYS_INTERNAL_H */
