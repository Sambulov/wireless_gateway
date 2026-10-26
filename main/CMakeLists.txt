idf_component_register(SRCS "apih_modbus.c" "apih_uart.c" "seg_ring.c"
                            "apih_sys.c" "apih_sys_common.c" "apih_wifi.c" "apih_device.c" "apih_files.c" "apih_fw.c"
                            "connection.c" "net_wifi.c" "net_scan.c" "net_link.c"
                            "uart.c" "nvs_config.c" "web_file_server.c" "web_iface_policy.c" "ws_transport.c" "app.c" "web_server.c" "web_httpd.c" "file_system_helpers.c" "fs_path.c"
                    REQUIRES api transport platform
                    PRIV_REQUIRES esp_http_server esp_wifi nvs_flash esp_eth esp_driver_uart esp_driver_gpio esp_timer app_update esp_rom
                    INCLUDE_DIRS ".")

# The device configuration page (Wi-Fi, network, files) is embedded in the
# application image as gzip — it therefore survives a formatted LittleFS and
# cannot be deleted from it. Serving it with "Content-Encoding: gzip" keeps the
# browser happy without any decompression code on the device; see
# config_page_handler() in web_file_server.c.
set(CONFIG_PAGE_GZ ${CMAKE_CURRENT_BINARY_DIR}/config_page.html.gz)
add_custom_command(
    OUTPUT ${CONFIG_PAGE_GZ}
    COMMAND ${PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/../tools/pack_web.py
            ${CMAKE_CURRENT_SOURCE_DIR}/config_page.html ${CONFIG_PAGE_GZ}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/config_page.html
            ${CMAKE_CURRENT_SOURCE_DIR}/../tools/pack_web.py
    COMMENT "Packing configuration page for embedding"
    VERBATIM)
target_add_binary_data(${COMPONENT_LIB} ${CONFIG_PAGE_GZ} BINARY DEPENDS ${CONFIG_PAGE_GZ})

littlefs_create_partition_image(storage ./flash_data FLASH_IN_PROJECT)
