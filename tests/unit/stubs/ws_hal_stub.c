#include "ws_hal.h"
#include "ws_hal_stub.h"

#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* ── Tick ─────────────────────────────────────────────────────────────── */

static uint32_t stub_tick;

uint32_t ws_hal_tick(void) { return stub_tick; }

void ws_hal_stub_set_tick(uint32_t t)     { stub_tick = t; }
void ws_hal_stub_advance_tick(uint32_t d) { stub_tick += d; }

/* Tests treat 1 stub tick == 1 ms; no conversion needed. */
uint32_t ws_hal_ms_to_ticks(uint32_t ms) { return ms; }

/* ── Mutex ────────────────────────────────────────────────────────────── */

/* Tracks recursive depth so tests can catch unbalanced take/give. */
typedef struct { int depth; } stub_mutex_t;

ws_mutex_t ws_hal_mutex_create(void) {
    return calloc(1, sizeof(stub_mutex_t));
}

int ws_hal_mutex_take(ws_mutex_t m, uint32_t timeout_ms) {
    (void)timeout_ms;
    if (m) ((stub_mutex_t *)m)->depth++;
    return 1;
}

void ws_hal_mutex_give(ws_mutex_t m) {
    if (m) ((stub_mutex_t *)m)->depth--;
}

/* ── Queue ────────────────────────────────────────────────────────────── */

typedef struct {
    uint8_t *buf;
    uint32_t capacity;
    uint32_t item_size;
    uint32_t head;  /* next read slot  */
    uint32_t count; /* items in queue  */
} stub_queue_t;

ws_queue_t ws_hal_queue_create(uint32_t depth, uint32_t item_size) {
    stub_queue_t *q = calloc(1, sizeof(stub_queue_t));
    if (!q) return NULL;
    q->buf = calloc(depth, item_size);
    if (!q->buf) { free(q); return NULL; }
    q->capacity  = depth;
    q->item_size = item_size;
    return q;
}

int ws_hal_queue_send(ws_queue_t handle, const void *item, uint32_t timeout_ms) {
    (void)timeout_ms;
    stub_queue_t *q = handle;
    if (!q || q->count >= q->capacity) return 0;
    uint32_t slot = (q->head + q->count) % q->capacity;
    memcpy(q->buf + slot * q->item_size, item, q->item_size);
    q->count++;
    return 1;
}

int ws_hal_queue_receive(ws_queue_t handle, void *item, uint32_t timeout_ms) {
    (void)timeout_ms;
    stub_queue_t *q = handle;
    if (!q || q->count == 0) return 0;
    memcpy(item, q->buf + q->head * q->item_size, q->item_size);
    q->head = (q->head + 1) % q->capacity;
    q->count--;
    return 1;
}

uint32_t ws_hal_stub_queue_count(void *handle) {
    stub_queue_t *q = handle;
    return q ? q->count : 0;
}

/* ── Task ─────────────────────────────────────────────────────────────── */

static int stub_task_created_count;

void ws_hal_task_create(void (*fn)(void *), const char *name,
                        uint32_t stack_words, void *arg, int priority) {
    (void)fn; (void)name; (void)stack_words; (void)arg; (void)priority;
    stub_task_created_count++;
}

void ws_hal_task_self_delete(void) {}

int ws_hal_stub_task_created_count(void) { return stub_task_created_count; }

/* ── Log ──────────────────────────────────────────────────────────────── */

static uint32_t stub_log_i_count;
static uint32_t stub_log_w_count;

void ws_hal_log_i(const char *tag, const char *fmt, ...) {
    (void)tag; (void)fmt;
    stub_log_i_count++;
}

void ws_hal_log_w(const char *tag, const char *fmt, ...) {
    (void)tag; (void)fmt;
    stub_log_w_count++;
}

uint32_t ws_hal_stub_log_i_count(void) { return stub_log_i_count; }
uint32_t ws_hal_stub_log_w_count(void) { return stub_log_w_count; }

/* ── Reset ────────────────────────────────────────────────────────────── */

void ws_hal_stub_reset(void) {
    stub_tick               = 0;
    stub_task_created_count = 0;
    stub_log_i_count        = 0;
    stub_log_w_count        = 0;
}
