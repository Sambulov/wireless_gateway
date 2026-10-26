/* app.c — точка сборки приложения: железо, сеть, веб-серверы интерфейсов,
 * порождённые ими транспорты, ядра API и сервисы. */

#include "app.h"
#include "platform.h"
#include "driver/gpio.h"
#include "web_httpd.h"
#include "net_iface.h"

#include <stdarg.h>

#define LED_D2_GPIO GPIO_NUM_2

/* ── Система для ядра API ───────────────────────────────────────────────────
 *
 * Ядро не знает ни FreeRTOS, ни ESP-IDF: всё, что ему нужно от системы, оно
 * получает этой таблицей (api_system_t в api.h) — и просит РОВНО три вещи:
 * мьютекс, чтение таймера и журнал. Ни очередей, ни задержек, ни создания
 * потоков в таблице нет: ядро их не просит и само не заводит, такт ему даёт
 * задача приложения (см. api_worker_task), а «подождать» для него значит
 * вернуть управление.
 *
 * Реализация — тем же platform.h, что у приложения и сервисов: ядро и сервисы
 * берут ОДИН мьютекс и ОДНО время, иначе «состояние ядра под мьютексом» ничего
 * не значило бы.
 *
 * Контекст системы (context) — объект приложения: он подставляется в таблицу
 * ниже, в app_main. Функции его пока не используют (платформа в прошивке — это
 * набор свободных функций), но граница проведена честно: система с состоянием
 * получит его здесь, и ядру для этого меняться не придётся. */
static void *app_system_mutex_create(void *context)
{
    (void)context;
    return platform_mutex_create();
}

static void app_system_mutex_take(void *context, void *mutex)
{
    /* Результат не возвращается: ядро берёт мьютекс без предела (критическая
     * секция — список указателей, ждать дольше нечего), и «не взялся» здесь —
     * уже ошибка платформы, разбирать её ядру нечем. */
    (void)context;
    platform_mutex_take((platform_mutex_t)mutex, PLATFORM_WAIT_FOREVER);
}

static void app_system_mutex_give(void *context, void *mutex)
{
    (void)context;
    platform_mutex_give((platform_mutex_t)mutex);
}

/* Журнал ядра. Ядро присылает уже собранную строку ("%s" + буфер), но таблица
 * принимает varargs — раскладываем их здесь. Уровни — API_LOG_* (api.h), а не
 * уровни ESP-IDF: ядро про него не знает. */
static void app_system_log(void *context, int level, const char *tag, const char *fmt, ...)
{
    (void)context;
    char buf[160];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    if (level >= API_LOG_WARN)
        platform_log_w(tag, "%s", buf);
    else if (level >= API_LOG_INFO)
        platform_log_i(tag, "%s", buf);
    else
        platform_log_d(tag, "%s", buf);
}

/* Время ядру нужно в миллисекундах (так задан ARG.RDL и период такта), а тик
 * платформы — в тиках FreeRTOS, как у xTaskGetTickCount(). Перевод здесь, чтобы
 * ядро не знало ни про тики, ни про их частоту. */
static uint32_t app_system_time_ms(void *context)
{
    (void)context;
    return platform_ticks_to_ms(platform_tick());
}

/* Таблица системы одна на оба ядра: мьютекс и время — платформенные объекты, а
 * не состояние ядра, и второй экземпляр таблицы означал бы два разных
 * представления о времени в одном приложении. Контекст заполняется в app_main
 * (объект приложения создаётся там). */
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
 * Ядро потоков не заводит — такт ему даёт приложение, и задача для этого ОДНА:
 * ядра обслуживаются по очереди, по вызову api_worker на каждое, и общего у них
 * ничего нет (дескрипторы разные, состояние своё). Пауза между тактами —
 * API_WORK_PERIOD_MS (объявлен в api.h): иначе «такт ядра» и «пауза
 * вызывающего» разошлись бы.
 *
 * Стек 8192: такт зовёт обработчики сервисов (Modbus, UART, системные), и стек
 * у них общий.
 *
 * Задача живёт до перезагрузки: останавливать её нечем и незачем. */
static void api_worker_task(void *param)
{
    app_context_t *app = param;
    /* ВРЕМЕННО (замер): сколько тактов в секунду и сколько длится самый долгий. */
    TickType_t stat_win = xTaskGetTickCount();
    uint32_t stat_n = 0, stat_max = 0;

    for (;;) {
        TickType_t t0 = xTaskGetTickCount();
        api_worker(app->api_ap);
        api_worker(app->api_sta);
        uint32_t dur = (uint32_t)(xTaskGetTickCount() - t0);
        if(dur > stat_max) stat_max = dur;
        stat_n++;
        TickType_t now = xTaskGetTickCount();
        if((uint32_t)(now - stat_win) >= 1000) {
            ESP_LOGW("stat", "тактов/с: %lu, самый долгий: %lu мс",
                     (unsigned long)stat_n, (unsigned long)stat_max);
            stat_n = 0; stat_max = 0; stat_win = now;
        }
        platform_delay_ms(API_WORK_PERIOD_MS);
    }
}

