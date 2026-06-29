#include "ws_hal.h"

#include <stdarg.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <esp_log.h>

ws_mutex_t ws_hal_mutex_create(void) {
    return xSemaphoreCreateRecursiveMutex();
}

int ws_hal_mutex_take(ws_mutex_t m, uint32_t timeout_ms) {
    TickType_t ticks = (timeout_ms == WS_HAL_WAIT_FOREVER)
                       ? portMAX_DELAY
                       : pdMS_TO_TICKS(timeout_ms);
    return (xSemaphoreTakeRecursive((SemaphoreHandle_t)m, ticks) == pdTRUE) ? 1 : 0;
}

void ws_hal_mutex_give(ws_mutex_t m) {
    xSemaphoreGiveRecursive((SemaphoreHandle_t)m);
}

ws_queue_t ws_hal_queue_create(uint32_t depth, uint32_t item_size) {
    return xQueueCreate(depth, item_size);
}

int ws_hal_queue_send(ws_queue_t q, const void *item, uint32_t timeout_ms) {
    TickType_t ticks = (timeout_ms == WS_HAL_WAIT_FOREVER)
                       ? portMAX_DELAY
                       : pdMS_TO_TICKS(timeout_ms);
    return (xQueueSend((QueueHandle_t)q, item, ticks) == pdPASS) ? 1 : 0;
}

int ws_hal_queue_receive(ws_queue_t q, void *item, uint32_t timeout_ms) {
    TickType_t ticks = (timeout_ms == WS_HAL_WAIT_FOREVER)
                       ? portMAX_DELAY
                       : pdMS_TO_TICKS(timeout_ms);
    return (xQueueReceive((QueueHandle_t)q, item, ticks) == pdPASS) ? 1 : 0;
}

uint32_t ws_hal_tick(void) {
    return (uint32_t)xTaskGetTickCount();
}

void ws_hal_task_create(void (*fn)(void *), const char *name,
                        uint32_t stack_words, void *arg, int priority) {
    xTaskCreate(fn, name, stack_words, arg, priority, NULL);
}

void ws_hal_task_self_delete(void) {
    vTaskDelete(NULL);
}

void ws_hal_log_i(const char *tag, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    esp_log_writev(ESP_LOG_INFO, tag, fmt, args);
    va_end(args);
}

void ws_hal_log_w(const char *tag, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    esp_log_writev(ESP_LOG_WARN, tag, fmt, args);
    va_end(args);
}
