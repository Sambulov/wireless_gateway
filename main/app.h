
#include "esp_littlefs.h"
#include "esp_http_server.h"


extern const httpd_uri_t ws;


extern esp_vfs_littlefs_conf_t conf;
bool directory_exists(const char *path);
esp_err_t setup_littlefs(void);


/*=======================*/

#define WEB_FILE_HANDLER_NAME "/*"

void connect_handler(void* arg, esp_event_base_t event_base,
                            int32_t event_id, void* event_data);

void disconnect_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data);

esp_err_t stop_webserver(httpd_handle_t server);

httpd_handle_t start_webserver(void);

extern httpd_uri_t file_server;
extern httpd_uri_t dir_list;
extern httpd_uri_t file_upload;
extern httpd_uri_t file_delete;

/*=======================*/

typedef struct {
    char sta_ssid[32];
    size_t sta_ssid_len;
    char sta_pass[64];
    size_t sta_pass_len;
    char ap_ssid[32];
    size_t ap_ssid_len;
    char ap_pass[64];
    size_t ap_pass_len;

    httpd_handle_t web_server;
} app_context_t;
