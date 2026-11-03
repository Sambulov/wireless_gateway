#include <stdio.h>
#include <string.h>
#include <sys/param.h>
#include <sys/stat.h>

#include "esp_err.h"
#include "esp_log.h"

#include "esp_littlefs.h"
#include "esp_http_server.h"

#include "app.h"
#include "fs_path.h"

#define GW_LOG_MODULE GW_LOG_HTTP
#include "gw_log.h"

#define FILE_PATH_MAX  256

#define BASE_PATH  "/lfs"

#define INDEX_FILE "/index.html"

#define IS_FILE_EXT(filename, ext) \
    (strcasecmp(&filename[strlen(filename) - sizeof(ext) + 1], ext) == 0)

/* Тип содержимого по расширению файла */
static esp_err_t set_content_type_from_file(httpd_req_t *req, const char *filename)
{
    if (IS_FILE_EXT(filename, ".pdf")) {
        return httpd_resp_set_type(req, "application/pdf");
    } else if (IS_FILE_EXT(filename, ".html")) {
        return httpd_resp_set_type(req, "text/html");
    } else if (IS_FILE_EXT(filename, ".jpeg")) {
        return httpd_resp_set_type(req, "image/jpeg");
    } else if (IS_FILE_EXT(filename, ".ico")) {
        return httpd_resp_set_type(req, "image/x-icon");
    } else if (IS_FILE_EXT(filename, ".css")) {
        return httpd_resp_set_type(req, "text/css");
    } else if (IS_FILE_EXT(filename, ".js")) {
        return httpd_resp_set_type(req, "application/javascript");
    }
    /* Набор типов ограничен, всё прочее отдаём как plain text */
    return httpd_resp_set_type(req, "text/plain");
}

static esp_err_t common_http_handler(httpd_req_t *req)
{
    LOGD(TAG, "%s: common handler is called", __func__);

    /* Путь из URI — через общую проверку (fs_path.c): без query и якоря,
     * без "..", длинный — отказ. Нормализуется сразу на место после базы, а
     * место под "/index.html" оставлено в конце, поэтому дописывание ниже не
     * обрезается (лишних буферов на стеке задачи httpd нет). */
    char path[FILE_PATH_MAX];
    const size_t base_len = sizeof(BASE_PATH) - 1;
    memcpy(path, BASE_PATH, base_len);
    if (fs_path_from_uri(req->uri, path + base_len,
                         sizeof(path) - base_len - (sizeof(INDEX_FILE) - 1)) == NULL)
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File does not exist");

    LOGD(TAG, "%s: path requested", path);

    struct stat st;
    if (stat(path, &st) != 0)
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File does not exist");
    if (S_ISDIR(st.st_mode)) {
        /* Корень нормализован в "/", остальные каталоги — без хвостового '/'. */
        size_t n = strlen(path);
        snprintf(path + n, sizeof(path) - n, "%s",
                 (path[n - 1] == '/') ? INDEX_FILE + 1 : INDEX_FILE);
        if (stat(path, &st) != 0)
            return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File does not exist");
    }

    FILE *fd = fopen(path, "r");
    if (!fd) {
        ESP_LOGE(TAG, "Failed to read existing file : %s", path);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read existing file");
        return ESP_FAIL;
    }

    LOGD(TAG, "Sending file : %s (%ld bytes)...", path, st.st_size);
    set_content_type_from_file(req, path);
    /* Кэш хранить можно, но пользоваться им без проверки — нельзя: старая
     * страница продолжает опрашивать прибор по своим правилам. Поэтому копия
     * хранится, а на каждый запрос приходит ETag: совпал — отдаём 304 без тела
     * (иначе 0,5 МБ xterm.js ехали по Wi-Fi при каждом обновлении страницы). */
    /* mtime у LittleFS нет (приходит 0xffffffff), поэтому в ETag кладём размер и
     * ДАТУ СБОРКИ: файлы меняются вместе с прошивкой, а после перепрошивки браузер
     * обязан взять файл заново. */
    char etag[64];
    snprintf(etag, sizeof(etag), "\"%lx-%s\"",
             (unsigned long)st.st_size, __DATE__ " " __TIME__);
    for (char *p = etag; *p != '\0'; p++)         /* пробелы в ETag недопустимы */
        if (*p == ' ') *p = '_';
    httpd_resp_set_hdr(req, "ETag", etag);

    char etag_in[40];
    if ((httpd_req_get_hdr_value_str(req, "If-None-Match", etag_in, sizeof(etag_in)) == ESP_OK)
        && (strcmp(etag_in, etag) == 0)) {
        fclose(fd);
        httpd_resp_set_status(req, "304 Not Modified");
        return httpd_resp_send(req, NULL, 0);
    }
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

    const size_t chunksize = 4096;
    char *chunk = malloc(chunksize);
    if(chunk == NULL) {
        fclose(fd);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read existing file");
        return ESP_FAIL;
    }
    size_t data_len;
    do {
        data_len = fread(chunk, 1, chunksize, fd);
        if (data_len > 0) {
            if (httpd_resp_send_chunk(req, chunk, data_len) != ESP_OK) {
                free(chunk);
                fclose(fd);
                ESP_LOGE(TAG, "File sending failed, closing session");
                /* Клиент в этот момент уже не читает, а каждая попытка — ещё один
                 * блокирующий вызов в задаче, общей для всех клиентов. Возврат не
                 * ESP_OK заставит httpd закрыть эту сессию. */
                return ESP_FAIL;
           }
        }
    } while (data_len > 0);

    free(chunk);
    fclose(fd);
    LOGD(TAG, "File sending complete");

    return httpd_resp_send_chunk(req, NULL, 0);
}


