/* app.c — точка сборки приложения: железо, сеть, веб-серверы интерфейсов,
 * порождённые ими транспорты, ядра API и сервисы. */

#include "app.h"
#include "platform.h"
#include "driver/gpio.h"
#include "web_httpd.h"
#include "net_iface.h"

#include <stdarg.h>

#define GW_LOG_MODULE GW_LOG_APP
#include "gw_log.h"

#define LED_D2_GPIO GPIO_NUM_2

/* ── Система для ядра API ───────────────────────────────────────────────────
 *
 * Ядро не знает ни FreeRTOS, ни ESP-IDF: от системы ему нужны только мьютекс,
 * таймер и журнал (api_system_t в api.h) — ни очередей, ни потоков в таблице
 * нет, такт даёт задача приложения (см. api_worker_task), а «подождать» значит
 * вернуть управление. Реализация — тем же platform.h: ядро и сервисы берут ОДИН
 * мьютекс и ОДНО время, иначе «состояние ядра под мьютексом» ничего не значило
 * бы. Контекст (context) — объект приложения, подставляется в app_main. */
static void *app_system_mutex_create(void *context)
{
    (void)context;
    return platform_mutex_create();
}

static void app_system_mutex_take(void *context, void *mutex)
{
    /* Результат не возвращается: мьютекс берётся без предела (критическая
     * секция — список указателей), а «не взялся» здесь — уже ошибка платформы. */
    (void)context;
    platform_mutex_take((platform_mutex_t)mutex, PLATFORM_WAIT_FOREVER);
}

static void app_system_mutex_give(void *context, void *mutex)
{
    (void)context;
    platform_mutex_give((platform_mutex_t)mutex);
}

/* Журнал ядра: строка уже собрана ядром, varargs раскладываем здесь. Уровни —
 * API_LOG_* (api.h), а не уровни ESP-IDF: ядро про него не знает. Информационные
 * строки ядра идут под тем же переключателем, что и логи служб (в обычной работе
 * их 50+ на старте); предупреждения и ошибки — всегда. */
static void app_system_log(void *context, int level, const char *tag, const char *fmt, ...)
{
    (void)context;
    if (level >= API_LOG_WARN) {
        char buf[160];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        platform_log_w(tag, "%s", buf);
        return;
    }
#if GW_LOG_API
    char buf[160];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (level >= API_LOG_INFO)
        platform_log_i(tag, "%s", buf);
    else
        platform_log_d(tag, "%s", buf);
#endif
}

/* Время ядру нужно в миллисекундах (ARG.RDL и период такта), а тик платформы —
 * в тиках FreeRTOS: перевод здесь, чтобы ядро не знало их частоту. */
static uint32_t app_system_time_ms(void *context)
{
    (void)context;
    return platform_ticks_to_ms(platform_tick());
}

/* Таблица системы одна на оба ядра: второй её экземпляр означал бы два разных
 * представления о времени в одном приложении. */
static api_system_t app_api_system = {
    .context      = NULL,
    .mutex_create = app_system_mutex_create,
    .mutex_take   = app_system_mutex_take,
    .mutex_give   = app_system_mutex_give,
    .time_ms      = app_system_time_ms,
    .log          = app_system_log,
};

/* ── Одна задача такта на все ядра ─────────────────────────────────────────
 *
 * Такт всем ядрам даёт ОДНА задача — ядра обслуживаются по очереди, по вызову
 * api_worker на каждое. Пауза — API_WORK_PERIOD_MS (api.h): иначе «такт ядра» и
 * «пауза вызывающего» разошлись бы. Стек 8192: такт зовёт обработчики сервисов
 * (Modbus, UART, системные), стек у них общий. */
static void api_worker_task(void *param)
{
    app_context_t *app = param;

    for (;;) {
        api_worker(app->api_ap);
        api_worker(app->api_sta);
        platform_delay_ms(API_WORK_PERIOD_MS);
    }
}

/* ── Активация WS-точки ────────────────────────────────────────────────────
 *
 * open транспорта — единственная операция, после которой точка принимает
 * соединения: до неё URI закреплён за объектом веб-сервера, но httpd о нём не
 * знает. */
static uint8_t ws_activate(const transport_t *transport)
{
    if ((transport == NULL) || (transport->iface == NULL) ||
        (transport->iface->open == NULL))
        return 0;
    return transport->iface->open(transport->context, NULL) == TRANSPORT_OK;
}

