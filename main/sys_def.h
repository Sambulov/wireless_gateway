#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_system.h"

#define queue_create  xQueueCreate
#define queue_send xQueueSend

typedef BaseType_t base_type_t;
typedef QueueHandle_t queue_handle_t;
typedef TickType_t tick_type_t;

static inline base_type_t queue_receive( queue_handle_t queue,
                          void * const buffer,
                          tick_type_t ticks_to_wait) {
    return xQueueReceive(queue, buffer, ticks_to_wait);
}

static inline tick_type_t task_get_tick_count( void ) {
    return xTaskGetTickCount();
}

static inline void task_delay(const TickType_t ticks_to_delay) {
    vTaskDelay(ticks_to_delay);
}

#ifdef __cplusplus
}
#endif