/* Вшитая страница конфигурации живёт в образе приложения (gzip), поэтому
 * форматирование файловой системы её не уносит — именно она позволяет вернуть
 * файлы обратно. Регистрируется только в объекте точки доступа (main/app.c), и
 * там должна стоять раньше общего обработчика файлов, иначе тот станет искать
 * файл с именем "config" в файловой системе. */
static esp_err_t config_page_handler(httpd_req_t *req)
{
    extern const uint8_t config_page_gz_start[] asm("_binary_config_page_html_gz_start");
    extern const uint8_t config_page_gz_end[]   asm("_binary_config_page_html_gz_end");
    const size_t len = config_page_gz_end - config_page_gz_start;

    LOGI(TAG, "Serving built-in configuration page (%u bytes gzipped)", (unsigned)len);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, (const char *)config_page_gz_start, len);
}

/* Отдача файлов LittleFS по HTTP GET: так браузер получает страницы и карты
 * регистров. Файлы ИЗМЕНЯЮТСЯ только через WebSocket-сервисы
 * sys.file.write/delete/rename (см. apih_sys.c). */
httpd_uri_t file_server = {
    .uri       = "/*",
    .method    = HTTP_GET,
    .handler   = common_http_handler,
    .user_ctx   = NULL,
    .is_websocket = false 
};

/* Короткий адрес интерпретатора приборов: страница modbus.html лежит в файловой
 * системе, чтобы её можно было править файловым менеджером без перепрошивки. */
static esp_err_t modbus_alias_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "301 Moved Permanently");
    httpd_resp_set_hdr(req, "Location", "/modbus.html");
    return httpd_resp_sendstr(req, "Moved to /modbus.html");
}

/* Интерпретатор карт регистров — /modbus */
httpd_uri_t modbus_alias = {
    .uri       = "/modbus",
    .method    = HTTP_GET,
    .handler   = modbus_alias_handler,
    .user_ctx  = NULL,
    .is_websocket = false
};

/* Вшитая страница конфигурации: /config и /config.html. Её отдаёт только объект
 * точки доступа (main/app.c). */
httpd_uri_t config_page = {
    .uri       = "/config",
    .method    = HTTP_GET,
    .handler   = config_page_handler,
    .user_ctx  = NULL,
    .is_websocket = false
};

httpd_uri_t config_page_html = {
    .uri       = "/config.html",
    .method    = HTTP_GET,
    .handler   = config_page_handler,
    .user_ctx  = NULL,
    .is_websocket = false
};
