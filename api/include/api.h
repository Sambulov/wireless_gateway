#ifndef NWWEB_H_INCLUDED
#define NWWEB_H_INCLUDED

#include <stdint.h>
#include <stddef.h>

/* Заголовков ESP-IDF здесь нет: подъём ядра возвращает указатель, а не код
 * ошибки IDF (см. api_init). Файлы, которым код ошибки нужен сами, включают его
 * сами.
 *
 * platform.h здесь ТОЖЕ не включается: ядро не знает ни платформенных имён, ни
 * FreeRTOS, ни ESP-IDF. Всё, что ядру нужно от системы, приходит таблицей
 * api_system_t (ниже), а платформа (мьютексы, очереди, время, задачи, журнал)
 * остаётся отдельным компонентом platform/ для приложения и сервисов. */
#include "transport.h"

/* ── Дескриптор ядра ────────────────────────────────────────────────────────
 *
 * Ядро API — экземпляр: api_init заводит дескриптор и возвращает его указателем,
 * а вызывающий хранит этот указатель и передаёт его функциям ядра. Сколько ядер
 * поднято — столько дескрипторов; в прошивке их два (точка доступа и станция), и
 * общего у них нет ничего.
 *
 * Раскладка состояния наружу не раскрывается: снаружи только неполный тип, всё
 * остальное — дело api/src/api_core.c. Размер дескриптора снаружи знать незачем:
 * его выделяет и освобождает само ядро. */
typedef struct api_s api_t;

/* ── Что ядру нужно от системы ──────────────────────────────────────────────
 *
 * Мьютекс, время и журнал. Мьютекс должен быть рекурсивным: обработчик зовёт
 * api_call_send_* изнутри такта. log — varargs, уровни API_LOG_*. */
typedef struct {
    void *context;                                        /* объект системы */
    void *(*mutex_create)(void *context);
    void  (*mutex_take)(void *context, void *mutex);
    void  (*mutex_give)(void *context, void *mutex);
    uint32_t (*time_ms)(void *context);                    /* чтение таймера */
    void  (*log)(void *context, int level, const char *tag, const char *fmt, ...);
} api_system_t;

/* Уровни журнала ядра; система переводит их в свои (см. main/app.c). */
#define API_LOG_DEBUG  0
#define API_LOG_INFO   1
#define API_LOG_WARN   2

/* Период пульса, мс: вызов обработчиков активных вызовов с data == NULL.
 * Паузу держит тот, кто зовёт api_worker. */
#define API_PULSE_INTERVAL_MS 10

/* FID — имя метода вида "<подсистема>.<действие>", до 31 символа. */
#define API_FID_NAME_MAX 32

/* Возврат обработчика. */
#define HDL_FALSE 0
#define HDL_TRUE  1

/* ── Статусы ────────────────────────────────────────────────────────────────
 *
 * Само ядро отправляет только BAD_REQ, NO_FID и NO_HANDLER. Подтверждения
 * подписки, отписки и приёма данных отправляет метод. */
#define API_CALL_STATUS_COMPLETE                    0x00000000
#define API_CALL_STATUS_EXECUTING                   0x00000001
#define API_CALL_STATUS_CANCELED                    0x00000002
#define API_CALL_STATUS_BUSY                        0x00000003
#define API_CALL_STATUS_INVALID                     0x00000004
#define API_CALL_STATUS_DELIVERED                   0x00000005
#define API_CALL_STATUS_ACCEPTED                    0x00000006
#define API_CALL_STATUS_REJECTED                    0x00000007
#define API_CALL_STATUS_PARTIAL                     0x00000008

#define API_CALL_ERROR_STATUS_BAD_REQ               0x80000000
#define API_CALL_ERROR_STATUS_FRAGMENTED            0x80000001
#define API_CALL_ERROR_STATUS_NO_FID                0x80000002
#define API_CALL_ERROR_STATUS_BAD_ARG               0x80000003
#define API_CALL_ERROR_STATUS_NO_FREE_DESCRIPTORS   0x80000004
#define API_CALL_ERROR_STATUS_TIMEOUT               0x80000005
#define API_CALL_ERROR_STATUS_NO_MEM                0x80000006
#define API_CALL_ERROR_STATUS_NO_ACCESS             0x80000007
#define API_CALL_ERROR_STATUS_NOT_FOUND             0x80000008
#define API_CALL_ERROR_STATUS_NO_HANDLER            0x8000000E
#define API_CALL_ERROR_STATUS_INTERNAL              0x8000000F

