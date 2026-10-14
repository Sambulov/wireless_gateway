#include <esp_log.h>
#include "esp_littlefs.h"
#include <esp_http_server.h>
#include <esp_wifi.h>
#include <errno.h>
#include <sys/socket.h>
#include <lwip/sockets.h>

#include "app.h"

httpd_handle_t server = NULL;

/* ── Отправка без бесконечного ожидания ───────────────────────────────────────
 *
 * esp_http_server обслуживает все сессии одного экземпляра ОДНОЙ задачей, а её
 * штатная отправка блокирующая: если клиент запросил файл и перестал читать,
 * буфер отправки забивается, задача залипает в send() и перестаёт обслуживать
 * всех остальных — сервер замолкает на десятки секунд (замерено: ≥45 с на
 * карте 122 КБ, до бесконечности при заполненном пуле сокетов).
 *
 * Поэтому у одного вызова отправки есть бюджет WEB_SEND_BUDGET_MS: как только
 * байты пошли, вызов сразу возвращается, и бюджет начинается заново. Живая
 * передача по Wi-Fi может замереть на секунды, пока полон буфер отправки
 * (здесь он 5760 байт), поэтому бюджет должен быть заметно больше таких
 * заминок — иначе клиент получает обрезанный файл. Нечитающий клиент не
 * отдаёт ничего вообще, и его сессия закрывается по этому же бюджету.
 *
 * Состояние по сессиям здесь не храним: дескрипторы в ESP-IDF выдаёт VFS, а не
 * lwIP, и они не ограничены CONFIG_LWIP_MAX_SOCKETS (в логах встречается fd=36),
 * поэтому таблица по fd была бы и неверной, и опасной.
 */
#define WEB_SEND_BUDGET_MS 5000
#define WEB_SEND_STEP_MS 10

/* Момент последнего успеха по каждой сессии. Ключ — дескриптор, но хранить это
 * массивом по дескриптору нельзя: в ESP-IDF его выдаёт VFS, а не lwIP, поэтому
 * он не ограничен CONFIG_LWIP_MAX_SOCKETS (в логах встречаются fd=36 и fd=56).
 * Поэтому таблица с проверкой ключа; нулевой дескриптор означает пустой слот. */
#define SESS_SLOTS 32
typedef struct {
    int fd;
    TickType_t last_ok;
} sess_progress_t;

static sess_progress_t sess_tab[SESS_SLOTS];

static sess_progress_t *sess_slot(int fd)
{
    if (fd <= 0) return NULL;
    unsigned h = ((unsigned)fd * 2654435761u) % SESS_SLOTS;
    sess_progress_t *free_slot = NULL;
    for (unsigned i = 0; i < SESS_SLOTS; i++) {
        sess_progress_t *e = &sess_tab[(h + i) % SESS_SLOTS];
        if (e->fd == fd) return e;
        if (e->fd == 0 && free_slot == NULL) free_slot = e;
    }
    if (free_slot) {
        free_slot->fd = fd;
        free_slot->last_ok = xTaskGetTickCount();
    }
    return free_slot;                 /* таблица полна — работаем по вызову */
}

/* Бюджет отправки разный: отдача файла — это сотни килобайт, и заминка на
 * секунды там нормальна; кадр WebSocket — сотни байт, и клиент, который не
 * может принять их за секунду, просто не успевает. Общая задача обслуживает все
 * сессии по очереди, поэтому медленный клиент API задерживал ответы ВСЕМ —
 * транзакция прибора занимает 40 мс, а до клиента ответ доходил через 500 мс. */
#define WEB_SEND_BUDGET_FILE_MS 5000
#define WEB_SEND_BUDGET_WS_MS   1000

