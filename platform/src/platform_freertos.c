#include "platform.h"

#include <stdarg.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <esp_log.h>

platform_mutex_t platform_mutex_create(void) {
    return xSemaphoreCreateRecursiveMutex();
}

int platform_mutex_take(platform_mutex_t m, uint32_t timeout_ms) {
    TickType_t ticks = (timeout_ms == PLATFORM_WAIT_FOREVER)
                       ? portMAX_DELAY
                       : pdMS_TO_TICKS(timeout_ms);
    return (xSemaphoreTakeRecursive((SemaphoreHandle_t)m, ticks) == pdTRUE) ? 1 : 0;
}

void platform_mutex_give(platform_mutex_t m) {
    xSemaphoreGiveRecursive((SemaphoreHandle_t)m);
}

platform_queue_t platform_queue_create(uint32_t depth, uint32_t item_size) {
    return xQueueCreate(depth, item_size);
}

int platform_queue_send(platform_queue_t q, const void *item, uint32_t timeout_ms) {
    TickType_t ticks = (timeout_ms == PLATFORM_WAIT_FOREVER)
                       ? portMAX_DELAY
                       : pdMS_TO_TICKS(timeout_ms);
    return (xQueueSend((QueueHandle_t)q, item, ticks) == pdPASS) ? 1 : 0;
}

int platform_queue_receive(platform_queue_t q, void *item, uint32_t timeout_ms) {
    TickType_t ticks = (timeout_ms == PLATFORM_WAIT_FOREVER)
                       ? portMAX_DELAY
                       : pdMS_TO_TICKS(timeout_ms);
    return (xQueueReceive((QueueHandle_t)q, item, ticks) == pdPASS) ? 1 : 0;
}

uint32_t platform_tick(void) {
    return (uint32_t)xTaskGetTickCount();
}

uint32_t platform_ms_to_ticks(uint32_t ms) {
    return (uint32_t)pdMS_TO_TICKS(ms);
}

uint32_t platform_ticks_to_ms(uint32_t ticks) {
    return (uint32_t)(ticks * portTICK_PERIOD_MS);
}

void platform_delay_ms(uint32_t ms) {
    vTaskDelay(pdMS_TO_TICKS(ms));
}

void platform_task_create(void (*fn)(void *), const char *name,
                        uint32_t stack_words, void *arg, int priority) {
    xTaskCreate(fn, name, stack_words, arg, priority, NULL);
}

void platform_task_self_delete(void) {
    vTaskDelete(NULL);
}

static void platform_log_write(esp_log_level_t level, const char *tag, const char *fmt, va_list args) {
    char buf[160];
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    if(len < 0) return;
    if((size_t)len >= sizeof(buf) - 1)
        len = sizeof(buf) - 2; /* leave room for '\n' + '\0' */
    buf[len]     = '\n';
    buf[len + 1] = '\0';
    esp_log_write(level, tag, "%s", buf);
}

void platform_log_i(const char *tag, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    platform_log_write(ESP_LOG_INFO, tag, fmt, args);
    va_end(args);
}

void platform_log_d(const char *tag, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    platform_log_write(ESP_LOG_DEBUG, tag, fmt, args);
    va_end(args);
}

void platform_log_w(const char *tag, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    platform_log_write(ESP_LOG_WARN, tag, fmt, args);
    va_end(args);
}
