idf_component_register(
    SRCS "src/ws_server.c" "src/ws_hal_freertos.c"
    INCLUDE_DIRS "include"
    REQUIRES CodeLibrary json
)