static int send_bounded(int fd, const char *buf, size_t len, int flags, uint32_t budget_ms)
{
    sess_progress_t *s = sess_slot(fd);
    TickType_t call_start = xTaskGetTickCount();
    /* Сессия могла просто простоять (keep-alive у браузера — до 30 с), и тогда
     * «последний успех» у неё старый. Это не залипание: окно отсчёта должно
     * начинаться заново, иначе первый же запрос после паузы будет убит как
     * нечитающий. Настоящее залипание до этой точки не доживает — его сессия
     * закрывается по бюджету прямо в вызове. */
    if (s && (call_start - s->last_ok) > pdMS_TO_TICKS(WEB_SEND_BUDGET_MS))
        s->last_ok = call_start;
    for (;;) {
        int ret = send(fd, buf, len, (flags & ~MSG_WAITALL) | MSG_DONTWAIT);
        if (ret > 0) {
            if (s) s->last_ok = xTaskGetTickCount();
            return ret;
        }
        /* ret == 0 при закрытом окне — не ошибка, но и не прогресс */
        if (ret < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
            errno != EINTR && errno != ENOMEM && errno != ENOBUFS)
            return -1;
        TickType_t base = s ? s->last_ok : call_start;
        if ((xTaskGetTickCount() - base) > pdMS_TO_TICKS(budget_ms)) {
            ESP_LOGW(TAG, "fd=%d не принимает данные %lu мс, закрываю сессию",
                     fd, (unsigned long)budget_ms);
            if (s) s->fd = 0;         /* сессия закрывается — освобождаем слот */
            return -1;
        }
        vTaskDelay(pdMS_TO_TICKS(WEB_SEND_STEP_MS));
    }
}

static int socket_send_file(httpd_handle_t hd, int fd,
                            const char *buf, size_t len, int flags)
{
    (void)hd;
    return send_bounded(fd, buf, len, flags, WEB_SEND_BUDGET_FILE_MS);
}

static int socket_send_ws(httpd_handle_t hd, int fd,
                          const char *buf, size_t len, int flags)
{
    (void)hd;
    return send_bounded(fd, buf, len, flags, WEB_SEND_BUDGET_WS_MS);
}

/* Закрытие сессии: освобождаем её слот в таблице прогресса, иначе таблица
 * забьётся закрытыми дескрипторами и учёт прогресса перестанет работать. */
static void session_close(httpd_handle_t hd, int sockfd)
{
    (void)hd;
    sess_progress_t *e = sess_slot(sockfd);
    if (e && e->fd == sockfd) e->fd = 0;
    /* ВАЖНО: при заданном close_fn httpd НЕ закрывает сокет сам — он просто
     * вызывает этот обработчик вместо close() (см. httpd_sess_delete в
     * httpd_sess.c). Если здесь не закрыть, каждый завершённый сеанс теряет
     * сокет: пул LWIP исчерпывается, accept() возвращает ENFILE, и сервер
     * начинает отвечать сбросом на все запросы. */
    close(sockfd);
}

/* Настройка каждой новой сессии: без задержек на подтверждениях и со своей
 * отправкой. */
static esp_err_t session_open_common(httpd_handle_t hd, int sockfd,
                                    httpd_send_func_t send_fn)
{
    /* TCP_NODELAY. Без него мелкие ответы (а их тут большинство: ответы JSON
     * API и кадры WebSocket) ждут подтверждения по алгоритму Нейгла вместе с
     * задержанным ACK, и ЛЮБОЙ запрос стоит 100-250 мс независимо от объёма —
     * замерено: 1 регистр 250 мс, 16 регистров 112 мс, тогда как сама передача
     * по 9600 бод занимает десятки миллисекунд. Именно это и ощущается как
     * «контроллер еле дышит». esp_http_server включает TCP_NODELAY только для
     * сообщений об ошибках (и лишь при CONFIG_HTTPD_ERR_RESP_NO_DELAY),
     * обычные ответы и кадры WS идут через Нейгла. Ставим один раз на сессию. */
    int one = 1;
    if (setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) < 0)
        ESP_LOGW(TAG, "TCP_NODELAY для fd=%d не включился (%d)", sockfd, errno);
    return httpd_sess_set_send_override(hd, sockfd, send_fn);
}

