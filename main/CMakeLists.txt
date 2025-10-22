idf_component_register(SRCS "apih_uart.c" "apih_sys.c" "connection.c" "uart.c" "nvs_config.c" "web_file_server.c" "ws_server.c" "app.c" "web_server.c" "file_system_helpers.c"
                    PRIV_REQUIRES esp_http_server esp_wifi nvs_flash esp_eth esp_driver_uart
                    INCLUDE_DIRS "."
                    EMBED_FILES "upload_script.html")

#littlefs_create_partition_image(storage ./flash_data FLASH_IN_PROJECT)
