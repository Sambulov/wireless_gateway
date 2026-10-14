#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/param.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include <dirent.h>

#include "esp_err.h"
#include "esp_log.h"

#include "esp_littlefs.h"
#include "esp_http_server.h"

#include "app.h"

#define FILE_PATH_MAX  256

#define MAX_FILE_SIZE   (500*1024) // 200 KB
#define MAX_FILE_SIZE_STR "500KB"

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

/* Answers either an XHR (plain text, no navigation) or a form post (redirect
 * back to the filesystem root, which is what the /dir/ page expects). */
static esp_err_t respond_done(httpd_req_t *req, const char *msg)
{
    if (httpd_req_get_hdr_value_len(req, "X-Requested-With") > 0) {
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_sendstr(req, msg);
    }
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    return httpd_resp_sendstr(req, msg);
}

static esp_err_t respond_error(httpd_req_t *req, const char *status, const char *msg)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, msg);
}


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

/* Handler to upload a file onto the server */
static esp_err_t upload_post_handler(httpd_req_t *req)
{
    char filepath[FILE_PATH_MAX];
    FILE *fd = NULL;
    struct stat file_stat;

    /* Skip leading "/upload" from URI to get filename */
    /* Note sizeof() counts NULL termination hence the -1 */
    const char *filename = get_path_from_uri(filepath, BASE_PATH,
                                             req->uri + sizeof("/upload") - 1, sizeof(filepath));
    if (!filename) {
        /* Respond with 500 Internal Server Error */
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Filename too long");
        return ESP_FAIL;
    }

    /* Filename cannot have a trailing '/' */
    if (filename[strlen(filename) - 1] == '/') {
        ESP_LOGE(TAG, "Invalid filename : %s", filename);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Invalid filename");
        return ESP_FAIL;
    }

    if (stat(filepath, &file_stat) == 0) {
        /* Uploads never overwrite: the old file has to be deleted or renamed
         * first, so a page that is currently being served cannot be replaced
         * half-way by accident. */
        ESP_LOGW(TAG, "File already exists : %s", filepath);
        return respond_error(req, "409 Conflict",
                             "File already exists: delete or rename it first");
    }

    /* File cannot be larger than a limit */
    if (req->content_len > MAX_FILE_SIZE) {
        ESP_LOGE(TAG, "File too large : %d bytes", req->content_len);
        /* Respond with 400 Bad Request */
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "File size must be less than "
                            MAX_FILE_SIZE_STR "!");
        /* Return failure to close underlying connection else the
         * incoming file content will keep the socket busy */
        return ESP_FAIL;
    }

    fd = fopen(filepath, "w");
    if (!fd) {
        ESP_LOGE(TAG, "Failed to create file : %s", filepath);
        /* Respond with 500 Internal Server Error */
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create file");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Receiving file : %s...", filename);

    /* Retrieve the pointer to scratch buffer for temporary storage */
    const size_t buf_size = 512;
    char *buf = malloc(buf_size);
    int received;
    if(buf == NULL) {
        ESP_LOGE(TAG, "malloc failed");
        /* Respond with 500 Internal Server Error */
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No mem");
    }
    /* Content length of the request gives
     * the size of the file being uploaded */
    int remaining = req->content_len;

    while (remaining > 0) {

        ESP_LOGI(TAG, "Remaining size : %d", remaining);
        /* Receive the file part by part into a buffer */
        if ((received = httpd_req_recv(req, buf, MIN(remaining, buf_size))) <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) {
                /* Retry if timeout occurred */
                continue;
            }
            
            free(buf);
            /* In case of unrecoverable error,
             * close and delete the unfinished file*/
            fclose(fd);
            unlink(filepath);

            ESP_LOGE(TAG, "File reception failed!");
            /* Respond with 500 Internal Server Error */
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to receive file");
            return ESP_FAIL;
        }

        /* Write buffer content to file on storage */
        if (received && (received != fwrite(buf, 1, received, fd))) {
            /* Couldn't write everything to file!
             * Storage may be full? */
            free(buf);
            fclose(fd);
            unlink(filepath);

            ESP_LOGE(TAG, "File write failed!");
            /* Respond with 500 Internal Server Error */
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to write file to storage");
            return ESP_FAIL;
        }

        /* Keep track of remaining size of
         * the file left to be uploaded */
        remaining -= received;
    }

    /* Close file upon upload completion */
    free(buf);
    fclose(fd);
    ESP_LOGI(TAG, "File reception complete");

    return respond_done(req, "File uploaded successfully");
}

/* Handler to delete a file from the server */
static esp_err_t delete_post_handler(httpd_req_t *req)
{
    char filepath[FILE_PATH_MAX];
    struct stat file_stat;

    /* Skip leading "/delete" from URI to get filename */
    /* Note sizeof() counts NULL termination hence the -1 */
    const char *filename = get_path_from_uri(filepath, BASE_PATH,
                                             req->uri  + sizeof("/delete") - 1, sizeof(filepath));
    if (!filename) {
        /* Respond with 500 Internal Server Error */
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Filename too long");
        return ESP_FAIL;
    }

    /* Filename cannot have a trailing '/' */
    if (filename[strlen(filename) - 1] == '/') {
        ESP_LOGE(TAG, "Invalid filename : %s", filename);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Invalid filename");
        return ESP_FAIL;
    }

    if (stat(filepath, &file_stat) == -1) {
        ESP_LOGE(TAG, "File does not exist : %s", filename);
        /* Respond with 400 Bad Request */
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "File does not exist");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Deleting file : %s", filename);
    /* Delete file */
    unlink(filepath);

    return respond_done(req, "File deleted successfully");
}

