#pragma once

#ifndef APIH_HOST_TEST
#include "esp_log.h"
#endif

/* Переключатели информационных логов по видам модулей. По умолчанию выключены:
 * в обычной работе консоль молчит. Предупреждения и ошибки (ESP_LOGW/ESP_LOGE)
 * остаются всегда — их скрывать нельзя.
 *
 * Включение — через define при сборке: либо точечно (GW_LOG_UART=1), либо всё
 * сразу (GW_LOG_ALL=1). Модуль выбирает свой переключатель ДО include:
 *
 *     #define GW_LOG_MODULE GW_LOG_UART
 *     #include "gw_log.h"
 */
#ifndef GW_LOG_ALL
#define GW_LOG_ALL 0
#endif

#ifndef GW_LOG_APP
#define GW_LOG_APP GW_LOG_ALL
#endif
#ifndef GW_LOG_UART
#define GW_LOG_UART GW_LOG_ALL
#endif
#ifndef GW_LOG_WIFI
#define GW_LOG_WIFI GW_LOG_ALL
#endif
#ifndef GW_LOG_SCAN
#define GW_LOG_SCAN GW_LOG_ALL
#endif
#ifndef GW_LOG_LINK
#define GW_LOG_LINK GW_LOG_ALL
#endif
#ifndef GW_LOG_WS
#define GW_LOG_WS GW_LOG_ALL
#endif
#ifndef GW_LOG_HTTP
#define GW_LOG_HTTP GW_LOG_ALL
#endif
#ifndef GW_LOG_API
#define GW_LOG_API GW_LOG_ALL
#endif
#ifndef GW_LOG_NVS
#define GW_LOG_NVS GW_LOG_ALL
#endif
#ifndef GW_LOG_FS
#define GW_LOG_FS GW_LOG_ALL
#endif

#ifndef GW_LOG_MODULE
#error "gw_log.h: перед include задайте GW_LOG_MODULE (например GW_LOG_UART)"
#endif

#if GW_LOG_MODULE
#define LOGI(tag, fmt, ...) ESP_LOGI(tag, fmt, ##__VA_ARGS__)
#define LOGD(tag, fmt, ...) ESP_LOGD(tag, fmt, ##__VA_ARGS__)
#else
#define LOGI(tag, fmt, ...) ((void)0)
#define LOGD(tag, fmt, ...) ((void)0)
#endif