static esp_err_t session_open(httpd_handle_t hd, int sockfd)
{
    return session_open_common(hd, sockfd, socket_send_file);
}

static esp_err_t session_open_ws(httpd_handle_t hd, int sockfd)
{
    return session_open_common(hd, sockfd, socket_send_ws);
}

//static uint32_t ws_connections_left = CONFIG_WEB_SERVER_MAX_CLIENTS;
/* start_ws_server()'s httpd instance has max_open_sockets=5, and httpd
 * reserves 3 of those for itself (listen + ctrl + spare) -> only 2 WS
 * clients actually fit; a 3rd connection overflows the instance's socket
 * pool (httpd_queue_work failures cascading into NIC buffer exhaustion). */
static uint32_t ws_connections_left = 2; /* internally limited */

static void httpd_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    switch (event_id) {
        case HTTP_SERVER_EVENT_ERROR : //httpd_err_code_t
          ESP_LOGW(TAG, "error: '%08lx'", (uint32_t)event_data);
          break;
        case HTTP_SERVER_EVENT_START : //NULL
          ESP_LOGW(TAG, "started");
          break;
        case HTTP_SERVER_EVENT_ON_CONNECTED : //int
          if (ws_connections_left > 0)
              ws_connections_left--;
          ESP_LOGD(TAG, "new connection: '%08x'; connections left %lu", *(int *)event_data, ws_connections_left);
          break;
        case HTTP_SERVER_EVENT_ON_HEADER : //int
          ESP_LOGD(TAG, "on header: '%08lx'", (uint32_t)event_data);
          break;
        case HTTP_SERVER_EVENT_HEADERS_SENT : //int
          ESP_LOGD(TAG, "headers sent: '%08lx'", (uint32_t)event_data);
          break;
        /* Эти два события приходят НА КАЖДУЮ порцию данных в обе стороны.
         * Логировать их на уровне WARN нельзя: вывод на консоль синхронный,
         * 115200 бод, и на файл в 122 КБ (30 порций по 4096) уходило больше
         * двухсот миллисекунд блокировки задачи httpd прямо посреди отдачи.
         * Для отладки включаются уровнем verbose. */
        case HTTP_SERVER_EVENT_ON_DATA :
          ESP_LOGD(TAG, "on data: fd: '%u' len:'%u'",
            ((esp_http_server_event_data *)event_data)->fd,
            ((esp_http_server_event_data *)event_data)->data_len);
          break;
        case HTTP_SERVER_EVENT_SENT_DATA :
          ESP_LOGD(TAG, "data sent: fd: '%u' len:'%u'",
            ((esp_http_server_event_data *)event_data)->fd,
            ((esp_http_server_event_data *)event_data)->data_len);
          break;
        case HTTP_SERVER_EVENT_DISCONNECTED : //int
          ws_connections_left++;
          ESP_LOGW(TAG, "disconnected: '%08x'", *(int *)event_data);
          break;
        case HTTP_SERVER_EVENT_STOP : //NULL
          ESP_LOGW(TAG, "stopped");
            break;
        default:
            break;
    }
}