/* ── Что оказано интерфейсу ────────────────────────────────────────────────
 *
 * Набор путей у объектов всех интерфейсов один. Прошивки по HTTP нет: маршрута
 * /update нет ни в одном объекте, его заменила служба sys.fw.* (apih_fw.c) на
 * AP-ядре. Вшитая страница конфигурации (/config, /config.html) в объекте
 * станции не регистрируется вовсе: маршрута там нет, запрос попадает в общий
 * обработчик файлов, то есть означает обычный путь файловой системы.
 *
 * Порядок важен: частные пути — до общего обработчика-ловушки, иначе он
 * ответил бы за них. */
static void register_pages(web_server_t *srv, uint8_t with_config_page) {
    web_server_register_uri(srv, &modbus_alias);
    if (with_config_page) {
        web_server_register_uri(srv, &config_page);
        web_server_register_uri(srv, &config_page_html);
    }
    web_server_register_uri(srv, &file_server);
}

void app_main(void)
{
    static app_context_t app_context;

    /* Контекст системы — сам объект приложения (см. app_api_system): ядро
     * передаёт его обратно в каждый системный вызов. */
    app_api_system.context = &app_context;

    LOGI(TAG, "App start");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    load_config(&app_context);
#if CONFIG_WIFI_AP_ENABLE
    /* The default softAP secret is derived from the MAC, so it has to be
     * discoverable: printed here and shown on /config.html. */
    LOGI(TAG, "SoftAP '%s' password '%s'",
             app_context.ap_cnf.ap.ssid, app_context.ap_cnf.ap.password);
#endif

    gpio_reset_pin(LED_D2_GPIO);
    gpio_set_direction(LED_D2_GPIO, GPIO_MODE_OUTPUT);

    /* system HW */
    if(gw_uart_init(&(app_context.uart.port[0].desc) , GW_UART_PORT_0, GW_UART_RX_RING))
        LOGI("app", "Uart0 ok");
    if(gw_uart_init(&(app_context.uart.port[1].desc), GW_UART_PORT_1, GW_UART_RX_RING))
        LOGI("app", "Uart1 ok");
    if(gw_uart_init(&(app_context.uart.port[2].desc), GW_UART_PORT_2, GW_UART_RX_RING))
        LOGI("app", "Uart2 ok");

    setup_littlefs();

    /* Сеть: подписки на события соединений и скана, Wi-Fi и адреса интерфейсов.
     * Статические адреса должны стоять до того, как серверы начнут отвечать. */
    net_init(&app_context);

    /* Сокетный слой: по одному httpd на порт (CONFIG_WEB_SERVER_PORT для
     * страниц, CONFIG_WEB_SOCKET_SERVER_PORT для WebSocket). Привязать httpd к
     * интерфейсу нельзя, поэтому развод делает объект веб-сервера за портом.
     * Размеры пулов — из конфигурации проекта (main/Kconfig.projbuild). */
    static const web_httpd_config_t port_config[WEB_ROUTE_KINDS] = {
        [WEB_ROUTE_HTTP] = {
            .port             = CONFIG_WEB_SERVER_PORT,
            .max_open_sockets = CONFIG_WEB_SERVER_MAX_CLIENTS,
            .max_uri_handlers = 20,
            .send_budget_ms   = WEB_SEND_BUDGET_FILE_MS,
        },
        [WEB_ROUTE_WS] = {
            .port             = CONFIG_WEB_SOCKET_SERVER_PORT,
            .max_open_sockets = CONFIG_WEB_SOCKET_MAX_CLIENTS,
            .max_uri_handlers = 8,
            .send_budget_ms   = WEB_SEND_BUDGET_WS_MS,
        },
    };
    /* Пул сокетов lwIP один на всё: два listen-сокета httpd, их ctrl-каналы,
     * DHCP-сервер точки доступа и DNS. Если пулы портов его перерастут, httpd
     * начнёт молча не принимать новые соединения — поэтому проверяем при сборке.
     * Отдельно проверяем потолок ОДНОВРЕМЕННЫХ TCP-соединений (MAX_ACTIVE_TCP):
     * HTTP и WebSocket делят его, и при умолчании 16 три открытые вкладки уже не
     * оставляют места ни новой странице, ни WebSocket. */
    _Static_assert(CONFIG_WEB_SERVER_MAX_CLIENTS + CONFIG_WEB_SOCKET_MAX_CLIENTS + 4
                   <= CONFIG_LWIP_MAX_SOCKETS,
                   "пулы соединений не влезают в CONFIG_LWIP_MAX_SOCKETS");
    _Static_assert(CONFIG_WEB_SERVER_MAX_CLIENTS + CONFIG_WEB_SOCKET_MAX_CLIENTS + 2
                   <= CONFIG_LWIP_MAX_ACTIVE_TCP,
                   "пулы соединений не влезают в CONFIG_LWIP_MAX_ACTIVE_TCP");
    if (!web_httpd_start(port_config))
        ESP_LOGE(TAG, "Сокетный слой веб-сервера поднялся не полностью");

    /* Объект веб-сервера заводят до транспорта: другого способа его получить
     * нет — транспорт порождает сам объект (web_server_open_websocket). */
    app_context.web_ap  = web_server_open(GW_IFACE_AP);
    app_context.web_sta = web_server_open(GW_IFACE_STA);
#ifdef CONFIG_QEMU_BUILD
    app_context.web_eth = web_server_open(GW_IFACE_ETH);
#endif
    if (app_context.web_ap == NULL)
        ESP_LOGE(TAG, "Веб-сервер точки доступа не поднялся");
    if (app_context.web_sta == NULL)
        ESP_LOGE(TAG, "Веб-сервер станции не поднялся");

    LOGI(TAG, "Registering URI handlers");
    /* Страницы и файлы — в объект каждого интерфейса (см. register_pages);
     * вшитая страница конфигурации — только в объект точки доступа. */
    register_pages(app_context.web_ap, 1);
    register_pages(app_context.web_sta, 0);
#ifdef CONFIG_QEMU_BUILD
    register_pages(app_context.web_eth, 0);
#endif

    /* Транспорт порождает сам веб-сервер: интерфейс задан тем, кто его породил.
     * Объектов два — транспорта тоже два, у каждого своя точка и свой URI. */
    transport_t *ws_ap  = web_server_open_websocket(app_context.web_ap,  "/api");
    transport_t *ws_sta = web_server_open_websocket(app_context.web_sta, "/api");
    if (ws_ap == NULL)  ESP_LOGE(TAG, "WebSocket transport (AP) not created");
    if (ws_sta == NULL) ESP_LOGE(TAG, "WebSocket transport (STA) not created");

    /* Ядер два, потому что транспорта два: клиенты точки доступа и клиенты
     * станционной сети обслуживаются разными ядрами. Дескрипторы создаёт само
     * ядро (api_init) — он же и есть назначение обработчика транспорту: до
     * этого URI точки не активен, соединение не находит пути. */
    app_context.api_ap  = api_init(ws_ap,  &app_api_system);
    app_context.api_sta = api_init(ws_sta, &app_api_system);

    if (app_context.api_ap == NULL)
        ESP_LOGE(TAG, "API core (AP) not started");
    if (app_context.api_sta == NULL)
        ESP_LOGE(TAG, "API core (STA) not started");

    /* Последний шаг — активация URI: open включает точку. Без обоих ядер не
     * активируем: соединение пришло бы транспорту, который некому отдать.
     * HTTP-страницы от ядер не зависят. */
    if ((app_context.api_ap != NULL) && (app_context.api_sta != NULL)) {
        if (!ws_activate(ws_ap) || !ws_activate(ws_sta))
            ESP_LOGE(TAG, "WebSocket transport not started");
    } else {
        ESP_LOGE(TAG, "WebSocket transport not started");
    }

    /* Задача такта — одна на оба ядра (см. api_worker_task), и только когда есть
     * что обслуживать: у снятого ядра такт — пустой вызов. */
    if ((app_context.api_ap != NULL) && (app_context.api_sta != NULL))
        platform_task_create(api_worker_task, "api_work", 8192, &app_context, 5);

    LOGI(TAG, "Run");

    /* Сервисы регистрируют свои FID на конкретных ядрах (app.api_ap/app.api_sta).
     * Если ядро не поднялось, регистрировать некуда — задачи сервисов не
     * запускаем: они крутились бы вхолостую, а FID никому не оказаны. */
    if ((app_context.api_ap != NULL) && (app_context.api_sta != NULL)) {
        ret = ws_uart_run(&app_context);
        if (ret != ESP_OK)
            ESP_LOGE("app", "can't run ws_uart task: %s", esp_err_to_name(ret));

        if (ws_modbus_run(&app_context) != ESP_OK)
            LOGI("app", "can't run ws_modbus tasks");

        if (api_sys_run(&app_context) != ESP_OK)
            ESP_LOGE("app", "can't run system api task");
    } else {
        ESP_LOGE(TAG, "API cores not started, services left down");
    }

    uint32_t led_tick = 0;
    uint8_t led_state = 0;
    while (1) {
        if (++led_tick >= CONFIG_FREERTOS_HZ) {
            led_tick = 0;
            led_state ^= 1;
            gpio_set_level(LED_D2_GPIO, led_state);
        }
        /* Отдать процессор остальным задачам и холостой задаче (сброс WDT).
         * Один тик FreeRTOS — в миллисекундах, как их понимает платформа. */
        platform_delay_ms(platform_ticks_to_ms(1));
    }
}
