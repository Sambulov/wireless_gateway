#include <stdio.h>
#include <string.h>
#include <sys/param.h>
#include <sys/stat.h>

#include "esp_err.h"
#include "esp_log.h"

#include "esp_littlefs.h"
#include "esp_http_server.h"

#include "app.h"

#define FILE_PATH_MAX  256

#define BASE_PATH  "/lfs"

/* Copies the full path into destination buffer and returns
 * pointer to path (skipping the preceding base path) */
static char* get_path_from_uri(char *dest, const char *base_path, const char *uri, size_t destsize)
{
    const size_t base_pathlen = strlen(base_path);
    size_t pathlen = strlen(uri);

    const char *quest = strchr(uri, '?');
    if (quest) {
        pathlen = MIN(pathlen, quest - uri);
    }
    const char *hash = strchr(uri, '#');
    if (hash) {
        pathlen = MIN(pathlen, hash - uri);
    }

    if (base_pathlen + pathlen + 1 > destsize) {
        /* Full path string won't fit into destination buffer */
        return NULL;
    }

    /* Construct full path (base + path) */
    strcpy(dest, base_path);
    strlcpy(dest + base_pathlen, uri, pathlen + 1);

    return dest;
}

#define IS_FILE_EXT(filename, ext) \
    (strcasecmp(&filename[strlen(filename) - sizeof(ext) + 1], ext) == 0)

/* Set HTTP response content type according to file extension */
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
    /* This is a limited set only */
    /* For any other type always set as plain text */
    return httpd_resp_set_type(req, "text/plain");
}

static esp_err_t common_http_handler(httpd_req_t *req)
{
    ESP_LOGD(TAG, "%s: common handler is called", __func__);

    char path_buf[FILE_PATH_MAX];
    char *path = get_path_from_uri(path_buf, BASE_PATH, req->uri, FILE_PATH_MAX);

    ESP_LOGD(TAG, "%s: path requested", path);

    struct stat st;
    if(stat(path, &st) == 0) {
        if(S_ISDIR(st.st_mode)) {
            const char index_file[] = "index.html";
            size_t pathlen = strlen(path_buf);
            if (pathlen + sizeof(index_file) + 1 >= FILE_PATH_MAX)
                path = NULL;
            else {
                strcpy(&path_buf[pathlen], index_file);
                if (stat(path, &st) != 0)
                    path = NULL;
            }
        }
    }
    else path = NULL;

    if(path == NULL)
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File does not exist");

    FILE *fd = fopen(path, "r");
    if (!fd) {
        ESP_LOGE(TAG, "Failed to read existing file : %s", path);
        /* Respond with 500 Internal Server Error */
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read existing file");
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "Sending file : %s (%ld bytes)...", path, st.st_size);
    set_content_type_from_file(req, path);
    /* Страницы и карты не кэшируем в браузере: старая версия страницы в кэше
     * продолжает опрашивать прибор по своим правилам, и обновление файла на
     * приборе до неё не доходит. Карту страница кэширует сама, в localStorage,
     * с проверкой размера файла. */
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    /* Retrieve the pointer to scratch buffer for temporary storage */
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
            /* Send the buffer contents as HTTP response chunk */
            if (httpd_resp_send_chunk(req, chunk, data_len) != ESP_OK) {
                free(chunk);
                fclose(fd);
                ESP_LOGE(TAG, "File sending failed, closing session");
                /* Больше ничего не отправляем. Клиент в этот момент уже не
                 * читает (иначе отправка бы прошла), а каждая попытка — ещё
                 * один блокирующий вызов в задаче, общей для всех клиентов.
                 * Возврат не ESP_OK заставит httpd закрыть эту сессию. */
                return ESP_FAIL;
           }
        }
        /* Keep looping till the whole file is sent */
    } while (data_len > 0);

    free(chunk);
    fclose(fd);
    ESP_LOGD(TAG, "File sending complete");

    /* Respond with an empty chunk to signal HTTP response completion */
    return httpd_resp_send_chunk(req, NULL, 0);
}


/* Reserved addresses for the built-in configuration page. The page lives in
 * the application image (gzip, generated at build time by tools/pack_web.py),
 * so formatting or emptying the filesystem cannot take it away — the page is
 * what lets the files be put back. It must stay registered ahead of the
 * catch-all file_server handler below, which would otherwise look for a file
 * named "config" on the filesystem. */
static esp_err_t config_page_handler(httpd_req_t *req)
{
    extern const uint8_t config_page_gz_start[] asm("_binary_config_page_html_gz_start");
    extern const uint8_t config_page_gz_end[]   asm("_binary_config_page_html_gz_end");
    const size_t len = config_page_gz_end - config_page_gz_start;

    ESP_LOGI(TAG, "Serving built-in configuration page (%u bytes gzipped)", (unsigned)len);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, (const char *)config_page_gz_start, len);
}

/* URI handler for reading files from the filesystem. Файлы ИЗМЕНЯЮТСЯ только
 * через WebSocket-сервисы sys.file.write/delete/rename (см. apih_sys.c): HTTP-
 * методы /upload/…, /delete/… и /rename/… убраны вместе со своими обработчиками.
 * Отдача файлов по HTTP GET остаётся — именно так браузер получает страницы,
 * карты регистров и прочие файлы LittleFS. */
httpd_uri_t file_server = {
    .uri       = "/*",  // Match all URIs of type /path/to/file
    .method    = HTTP_GET,
    .handler   = common_http_handler,
    .user_ctx   = NULL,
    .is_websocket = false 
    //.user_ctx  = server_data    // Pass server data as context
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

/* Built-in configuration page, reachable as /config and /config.html */
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
