idf_component_register(SRCS "web_file_server.c" "ws_server.c" "app.c" "http_server_helpers.c" "file_system_helpers.c"
                    PRIV_REQUIRES esp_http_server esp_wifi nvs_flash esp_eth
                    INCLUDE_DIRS ".")