httpd_handle_t start_webserver(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    config.uri_match_fn = httpd_uri_match_wildcard;
    config.max_uri_handlers = 20;
    config.server_port = CONFIG_WEB_SERVER_PORT;
    config.keep_alive_enable = true;
    /* Брошенное соединение держит сокет из общего пула: браузер, ушедший со
     * страницы, оставлял его на 30 с, а сокетов всего CONFIG_LWIP_MAX_SOCKETS.
     * Когда пул кончался, httpd писал "error in accept (23)" (ENFILE) и отвечал
     * сбросом на всё подряд. 10 секунд — достаточно для догрузки ресурсов
     * страницы и заметно быстрее освобождает пул. */
    config.keep_alive_idle = 10;
    config.keep_alive_interval = 5;
    config.keep_alive_count = 3;

    /* Сокеты берутся из общего пула CONFIG_LWIP_MAX_SOCKETS (32) вместе с
     * экземпляром WS-сервера, слушающими и служебными сокетами.
     *
     * Шести сессий не хватало: браузер открывает до шести соединений на узел,
     * а страница тянет ещё и карту прибора в сотню килобайт. Четыре вкладки
     * выбирали пул целиком, и следующая страница не могла загрузиться, пока не
     * истечёт keep-alive у брошенных соединений. */
    config.max_open_sockets = 12;

    /* Вытеснение по LRU здесь ОТКЛЮЧЕНО: оно закрывает уже принятые сессии,
     * чтобы впустить новую, и при быстром клиенте с keep-alive это рвёт живые
     * соединения (замерено: с ним 2 из 25 запросов, без него 22 из 25). Вместо
     * него пул сокетов увеличен, а брошенные соединения отпускаются быстрее
     * (keep_alive_idle). */
    config.lru_purge_enable = false;

    /* Своя отправка с дедлайном: одна нечитающая сессия не должна держать
     * задачу, общую для всех клиентов. */
    config.open_fn = session_open;
    config.close_fn = session_close;

    esp_event_handler_register(ESP_HTTP_SERVER_EVENT ,ESP_EVENT_ANY_ID, &httpd_event_handler, NULL);

    // Start the httpd server
    ESP_LOGI(TAG, "Starting server on port: '%d'", config.server_port);
    if (httpd_start(&server, &config) == ESP_OK) return server;
    ESP_LOGI(TAG, "Error starting server!");
    return NULL;
}

/* Own httpd instance (own FreeRTOS task + select() loop) for the WebSocket
 * API, kept off the HTTP file server's port. esp_http_server processes all
 * sessions of one instance serially in a single task, so a slow file
 * transfer on start_webserver()'s instance would otherwise starve WS
 * handshakes/frames queued on the same task. */
httpd_handle_t start_ws_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    config.server_port = CONFIG_WEB_SOCKET_SERVER_PORT;
    config.keep_alive_enable = true;
    /* Брошенная вкладка не должна держать слот WebSocket дольше нужного:
     * слотов немного, а страница при перезагрузке подключается заново. */
    config.keep_alive_idle = 10;
    config.keep_alive_interval = 5;
    config.keep_alive_count = 3;

    /* Столько страниц может быть подключено одновременно. Пять слотов давали
     * всего четыре рабочие вкладки — пятая (или перезагруженная) не
     * подключалась вовсе. Вместе с файловым сервером это 12+8=20 сессий, в пул
     * из 32 сокетов укладывается с запасом на слушающие и служебные. */
    config.max_open_sockets = 8;

    /* Короткий бюджет отправки: кадр API — это сотни байт, и клиент, который не
     * принимает их за секунду, просто не успевает. Иначе он задерживал ответы
     * всем остальным (общая задача). Вытеснение по LRU сюда не ставим —
     * соединения здесь долгие и предназначенные. */
    config.open_fn = session_open_ws;
    config.close_fn = session_close;

    /* Every httpd instance defaults to the SAME ctrl_port (internal UDP
     * control channel for cross-task signalling); two instances sharing
     * one ctrl_port would cross-talk. Give this instance its own. */
    config.ctrl_port = ESP_HTTPD_DEF_CTRL_PORT + 1;

    /* Event handler for ESP_HTTP_SERVER_EVENT is already registered in
     * start_webserver() above; that registration is instance-agnostic and
     * picks up events from this server too, so it must not be repeated
     * here (esp_event_handler_register is not idempotent). */

    ESP_LOGI(TAG, "Starting WS server on port: '%d'", config.server_port);
    httpd_handle_t ws_server = NULL;
    if (httpd_start(&ws_server, &config) == ESP_OK) return ws_server;
    ESP_LOGI(TAG, "Error starting WS server!");
    return NULL;
}

uint8_t webserver_register_handler(httpd_handle_t server, httpd_uri_t *uri_handler) {
  return httpd_register_uri_handler(server, uri_handler) == ESP_OK;
}
