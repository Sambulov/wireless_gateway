#pragma once

#include <stdint.h>

/* Стаб системы для ядра API на хосте.
 *
 * api_system_t — то, что ядро получает в api_init: объект системы и три
 * операции (мьютекс, чтение таймера, журнал). Здесь же живут платформенные
 * функции (platform.h), которыми пользуются сервисы и приложение: на хосте
 * «система» и «платформа» — один и тот же стаб, как в прошивке (main/app.c
 * собирает api_system_t из platform.h). */
#include "api.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Control interface for tests.
 * Call system_stub_reset() in TEST_GROUP setup() to start clean.
 * It also frees every mutex/queue created through the stub since the last
 * reset — anything still pointing at them must be re-created (re-init).
 */

void     system_stub_reset(void);

/* Управляемое время. В стабе 1 тик == 1 мс: и platform_tick, и api_system_t.time_ms
 * отдают этот счётчик, поэтому тест двигает время в миллисекундах. */
void     system_stub_set_time(uint32_t ms);
void     system_stub_advance_time(uint32_t delta_ms);

/* Observability */
int      system_stub_task_created_count(void);
uint32_t system_stub_log_i_count(void);
uint32_t system_stub_log_w_count(void);
/* Сколько раз ядро (или сервис) брало мьютекс через стаб: проверка «ядро не
 * работает без мьютекса» опирается на этот счётчик. */
uint32_t system_stub_mutex_takes(void);
/* Сколько раз мьютекс создавали — по нему видно, что ядро заводит ровно один. */
uint32_t system_stub_mutex_creates(void);
/* Сколько мьютексов создано и ещё не освобождено (после api_deinit остаётся
 * один — системный, ядро его не удаляет). */
uint32_t system_stub_mutex_alive(void);

/* Queue introspection — how many items are currently pending in a queue */
uint32_t system_stub_queue_count(void *queue);

/* Системная таблица для ядра API: мьютекс — заглушка стаба (рекурсивный счётчик
 * глубины), time_ms — управляемый счётчик (system_stub_set_time/advance_time),
 * журнал считает сообщения по уровням. Таблица одна на все ядра теста: она не
 * состояние, а набор операций. */
const api_system_t *system_stub_api_system(void);
/* Сколько миллисекунд кто-то просил подождать (суммарно с последнего сброса) */
uint32_t system_stub_last_delay(void);
/* Сколько раз кто-то просил перезагрузить прибор (esp_restart из esp_system.h:
 * служба прошивки перезагружается в принятый образ). Перезагрузки не
 * происходит — считается вызов. */
uint32_t system_stub_restart_count(void);

#ifdef __cplusplus
}
#endif
