
extern const httpd_uri_t ws;


extern esp_vfs_littlefs_conf_t conf;
bool directory_exists(const char *path);
esp_err_t setup_littlefs(void);


/*=======================*/

#define NO_ONE_HTTP_NAME "NO_ONE"

void connect_handler(void* arg, esp_event_base_t event_base,
                            int32_t event_id, void* event_data);

void disconnect_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data);

esp_err_t stop_webserver(httpd_handle_t server);

httpd_handle_t start_webserver(void);

extern const httpd_uri_t no_one_http;
