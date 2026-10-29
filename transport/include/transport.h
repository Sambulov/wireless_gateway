#pragma once

#include <stdint.h>
#include <stddef.h>

/* Подписка на события — делегат (delegate_t) из CodeLibrary. */
#include "CodeLib.h"

/* ── Контракт транспорта ─────────────────────────────────────────────────────
 *
 * Транспорт доставляет сообщения между удалённой стороной и подписчиком:
 * принимает сообщения целиком (фрагменты склеивает сам) и отдаёт их событием
 * RECEIVED, отправляет асинхронно с событием TRANSFER_COMPLETE/TRANSFER_ERR,
 * сообщает об установке и разрыве соединений.
 *
 * Экземпляр описывается объектом transport_t: методы плюс контекст реализации.
 * У каждого экземпляра свои соединения и свои подписчики. Роль `self` в методах
 * одна — это объект транспорта; что лежит в его context (модуль экземпляра или
 * соединение), знает только реализация.
 */

/* Длина потока неизвестна: транспорт уровня сообщений всегда отдаёт сообщение
 * целиком, поэтому в событиях RECEIVED/COMPLETE/ERR поле left == 0. */
#define TRANSPORT_STREAM_LENGTH  0xffffffffu

typedef enum {
    TRANSPORT_OPEN              = 0x01, /* экземпляр начал принимать соединения */
    TRANSPORT_ESTABLISHED       = 0x02, /* установлено новое соединение */
    TRANSPORT_CLOSED            = 0x04, /* соединение разорвано: ресурсы можно освобождать */
    TRANSPORT_TRANSFER_BUFFER   = 0x10, /* транспорт даёт буфер под отправку */
    TRANSPORT_TRANSFER_COMPLETE = 0x20, /* отправка завершена */
    TRANSPORT_TRANSFER_ERR      = 0x40, /* отправка не удалась */
    TRANSPORT_RECEIVED          = 0x80, /* принято сообщение */
} transport_event_type_t;

typedef enum {
    TRANSPORT_INVALID  = 0x00,
    TRANSPORT_OK       = 0x01,
    TRANSPORT_BUSY     = 0x02,
    TRANSPORT_BAD_CALL = 0x04,
    TRANSPORT_FAULT    = 0x08,
} transport_result_t;

typedef struct {
    transport_event_type_t event;
    union {
        /* RECEIVED: ptr — собранное сообщение целиком, length — его длина,
         *           left == 0 (см. TRANSPORT_STREAM_LENGTH).
         * TRANSFER_COMPLETE/ERR: ptr — ровно тот указатель, который подписчик
         *           отдал в begin_send, length — его длина. Указатель обязан
         *           вернуться неизменным: по нему подписчик находит свой блок.
         * Данные принадлежат транспорту: подписчик их не освобождает и не
         * изменяет, они живут до возврата из обработчика события. */
        struct { const void *ptr; uint32_t length; uint32_t left; } data;
        /* TRANSFER_BUFFER — транспорт предоставляет буфер под заполнение (ответ
         * на begin_send с data == NULL). Подписчик заполняет его ПРЯМО в
         * обработчике события и выставляет fill — сколько байт он положил.
         * size — сколько помещается; если меньше запрошенного, данные уходят
         * частями и событие повторится. */
        struct { void *ptr; uint32_t size; uint32_t fill; } buffer;
    };
} transport_event_trigger_t;

/* Аргументы обработчика делегатные (event_handler_t из CL/Workflow/Event.h):
 *   event_trigger — событие транспорта (const transport_event_trigger_t *);
 *   sender        — дескриптор соединения, о котором событие;
 *   context       — то, что подписчик положил в СВОЙ delegate_t: по нему
 *                   обработчик находит своё состояние (подписчиков бывает
 *                   несколько, общий обработчик на всех не годится).
 *
 * События одного экземпляра поднимаются по одному: event_raise помечает
 * делегатов, которых уже обходит, поэтому поднимать новое событие того же
 * экземпляра из обработчика нельзя — оно не дойдёт до того, кто занят
 * предыдущим. */

