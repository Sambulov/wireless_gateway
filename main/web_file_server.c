#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_system.h>
#include <nvs_flash.h>
#include <sys/param.h>
#include <sys/stat.h>
#include "esp_netif.h"
#include "esp_eth.h"
#include "protocol_examples_common.h"
#include "ws_api.h"
#include "esp_littlefs.h"
#include <tftp_server_wg.h>

#include <esp_http_server.h>
#include <stdio.h>

#include "app.h"

static const char *TAG = "http_server_handlers";


// static esp_err_t http_index_handler(httpd_req_t *req)
// {
//         ESP_LOGI(TAG, "%s: index called", __func__);
// //	httpd_ws_send_frame(req, &ws_pkt);

// 	return ESP_OK;
// }

static esp_err_t no_one_http_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "%s: common handler is called", __func__);
    return httpd_resp_send_404(req);
}

// /* Send HTTP response with a run-time generated html consisting of
//  * a list of all files and folders under the requested path.
//  * In case of SPIFFS this returns empty list when path is any
//  * string other than '/', since SPIFFS doesn't support directories */
// static esp_err_t http_resp_dir_html(httpd_req_t *req, const char *dirpath)
// {
//     char entrypath[CONFIG_HTTPD_MAX_URI_LEN];
//     char entrysize[16];
//     const char *entrytype;

//     struct dirent *entry;
//     struct stat entry_stat;

//     DIR *dir = fopen(dirpath);
//     const size_t dirpath_len = strlen(dirpath);

//     /* Retrieve the base path of file storage to construct the full path */
//     strlcpy(entrypath, dirpath, sizeof(entrypath));

//     if (!dir) {
//         ESP_LOGE(TAG, "Failed to stat dir : %s", dirpath);
//         /* Respond with 404 Not Found */
//         httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Directory does not exist");
//         return ESP_FAIL;
//     }

//     /* Send HTML file header */
//     httpd_resp_sendstr_chunk(req, "<!DOCTYPE html><html><body>");

//     /* Get handle to embedded file upload script */
//     extern const unsigned char upload_script_start[] asm("_binary_upload_script_html_start");
//     extern const unsigned char upload_script_end[]   asm("_binary_upload_script_html_end");
//     const size_t upload_script_size = (upload_script_end - upload_script_start);

//     /* Add file upload form and script which on execution sends a POST request to /upload */
//     httpd_resp_send_chunk(req, (const char *)upload_script_start, upload_script_size);

//     /* Send file-list table definition and column labels */
//     httpd_resp_sendstr_chunk(req,
//         "<table class=\"fixed\" border=\"1\">"
//         "<col width=\"800px\" /><col width=\"300px\" /><col width=\"300px\" /><col width=\"100px\" />"
//         "<thead><tr><th>Name</th><th>Type</th><th>Size (Bytes)</th><th>Delete</th></tr></thead>"
//         "<tbody>");

//     /* Iterate over all files / folders and fetch their names and sizes */
//     while ((entry = readdir(dir)) != NULL) {
//         entrytype = (entry->d_type == DT_DIR ? "directory" : "file");

//         strlcpy(entrypath + dirpath_len, entry->d_name, sizeof(entrypath) - dirpath_len);
//         if (stat(entrypath, &entry_stat) == -1) {
//             ESP_LOGE(TAG, "Failed to stat %s : %s", entrytype, entry->d_name);
//             continue;
//         }
//         sprintf(entrysize, "%ld", entry_stat.st_size);
//         ESP_LOGI(TAG, "Found %s : %s (%s bytes)", entrytype, entry->d_name, entrysize);

//         /* Send chunk of HTML file containing table entries with file name and size */
//         httpd_resp_sendstr_chunk(req, "<tr><td><a href=\"");
//         httpd_resp_sendstr_chunk(req, req->uri);
//         httpd_resp_sendstr_chunk(req, entry->d_name);
//         if (entry->d_type == DT_DIR) {
//             httpd_resp_sendstr_chunk(req, "/");
//         }
//         httpd_resp_sendstr_chunk(req, "\">");
//         httpd_resp_sendstr_chunk(req, entry->d_name);
//         httpd_resp_sendstr_chunk(req, "</a></td><td>");
//         httpd_resp_sendstr_chunk(req, entrytype);
//         httpd_resp_sendstr_chunk(req, "</td><td>");
//         httpd_resp_sendstr_chunk(req, entrysize);
//         httpd_resp_sendstr_chunk(req, "</td><td>");
//         httpd_resp_sendstr_chunk(req, "<form method=\"post\" action=\"/delete");
//         httpd_resp_sendstr_chunk(req, req->uri);
//         httpd_resp_sendstr_chunk(req, entry->d_name);
//         httpd_resp_sendstr_chunk(req, "\"><button type=\"submit\">Delete</button></form>");
//         httpd_resp_sendstr_chunk(req, "</td></tr>\n");
//     }
//     closedir(dir);

