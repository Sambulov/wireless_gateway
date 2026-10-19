#pragma once

/* Платформа прошивки: единственное место, где живут системные вызовы (FreeRTOS и
 * ESP-IDF). Ею пользуются приложение и сервисы, но НЕ ядро API: ядро системных
 * вызовов не делает вовсе и получает нужное таблицей api_system_t (api.h),
 * которую приложение собирает из этих же функций (main/app.c).
 *
 * Мьютекс — рекурсивный: ядро берёт свой мьютекс и внутри критической секции
 * зовёт то, что берёт его повторно (отправка ответа из обработчика), а сервис
 * вправе делать то же со своим.
 *
 * Задержка и задача здесь нужны приложению и сервисам; ядру — нет: такт ядру
 * даёт вызывающий (api_worker), а ждать внутри такта оно не имеет права. */

#include <stdint.h>

typedef void *platform_mutex_t;
typedef void *platform_queue_t;

#define PLATFORM_WAIT_FOREVER  UINT32_MAX
#define PLATFORM_WAIT_NONE     0U

/* Mutex */
platform_mutex_t platform_mutex_create(void);
int        platform_mutex_take(platform_mutex_t m, uint32_t timeout_ms);
void       platform_mutex_give(platform_mutex_t m);

/* Queue — внутреннее дело сервиса: ядро очередей не заводит и в них не кладёт */
platform_queue_t platform_queue_create(uint32_t depth, uint32_t item_size);
int        platform_queue_send(platform_queue_t q, const void *item, uint32_t timeout_ms);
int        platform_queue_receive(platform_queue_t q, void *item, uint32_t timeout_ms);

/* Time — monotonic ticks; same unit as FreeRTOS xTaskGetTickCount() on target */
uint32_t   platform_tick(void);
uint32_t   platform_ms_to_ticks(uint32_t ms);
uint32_t   platform_ticks_to_ms(uint32_t ticks);
/* Отдать процессор на ms миллисекунд (в хостовых тестах не спит, а только
 * запоминает запрошенную задержку — см. system_stub_last_delay). */
void       platform_delay_ms(uint32_t ms);

/* Task */
void platform_task_create(void (*fn)(void *), const char *name,
                        uint32_t stack_words, void *arg, int priority);
void platform_task_self_delete(void);

/* Log */
void platform_log_i(const char *tag, const char *fmt, ...);
/* Подробные логи уровня debug: на каждый пакет их слишком много для
 * консоли 115200 — вывод синхронный и тормозит обработку. */
void platform_log_d(const char *tag, const char *fmt, ...);
void platform_log_w(const char *tag, const char *fmt, ...);
