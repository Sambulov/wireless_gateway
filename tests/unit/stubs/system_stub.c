#include "platform.h"
#include "api.h"
#include "esp_err.h"
#include "system_stub.h"

#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* ── Object registry ──────────────────────────────────────────────────── */

/* The stub owns every mutex/queue it hands out. Production code creates them
 * once at init and never deletes them (tasks live forever on target), so
 * system_stub_reset() plays the role of a reboot and frees them all —
 * otherwise each test's api_init() would leak. */
typedef struct stub_obj {
    struct stub_obj *next;
    void *obj;
    void (*destroy)(void *obj);
} stub_obj_t;

static stub_obj_t *stub_objs;

static void *stub_track(void *obj, void (*destroy)(void *obj)) {
    if (!obj) return NULL;
    stub_obj_t *n = malloc(sizeof(*n));
    if (!n) { destroy(obj); return NULL; }
    n->obj = obj;
    n->destroy = destroy;
    n->next = stub_objs;
    stub_objs = n;
    return obj;
}

static void stub_free_all(void) {
    while (stub_objs) {
        stub_obj_t *n = stub_objs;
        stub_objs = n->next;
        n->destroy(n->obj);
        free(n);
    }
}

/* ── Time ─────────────────────────────────────────────────────────────── */

static uint32_t stub_time;

uint32_t platform_tick(void) { return stub_time; }

void system_stub_set_time(uint32_t ms)     { stub_time = ms; }
void system_stub_advance_time(uint32_t d)  { stub_time += d; }

/* Tests treat 1 stub tick == 1 ms; no conversion needed. */
uint32_t platform_ms_to_ticks(uint32_t ms) { return ms; }
uint32_t platform_ticks_to_ms(uint32_t ticks) { return ticks; }

/* Задержку не отбываем, а запоминаем: тесты проверяют, сколько кто-то просил
 * подождать, и на этом же продолжают работу. */
static uint32_t stub_last_delay;
void platform_delay_ms(uint32_t ms) { stub_last_delay += ms; }
uint32_t system_stub_last_delay(void) { return stub_last_delay; }

/* ── Mutex ────────────────────────────────────────────────────────────── */

/* Tracks recursive depth so tests can catch unbalanced take/give, and counts
 * takings: «ядро не работает без мьютекса системы» проверяется по счётчику. */
typedef struct { int depth; } stub_mutex_t;

static uint32_t stub_mutex_takes;
static uint32_t stub_mutex_creates;
static uint32_t stub_mutex_alive;

platform_mutex_t platform_mutex_create(void) {
    void *m = stub_track(calloc(1, sizeof(stub_mutex_t)), free);
    if (m) { stub_mutex_creates++; stub_mutex_alive++; }
    return m;
}

int platform_mutex_take(platform_mutex_t m, uint32_t timeout_ms) {
    (void)timeout_ms;
    if (m) { ((stub_mutex_t *)m)->depth++; stub_mutex_takes++; }
    return 1;
}

void platform_mutex_give(platform_mutex_t m) {
    if (m) ((stub_mutex_t *)m)->depth--;
}

uint32_t system_stub_mutex_takes(void)   { return stub_mutex_takes; }
uint32_t system_stub_mutex_creates(void) { return stub_mutex_creates; }
uint32_t system_stub_mutex_alive(void)   { return stub_mutex_alive; }

/* ── Queue ────────────────────────────────────────────────────────────── */

typedef struct {
    uint8_t *buf;
    uint32_t capacity;
    uint32_t item_size;
    uint32_t head;  /* next read slot  */
    uint32_t count; /* items in queue  */
} stub_queue_t;

static void stub_queue_destroy(void *handle) {
    stub_queue_t *q = handle;
    free(q->buf);
    free(q);
}

platform_queue_t platform_queue_create(uint32_t depth, uint32_t item_size) {
    stub_queue_t *q = calloc(1, sizeof(stub_queue_t));
    if (!q) return NULL;
    q->buf = calloc(depth, item_size);
    if (!q->buf) { free(q); return NULL; }
    q->capacity  = depth;
    q->item_size = item_size;
    return stub_track(q, stub_queue_destroy);
}

int platform_queue_send(platform_queue_t handle, const void *item, uint32_t timeout_ms) {
    (void)timeout_ms;
    stub_queue_t *q = handle;
    if (!q || q->count >= q->capacity) return 0;
    uint32_t slot = (q->head + q->count) % q->capacity;
    memcpy(q->buf + slot * q->item_size, item, q->item_size);
    q->count++;
    return 1;
}