//     /* Finish the file list table */
//     httpd_resp_sendstr_chunk(req, "</tbody></table>");

//     /* Send remaining chunk of HTML file to complete it */
//     httpd_resp_sendstr_chunk(req, "</body></html>");

//     /* Send empty chunk to signal HTTP response completion */
//     httpd_resp_sendstr_chunk(req, NULL);
//     return ESP_OK;
// }

// static esp_err_t download_get_handler(httpd_req_t *req)
// {
//     char filepath[CONFIG_HTTPD_MAX_URI_LEN];
//     FILE *fd = NULL;
//     struct stat file_stat;

//     const char *filename = get_path_from_uri(filepath, ((struct file_server_data *)req->user_ctx)->base_path,
//                                              req->uri, sizeof(filepath));
//     if (!filename) {
//         ESP_LOGE(TAG, "Filename is too long");
//         /* Respond with 500 Internal Server Error */
//         httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Filename too long");
//         return ESP_FAIL;
//     }

//     /* If name has trailing '/', respond with directory contents */
//     if (filename[strlen(filename) - 1] == '/') {
//         return http_resp_dir_html(req, filepath);
//     }

//     if (stat(filepath, &file_stat) == -1) {
//         // /* If file not present on SPIFFS check if URI
//         //  * corresponds to one of the hardcoded paths */
//         // if (strcmp(filename, "/index.html") == 0) {
//         //     return index_html_get_handler(req);
//         // } else if (strcmp(filename, "/favicon.ico") == 0) {
//         //     return favicon_get_handler(req);
//         // }
//         ESP_LOGE(TAG, "Failed to stat file : %s", filepath);
//         /* Respond with 404 Not Found */
//         httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File does not exist");
//         return ESP_FAIL;
//     }

//     fd = fopen(filepath, "r");
//     if (!fd) {
//         ESP_LOGE(TAG, "Failed to read existing file : %s", filepath);
//         /* Respond with 500 Internal Server Error */
//         httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read existing file");
//         return ESP_FAIL;
//     }

//     ESP_LOGI(TAG, "Sending file : %s (%ld bytes)...", filename, file_stat.st_size);
//     set_content_type_from_file(req, filename);

//     /* Retrieve the pointer to scratch buffer for temporary storage */
//     char *chunk = ((struct file_server_data *)req->user_ctx)->scratch;
//     size_t chunksize;
//     do {
//         /* Read file in chunks into the scratch buffer */
//         chunksize = fread(chunk, 1, SCRATCH_BUFSIZE, fd);

//         if (chunksize > 0) {
//             /* Send the buffer contents as HTTP response chunk */
//             if (httpd_resp_send_chunk(req, chunk, chunksize) != ESP_OK) {
//                 fclose(fd);
//                 ESP_LOGE(TAG, "File sending failed!");
//                 /* Abort sending file */
//                 httpd_resp_sendstr_chunk(req, NULL);
//                 /* Respond with 500 Internal Server Error */
//                 httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to send file");
//                return ESP_FAIL;
//            }
//         }

//         /* Keep looping till the whole file is sent */
//     } while (chunksize != 0);

//     /* Close file after sending complete */
//     fclose(fd);
//     ESP_LOGI(TAG, "File sending complete");

//     /* Respond with an empty chunk to signal HTTP response completion */
// #ifdef CONFIG_EXAMPLE_HTTPD_CONN_CLOSE_HEADER
//     httpd_resp_set_hdr(req, "Connection", "close");
// #endif
//     httpd_resp_send_chunk(req, NULL, 0);
//     return ESP_OK;
// }

// static const httpd_uri_t http_index = {
//         .uri        = "/index.html",
//         .method     = HTTP_GET,
//         .handler    = http_index_handler,
//         .user_ctx   = NULL,
//         .is_websocket = false 
// };

const httpd_uri_t no_one_http = {
        .uri        = NO_ONE_HTTP_NAME,
        .method     = HTTP_GET,
        .handler    = no_one_http_handler,
        .user_ctx   = NULL,
        .is_websocket = false 
};

// const httpd_uri_t no_one_http = {
//         .uri        = "/*",
//         .method     = HTTP_GET,
//         .handler    = download_get_handler,
//         .user_ctx   = NULL,
//         .is_websocket = false 
// };