/* ── Активация WS-точки ────────────────────────────────────────────────────
 *
 * open транспорта — единственная операция, после которой точка начинает
 * принимать соединения: до неё URI закреплён за объектом веб-сервера, но httpd о
 * нём не знает. Ядро к этому моменту уже назначено (api_init), поэтому первое же
 * соединение находит подписчика. */
static uint8_t ws_activate(const transport_t *transport)
{
    if ((transport == NULL) || (transport->iface == NULL) ||
        (transport->iface->open == NULL))
        return 0;
    return transport->iface->open(transport->context, NULL) == TRANSPORT_OK;
}

/* ── Что оказано интерфейсу ────────────────────────────────────────────────
 *
 * Страницы, карты регистров, терминал и файлы доступны отовсюду: набор путей у
 * объектов всех интерфейсов один. Исключений два, и заданы они по-разному.
 *
 * Прошивки по HTTP нет: маршрута /update не существует ни в одном объекте, его
 * заменила служба sys.fw.* (apih_fw.c), зарегистрированная только на AP-ядре.
 *
 * Вшитая страница конфигурации (/config, /config.html) в объекте станции не
 * регистрируется вовсе. Маршрута там нет, поэтому запрос попадает в общий
 * обработчик файлов и означает обычный путь файловой системы: нет файла —
 * 404, есть — отдаётся. Никакой маски для этого не нужно: отсутствие маршрута
 * и есть ответ.
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
     * передаёт его обратно в каждый системный вызов, а что в нём лежит —
     * дело приложения. */
    app_api_system.context = &app_context;

    ESP_LOGI(TAG, "App start");

    //NOTE: just a test to check component build system. Delete it as soon as possible

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    load_config(&app_context);
#if CONFIG_WIFI_AP_ENABLE
    /* The default softAP secret is derived from the MAC, so it has to be
     * discoverable: it is printed here and shown on /config.html. */
    ESP_LOGI(TAG, "SoftAP '%s' password '%s'",
             app_context.ap_cnf.ap.ssid, app_context.ap_cnf.ap.password);
#endif

    gpio_reset_pin(LED_D2_GPIO);
    gpio_set_direction(LED_D2_GPIO, GPIO_MODE_OUTPUT);

    /* system HW */
    if(gw_uart_init(&(app_context.uart.port[0].desc) , GW_UART_PORT_0, GW_UART_RX_RING))
        ESP_LOGI("app", "Uart0 ok");
    if(gw_uart_init(&(app_context.uart.port[1].desc), GW_UART_PORT_1, GW_UART_RX_RING))
        ESP_LOGI("app", "Uart1 ok");
    if(gw_uart_init(&(app_context.uart.port[2].desc), GW_UART_PORT_2, GW_UART_RX_RING))
        ESP_LOGI("app", "Uart2 ok");

    setup_littlefs();

    /* Сеть: подписки на события соединений и скана, Wi-Fi (в сборке для QEMU —
     * проводной интерфейс) и адреса интерфейсов. Статические адреса должны
     * стоять до того, как серверы начнут на них отвечать. */
    net_init(&app_context);

    /* Сокетный слой: по одному httpd на порт. Порты для клиентов не меняются —
     * страницы на CONFIG_WEB_SERVER_PORT, WebSocket на
     * CONFIG_WEB_SOCKET_SERVER_PORT: привязать httpd к интерфейсу нельзя, поэтому
     * развод по интерфейсу делает не порт, а объект веб-сервера за ним. */
    static const web_httpd_config_t port_config[WEB_ROUTE_KINDS] = {
        [WEB_ROUTE_HTTP] = {
            .port             = CONFIG_WEB_SERVER_PORT,
            .max_open_sockets = 12,
            .max_uri_handlers = 20,
            .send_budget_ms   = WEB_SEND_BUDGET_FILE_MS,
        },
        [WEB_ROUTE_WS] = {
            .port             = CONFIG_WEB_SOCKET_SERVER_PORT,
            .max_open_sockets = 8,
            .max_uri_handlers = 8,
            .send_budget_ms   = WEB_SEND_BUDGET_WS_MS,
        },
    };
    if (!web_httpd_start(port_config))
        ESP_LOGE(TAG, "Сокетный слой веб-сервера поднялся не полностью");

    /* Объект веб-сервера — на каждый интерфейс. Его заводят до транспорта:
     * транспорт порождает сам объект (web_server_open_websocket), и другого
     * способа его получить нет. */
    app_context.web_ap  = web_server_open(GW_IFACE_AP);
    app_context.web_sta = web_server_open(GW_IFACE_STA);