int platform_queue_receive(platform_queue_t handle, void *item, uint32_t timeout_ms) {
    (void)timeout_ms;
    stub_queue_t *q = handle;
    if (!q || q->count == 0) return 0;
    memcpy(item, q->buf + q->head * q->item_size, q->item_size);
    q->head = (q->head + 1) % q->capacity;
    q->count--;
    return 1;
}

uint32_t system_stub_queue_count(void *handle) {
    stub_queue_t *q = handle;
    return q ? q->count : 0;
}

/* ── Перезагрузка ─────────────────────────────────────────────────────── */

/* esp_restart (esp_system.h) — перезагрузка прибора. Хост «перезагружаться» не
 * умеет, поэтому вызов только считается: тест проверяет, что служба прошивки его
 * запросила. */
static uint32_t stub_restart_count;

void esp_restart(void) { stub_restart_count++; }

uint32_t system_stub_restart_count(void) { return stub_restart_count; }

/* Имя ошибки ESP-IDF: на хосте его нет. Текст сообщения клиенту от этого не
 * зависит — важен код отказа. */
const char *esp_err_to_name(esp_err_t code) {
    (void)code;
    return "ESP_ERR";
}

/* ── Task ─────────────────────────────────────────────────────────────── */

static int stub_task_created_count;

void platform_task_create(void (*fn)(void *), const char *name,
                        uint32_t stack_words, void *arg, int priority) {
    (void)fn; (void)name; (void)stack_words; (void)arg; (void)priority;
    stub_task_created_count++;
}

void platform_task_self_delete(void) {}

int system_stub_task_created_count(void) { return stub_task_created_count; }

/* ── Log ──────────────────────────────────────────────────────────────── */

static uint32_t stub_log_i_count;
static uint32_t stub_log_w_count;

void platform_log_i(const char *tag, const char *fmt, ...) {
    (void)tag; (void)fmt;
    stub_log_i_count++;
}

void platform_log_d(const char *tag, const char *fmt, ...) {
    (void)tag; (void)fmt;
}

void platform_log_w(const char *tag, const char *fmt, ...) {
    (void)tag; (void)fmt;
    stub_log_w_count++;
}

uint32_t system_stub_log_i_count(void) { return stub_log_i_count; }
uint32_t system_stub_log_w_count(void) { return stub_log_w_count; }

/* ── Системная таблица ядра API ───────────────────────────────────────── */

/* Ядро не зовёт platform_* напрямую: оно получает эти операции таблицей и
 * передаёт в каждую системный контекст (здесь он не нужен — стаб один на
 * процесс, как платформа в прошивке). Таблица — ТА ЖЕ система, что у сервисов:
 * управляемое время и счётчики мьютексов и журнала. */

static void *stub_system_mutex_create(void *context) {
    (void)context;
    return platform_mutex_create();
}

static void stub_system_mutex_take(void *context, void *mutex) {
    (void)context;
    (void)platform_mutex_take((platform_mutex_t)mutex, PLATFORM_WAIT_FOREVER);
}

static void stub_system_mutex_give(void *context, void *mutex) {
    (void)context;
    platform_mutex_give((platform_mutex_t)mutex);
}

static void stub_system_log(void *context, int level, const char *tag, const char *fmt, ...) {
    (void)context; (void)tag; (void)fmt;
    switch (level) {
    case API_LOG_WARN:  stub_log_w_count++; break;
    case API_LOG_INFO:  stub_log_i_count++; break;
    default:                                break;   /* debug в тестах не считаем */
    }
}

static uint32_t stub_system_time_ms(void *context) {
    (void)context;
    /* 1 тик стаба == 1 мс (см. platform_ms_to_ticks): тесты двигают время
     * system_stub_advance_time в миллисекундах. */
    return platform_tick();
}

static const api_system_t stub_api_system = {
    .context      = NULL,                 /* у стаба нет объекта системы */
    .mutex_create = stub_system_mutex_create,
    .mutex_take   = stub_system_mutex_take,
    .mutex_give   = stub_system_mutex_give,
    .time_ms      = stub_system_time_ms,
    .log          = stub_system_log,
};

const api_system_t *system_stub_api_system(void) {
    return &stub_api_system;
}

/* ── Reset ────────────────────────────────────────────────────────────── */

void system_stub_reset(void) {
    stub_last_delay         = 0;
    stub_free_all();
    stub_time               = 0;
    stub_task_created_count = 0;
    stub_log_i_count        = 0;
    stub_log_w_count        = 0;
    stub_mutex_takes        = 0;
    stub_mutex_creates      = 0;
    stub_mutex_alive        = 0;
    stub_restart_count      = 0;
}
