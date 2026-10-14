/* Разбор входящего JSON без выделения памяти.
 *
 * Раньше разбор делал cJSON: на каждый объект, массив, ключ и значение он
 * выделяет отдельный узел, то есть память растёт с ЧИСЛОМ ЭЛЕМЕНТОВ входных
 * данных, а не с их размером. Пакет с большим числом объектов ронял платформу
 * двумя способами (проверено на устройстве):
 *   - разбор рекурсивный, и на глубине ~400 заканчивался стек задачи httpd
 *     (4 КБ) — «stack overflow … Rebooting»;
 *   - вложенные объекты давали панику LoadProhibited.
 *
 * Здесь разбор итеративный и не выделяет память вовсе: jsmn раскладывает вход
 * в массив токенов, который вызывающий держит на своём стеке. Не хватило
 * токенов — отказ (см. gw_json_parse), а не переполнение или удар по куче.
 *
 * Токены jsmn описывают СРЕЗЫ исходной строки, поэтому буфер, переданный в
 * gw_json_parse, должен жить столько же, сколько используются результаты.
 */
#ifndef GW_JSON_H
#define GW_JSON_H

#include <stddef.h>
#include <stdint.h>

/* Только объявления: реализация jsmn компилируется отдельно (jsmn/jsmn.c). */
#define JSMN_HEADER
#include "jsmn.h"

/* Разобранный документ. Массив токенов задаёт вызывающий — обычно на стеке,
 * поэтому число токенов и есть предел сложности входа. */
typedef struct {
    const char *js;
    size_t      len;
    jsmntok_t  *tok;
    int         cap;
    int         count;      /* сколько токенов получилось */
} gw_json_t;

/* Объявить массив токенов нужного размера рядом с gw_json_t. */
#define GW_JSON_TOKENS(name, n) jsmntok_t name[n]

/* Разобрать документ. Возвращает 1 при успехе, 0 если это не JSON или токенов
 * не хватило (cap мал). Никогда не выделяет память и не уходит в рекурсию. */
uint8_t gw_json_parse(gw_json_t *j, const char *js, size_t len,
                      jsmntok_t *tok, int cap);

/* Значение по ключу ВЕРХНЕГО уровня (наш протокол плоский: FID/FLAGS/SID/ARG,
 * внутри ARG — свои поля). Возвращает индекс токена значения или -1. */
int gw_json_find(const gw_json_t *j, const char *key);

/* Число: целое, в том числе отрицательное. 1 — получено. */
uint8_t gw_json_int(const gw_json_t *j, const char *key, int32_t *out);
uint8_t gw_json_int_at(const gw_json_t *j, int idx, int32_t *out);

/* FID в протоколе — число или hex-строка вида "0x1012". */
uint8_t gw_json_id(const gw_json_t *j, const char *key, uint32_t *out);

/* Строка: раскрывается экранирование, результат копируется в out и всегда
 * заканчивается нулём (в отличие от cJSON, который отдавал указатель внутрь
 * дерева). 0 — значение есть, но не влезло в out_size. */
uint8_t gw_json_str(const gw_json_t *j, const char *key, char *out, size_t out_size);

/* Срез значения прямо в исходном тексте — для ARG, который обработчику
 * отдаётся как есть, без промежуточного дерева и печати в строку. */
uint8_t gw_json_raw(const gw_json_t *j, const char *key, const char **start, size_t *len);

/* Массив: длина и элемент по индексу. */
uint8_t gw_json_arr_len(const gw_json_t *j, const char *key, int *count);
uint8_t gw_json_arr_int(const gw_json_t *j, const char *key, int idx, int32_t *out);

/* Сколько токенов заняла бы вся структура значения (нужно, чтобы идти по
 * объекту вперёд, пропуская вложенные значения). */
int gw_json_skip(const gw_json_t *j, int idx);

#endif /* GW_JSON_H */
