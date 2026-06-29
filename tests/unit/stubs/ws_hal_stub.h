#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Control interface for tests.
 * Call ws_hal_stub_reset() in TEST_GROUP setup() to start clean.
 */

void     ws_hal_stub_reset(void);

/* Tick control */
void     ws_hal_stub_set_tick(uint32_t tick);
void     ws_hal_stub_advance_tick(uint32_t delta);

/* Observability */
int      ws_hal_stub_task_created_count(void);
uint32_t ws_hal_stub_log_i_count(void);
uint32_t ws_hal_stub_log_w_count(void);

/* Queue introspection — how many items are currently pending in a queue */
uint32_t ws_hal_stub_queue_count(void *queue);

#ifdef __cplusplus
}
#endif