/*!
	@brief Обработчик вызова API.

	Неблокирующий: ждать периферию нельзя — вернуть HDL_TRUE и завершить позже.

	Возврат — это и есть завершение:
	  * HDL_FALSE — вызов завершён, дескриптор освобождается;
	  * HDL_TRUE  — вызов продолжается и получает пульс (data == NULL) каждый такт.

	@param[in] call      дескриптор вызова; NULL — принудительное завершение
	                     (клиент ушёл): освободить контекст и вернуть HDL_FALSE,
	                     статус не шлётся. Вне обработчика не сохранять.
	@param[in,out] context контекст вызова: при первом кадре — родной контекст
	                     регистрации, дальше — тот, что поставил обработчик.
	@param[in] data      для кадров клиента не NULL (нет ARG — пустая нагрузка,
	                     data_len == 0); NULL при живом call — пульс.
	@param[in] data_len  длина data.
	@return HDL_TRUE / HDL_FALSE.
*/
typedef uint8_t (*api_handler_t)(void *call, void **context, uint8_t *data,
                                 uint32_t data_len);

/* ── Регистрация методов ────────────────────────────────────────────────────
 *
 * Повторная регистрация обновляет обработчик и родной контекст; NULL в handler
 * снимает метод (новые запросы получают NO_HANDLER, активные вызовы доживают).
 * Ядер в прошивке два, реестр у каждого свой — отсюда api_t первым аргументом.
 * Возврат: 1 — есть; 0 — пустой или слишком длинный fid. */
uint8_t api_call_register(api_t *api, api_handler_t handler, const char *fid,
                          void *context);

/* Снятие метода: тот же вызов с пустым обработчиком. */
static inline uint8_t api_call_unregister(api_t *api, const char *fid) {
    return api_call_register(api, NULL, fid, NULL);
}

/* ── Подъём и снятие ядра ───────────────────────────────────────────────────
 *
 * api_init создаёт ядро из двух объектов: транспорта (подписка на его события)
 * и системы (мьютекс, время, журнал), и открывает транспорт: transport_config
 * уходит в open. NULL — поднять не удалось. Задачу ядро не заводит: такт даёт
 * api_worker. */
api_t *api_init(const transport_t *transport, void *transport_config,
                const api_system_t *system);
/* Снять ядро: отписаться от транспорта и освободить всё своё. */
void   api_deinit(api_t *api);

/* Один такт: снять мёртвое соединение, одно принудительное завершение, пульс
 * раз в API_PULSE_INTERVAL_MS, разобрать входящие кадры. Такт не ждёт. */
void   api_worker(api_t *api);

/* ── Отправка кадров ────────────────────────────────────────────────────────
 *
 * Кадр собирает ядро: {"FID":"…","SID":"0x…","ARG":<json>} или
 * {"FID":"…","SID":"0x…","STA":"0x…"}. Возврат: HDL_TRUE — кадр ушёл в
 * транспорт, HDL_FALSE — собрать не удалось или клиента уже нет. */
/* Статус конкретному вызову. */
uint8_t api_call_send_status(void *call, uint32_t sta);
/* Кадр данных из готового тела; пустое тело уходит как null. */
uint8_t api_call_send_arg(void *call, const char *arg, uint32_t len);
/* Кадр под ответ: резервирует буфер и возвращает область ARG под заполнение.
 * NULL — не хватило памяти. */
char *api_call_begin_response(uint32_t arg_len);
/* Освободить подготовленный, но не отправленный ответ. */
void api_call_cancel_response(char *resp);
/* Закрыть кадр (дописав FID/SID) и отдать транспорту; освобождать не нужно. */
uint8_t api_call_send_response(void *call, char *resp, uint32_t arg_len);

/* Публичная рассылка: кадр данных уходит каждому активному вызову с данным FID,
 * БЕЗ SID (номер подписки клиент знает сам — он его и выбрал). Конверт
 * собирается один раз на всех получателей. */
uint8_t api_call_group_fid_data(api_t *api, const char *fid, const char *data,
                                uint32_t len);
/* То же для тела, уже собранного через api_call_begin_response: один буфер
 * отдаётся всем получателям, освобождает его последний из них. */
uint8_t api_call_group_response(api_t *api, const char *fid, void *resp,
                                uint32_t arg_len);

#endif /* NWWEB_H_INCLUDED */