#ifdef CONFIG_QEMU_BUILD
    app_context.web_eth = web_server_open(GW_IFACE_ETH);
#endif
    if (app_context.web_ap == NULL)
        ESP_LOGE(TAG, "Веб-сервер точки доступа не поднялся");
    if (app_context.web_sta == NULL)
        ESP_LOGE(TAG, "Веб-сервер станции не поднялся");

    ESP_LOGI(TAG, "Registering URI handlers");
    /* Страницы и файлы — в объект каждого интерфейса (см. register_pages);
     * вшитая страница конфигурации — только в объект точки доступа. */
    register_pages(app_context.web_ap, 1);
    register_pages(app_context.web_sta, 0);
#ifdef CONFIG_QEMU_BUILD
    register_pages(app_context.web_eth, 0);
#endif

    /* Транспорт порождает сам веб-сервер: интерфейс задан тем, кто его породил,
     * и отдельно не называется. Объектов два — значит и транспорта два, и
     * делить им нечего: у каждого своя точка, свой URI и своё будущее ядро. */
    transport_t *ws_ap  = web_server_open_websocket(app_context.web_ap,  "/api");
    transport_t *ws_sta = web_server_open_websocket(app_context.web_sta, "/api");
    if (ws_ap == NULL)  ESP_LOGE(TAG, "WebSocket transport (AP) not created");
    if (ws_sta == NULL) ESP_LOGE(TAG, "WebSocket transport (STA) not created");

    /* Ядер два, потому что транспорта два: клиенты точки доступа и клиенты
     * станционной сети обслуживаются разными ядрами. Дескрипторы создаёт само
     * ядро (api_init), здесь остаются только указатели: память ядра принадлежит
     * ядру и живёт до перезагрузки.
     *
     * api_init — это и есть назначение обработчика транспорту: ядро подписывается
     * на его события. До этого момента URI точки не активен, и пришедшее
     * соединение просто не находит пути.
     *
     * Система у обоих ядер одна и та же (app_api_system): мьютекс и время —
     * платформенные объекты, а не состояние ядра, и второй экземпляр таблицы
     * означал бы два разных представления о времени в одном приложении. */
    app_context.api_ap  = api_init(ws_ap,  &app_api_system);
    app_context.api_sta = api_init(ws_sta, &app_api_system);

    if (app_context.api_ap == NULL)
        ESP_LOGE(TAG, "API core (AP) not started");
    if (app_context.api_sta == NULL)
        ESP_LOGE(TAG, "API core (STA) not started");

    /* Последний шаг — активация URI: open у транспорта включает точку, и только
     * теперь к ней можно подключиться. Без обоих ядер не активируем: соединение
     * пришло бы транспорту, который некому отдать. HTTP-страницы от ядер не
     * зависят — они зарегистрированы выше. */
    if ((app_context.api_ap != NULL) && (app_context.api_sta != NULL)) {
        if (!ws_activate(ws_ap) || !ws_activate(ws_sta))
            ESP_LOGE(TAG, "WebSocket transport not started");
    } else {
        ESP_LOGE(TAG, "WebSocket transport not started");
    }

    /* Задача такта — одна на оба ядра (см. api_worker_task): сами ядра потоков
     * не заводят. Поднимаем её только когда есть что обслуживать: у снятого
     * ядра такт — пустой вызов, и крутить его незачем. */
    if ((app_context.api_ap != NULL) && (app_context.api_sta != NULL))
        platform_task_create(api_worker_task, "api_work", 8192, &app_context, 5);

    ESP_LOGI(TAG, "Run");

    /* Сервисы регистрируют свои FID на конкретных ядрах (app.api_ap/app.api_sta).
     * Если ядро не поднялось, регистрировать некуда: задачи сервисов не
     * запускаем вовсе, иначе они крутились бы вхолостую, а их FID не были бы
     * никому оказаны. */
    if ((app_context.api_ap != NULL) && (app_context.api_sta != NULL)) {
        ret = ws_uart_run(&app_context);
        if (ret != ESP_OK)
            ESP_LOGE("app", "can't run ws_uart task: %s", esp_err_to_name(ret));

        if (ws_modbus_run(&app_context) != ESP_OK)
            ESP_LOGI("app", "can't run ws_modbus tasks");

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