/* Handler to rename a file: POST /rename/<old>, body holds the new path.
 * Renaming onto an existing name is refused, just like an upload would be. */
static esp_err_t rename_post_handler(httpd_req_t *req)
{
    char src_path[FILE_PATH_MAX];
    char dst_path[FILE_PATH_MAX];
    char uri_buf[FILE_PATH_MAX];
    char body[FILE_PATH_MAX];
    struct stat file_stat;

    const char *src = get_path_from_uri(src_path, BASE_PATH,
                                        req->uri + sizeof("/rename") - 1, sizeof(src_path));
    if (!src)
        return respond_error(req, "500 Internal Server Error", "Path too long");

    if (src[strlen(src) - 1] == '/')
        return respond_error(req, "400 Bad Request", "Invalid file name");

    int len = req->content_len;
    if (len <= 0 || len >= (int)sizeof(body))
        return respond_error(req, "400 Bad Request", "New name is required");

    if (httpd_req_recv(req, body, len) != len)
        return respond_error(req, "500 Internal Server Error", "Failed to read request");

    body[len] = '\0';
    /* The page sends the bare name; trailing CR/LF and spaces are stripped so a
     * hand-made request works too. */
    while (len > 0 && (body[len - 1] == '\n' || body[len - 1] == '\r' || body[len - 1] == ' '))
        body[--len] = '\0';
    if (len == 0)
        return respond_error(req, "400 Bad Request", "New name is empty");

    size_t blen = strlen(body);
    if (body[0] == '/') {
        /* Абсолютный путь от корня файловой системы */
        if (blen >= sizeof(uri_buf))
            return respond_error(req, "500 Internal Server Error", "New name too long");
        strlcpy(uri_buf, body, sizeof(uri_buf));
    } else {
        /* Короткое имя: переименование остаётся в том же каталоге, что и файл */
        const char *src_uri = req->uri + sizeof("/rename") - 1;
        const char *last_slash = strrchr(src_uri, '/');
        size_t dir_len = last_slash ? (size_t)(last_slash - src_uri) : 0;
        if (dir_len + blen + 2 >= sizeof(uri_buf))
            return respond_error(req, "500 Internal Server Error", "New name too long");
        memcpy(uri_buf, src_uri, dir_len);
        uri_buf[dir_len] = '/';
        memcpy(uri_buf + dir_len + 1, body, blen + 1);
    }

    const char *dst = get_path_from_uri(dst_path, BASE_PATH, uri_buf, sizeof(dst_path));
    if (!dst)
        return respond_error(req, "500 Internal Server Error", "New name too long");

    if (dst[strlen(dst) - 1] == '/')
        return respond_error(req, "400 Bad Request", "New name is invalid");

    if (stat(src, &file_stat) != 0) {
        ESP_LOGW(TAG, "Rename: source does not exist: %s", src);
        return respond_error(req, "404 Not Found", "File does not exist");
    }
    if (stat(dst, &file_stat) == 0) {
        ESP_LOGW(TAG, "Rename: target exists: %s", dst);
        return respond_error(req, "409 Conflict", "Target name already exists");
    }

    ESP_LOGI(TAG, "Renaming %s -> %s", src, dst);
    if (rename(src, dst) != 0) {
        ESP_LOGE(TAG, "Rename failed (%d)", errno);
        return respond_error(req, "500 Internal Server Error", "Rename failed");
    }

    return respond_done(req, "File renamed successfully");
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

/* URI handler for getting uploaded files */
httpd_uri_t file_server = {
    .uri       = "/*",  // Match all URIs of type /path/to/file
    .method    = HTTP_GET,
    .handler   = common_http_handler,
    .user_ctx   = NULL,
    .is_websocket = false 
    //.user_ctx  = server_data    // Pass server data as context
};

/* URI handler for uploading files to server */
httpd_uri_t file_upload = {
    .uri       = "/upload/*",   // Match all URIs of type /upload/path/to/file
    .method    = HTTP_POST,
    .handler   = upload_post_handler,
    .user_ctx   = NULL,
    .is_websocket = false 
    //.user_ctx  = server_data    // Pass server data as context
};

// /* URI handler for deleting files from server */
httpd_uri_t file_delete = {
    .uri       = "/delete/*",   // Match all URIs of type /delete/path/to/file
    .method    = HTTP_POST,
    .handler   = delete_post_handler,
    .user_ctx   = NULL,
    .is_websocket = false 
    //.user_ctx  = server_data    // Pass server data as context
};

/* URI handler for renaming a file: /rename/<old path>, new path in the body */
httpd_uri_t file_rename = {
    .uri       = "/rename/*",
    .method    = HTTP_POST,
    .handler   = rename_post_handler,
    .user_ctx  = NULL,
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
