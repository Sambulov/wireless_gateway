#pragma once

#include <stdint.h>

typedef void *ws_mutex_t;
typedef void *ws_queue_t;

#define WS_HAL_WAIT_FOREVER  UINT32_MAX
#define WS_HAL_WAIT_NONE     0U

/* Mutex */
ws_mutex_t ws_hal_mutex_create(void);
int        ws_hal_mutex_take(ws_mutex_t m, uint32_t timeout_ms);
void       ws_hal_mutex_give(ws_mutex_t m);

/* Queue */
ws_queue_t ws_hal_queue_create(uint32_t depth, uint32_t item_size);
int        ws_hal_queue_send(ws_queue_t q, const void *item, uint32_t timeout_ms);
int        ws_hal_queue_receive(ws_queue_t q, void *item, uint32_t timeout_ms);

/* Time — monotonic ticks; same unit as FreeRTOS xTaskGetTickCount() on target */
uint32_t   ws_hal_tick(void);
uint32_t   ws_hal_ms_to_ticks(uint32_t ms);

/* Task */
void ws_hal_task_create(void (*fn)(void *), const char *name,
                        uint32_t stack_words, void *arg, int priority);
void ws_hal_task_self_delete(void);

/* Log */
void ws_hal_log_i(const char *tag, const char *fmt, ...);
void ws_hal_log_w(const char *tag, const char *fmt, ...);
