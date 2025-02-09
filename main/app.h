
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