typedef struct {
    /* Подготовить экземпляр к работе. config — настройки экземпляра, вид
     * структуры задаёт реализация; зовётся один раз, при создании экземпляра. */
    transport_result_t (*init)(const void *self, const void *config);
    /* Начать принимать соединения и отдавать их подписчику. Вызывается ПОСЛЕ
     * подписки (subscribe): иначе первое соединение отдавать некому. */
    transport_result_t (*open)(const void *self, const void *config);
    /* Разорвать соединение и освободить его ресурсы. */
    transport_result_t (*close)(const void *self);
    /* Начать отправку. Что именно просят — видно по аргументам:
     *   data != NULL            — вот готовое сообщение (length байт), транспорт
     *                             его не копирует: оно должно жить до
     *                             TRANSFER_COMPLETE/TRANSFER_ERR;
     *   data == NULL            — нужен буфер на length байт: транспорт отдаст его
     *                             событием TRANSFER_BUFFER, подписчик заполнит;
     *   length == STREAM_LENGTH — то же, но потоком: буферы выдаются, пока не
     *                             придёт break_sending.
     * Асинхронно: результат приходит событием. TRANSPORT_BUSY — отправка не
     * принята, повторить; другой отказ — тоже повторять нечего, буфер остаётся
     * у подписчика. */
    transport_result_t (*begin_send)(const void *self, const void *data, uint32_t length);
    /* Прервать начатую отправку: недоотправленное бросается, подписчик получает
     * событие ошибки. */
    transport_result_t (*break_sending)(const void *self);
    /* Подписаться на события экземпляра: делегат кладётся в event_t реализации. */
    transport_result_t (*subscribe)(const void *self, delegate_t *delegate);
} transport_iface_t;

/* ── Объект транспорта ──────────────────────────────────────────────────────
 *
 * Методы вместе с контекстом реализации: подписчик получает один объект, и
 * половины от разных экземпляров разойтись не могут.
 *
 * Полей два: чем экземпляр умеет и чей он. */
typedef struct {
    const transport_iface_t *iface;   /* методы транспорта */
    void *context;                    /* контекст реализации; он же идентификатор */
} transport_t;

/* ── Обёртки вызовов ────────────────────────────────────────────────────────
 *
 * Вызов идёт только через них: проверка «объект есть, метод есть» живёт здесь,
 * ошибка в аргументах — TRANSPORT_FAULT.
 *
 * Методу уходит сам объект транспорта; что в его context — знает реализация. */

static inline transport_result_t transport_init(const void *self, const void *config) {
    const transport_t *tr = (const transport_t *)self;
    if (!tr || !tr->iface || !tr->iface->init) return TRANSPORT_FAULT;
    return tr->iface->init(self, config);
}

static inline transport_result_t transport_open(const void *self, const void *config) {
    const transport_t *tr = (const transport_t *)self;
    if (!tr || !tr->iface || !tr->iface->open) return TRANSPORT_FAULT;
    return tr->iface->open(self, config);
}

static inline transport_result_t transport_close(const void *self) {
    const transport_t *tr = (const transport_t *)self;
    if (!tr || !tr->iface || !tr->iface->close) return TRANSPORT_FAULT;
    return tr->iface->close(self);
}

static inline transport_result_t transport_begin_send(const void *self, const void *data, uint32_t length) {
    const transport_t *tr = (const transport_t *)self;
    if (!tr || !tr->iface || !tr->iface->begin_send) return TRANSPORT_FAULT;
    return tr->iface->begin_send(self, data, length);
}

/* Отправить готовое сообщение: транспорт его не копирует, блок живёт до
 * TRANSFER_COMPLETE/TRANSFER_ERR. */
static inline transport_result_t transport_send_zero_copy(const void *self, const void *data,
                                                          uint32_t length) {
    return transport_begin_send(self, data, length);
}

/* Попросить буфер на expected_length байт: транспорт отдаст его событием
 * TRANSFER_BUFFER, заполняет подписчик (в обработчике, с fill). */
static inline transport_result_t transport_send(const void *self, uint32_t expected_length) {
    return transport_begin_send(self, NULL, expected_length);
}

/* То же, но потоком: буферы выдаются, пока не придёт transport_break_sending. */
static inline transport_result_t transport_send_stream(const void *self) {
    return transport_begin_send(self, NULL, TRANSPORT_STREAM_LENGTH);
}

static inline transport_result_t transport_break_sending(const void *self) {
    const transport_t *tr = (const transport_t *)self;
    if (!tr || !tr->iface || !tr->iface->break_sending) return TRANSPORT_FAULT;
    return tr->iface->break_sending(self);
}

static inline transport_result_t transport_subscribe(const void *self, delegate_t *delegate) {
    const transport_t *tr = (const transport_t *)self;
    if (!tr || !tr->iface || !tr->iface->subscribe) return TRANSPORT_FAULT;
    return tr->iface->subscribe(self, delegate);
}
