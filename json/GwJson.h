/* Разбор входящего JSON без выделения памяти.
 *
 * Прежний разбор строил дерево: на каждый объект, массив, ключ и значение —
 * отдельный узел, то есть память росла с ЧИСЛОМ ЭЛЕМЕНТОВ входных данных, а не
 * с их размером. Пакет с большим числом объектов ронял платформу
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

/* Строка: раскрывается экранирование, результат копируется в out и всегда
 * заканчивается нулём (раньше возвращался указатель внутрь дерева разбора). 0 — значение есть, но не влезло в out_size. */
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

/* ── Запись JSON без выделения памяти ──────────────────────────────────────
 *
 * Ответ собирается прямо в буфер, который даёт вызывающий (обычно статический
 * или на стеке): ни дерева, ни узлов, ни печати в строку. Раньше это делал
 * Прежний сборщик делал на каждый объект, ключ и значение отдельный malloc, а
 * результат потом целиком печатался ещё одной аллокацией.
 *
 * Правила простые:
 *   - значение внутри объекта пишется только вместе с ключом (key != NULL);
 *   - значение внутри массива — без ключа (key == NULL);
 *   - в корне допускается ровно одно значение: объект, массив или строка;
 *   - буфер вмещает cap-1 символ, последний байт всегда '\0'.
 *
 * Любая ошибка — переполнение буфера или вызов не по правилам — взводит флаг
 * fail (см. gw_jsonw_ok); за границу буфера не пишется ни один байт. */
#define GW_JSONW_MAX_DEPTH 8

typedef struct {
    char    *buf;                            /* буфер вызывающего */
    size_t   cap;                            /* его размер, включая место под '\0' */
    size_t   len;                            /* сколько уже записано */
    uint8_t  fail;                           /* 1 — не влезло или вызов не по правилам */
    uint8_t  depth;                          /* сколько контейнеров открыто */
    uint8_t  is_arr[GW_JSONW_MAX_DEPTH];     /* 1 — текущий контейнер массив */
    uint8_t  empty[GW_JSONW_MAX_DEPTH];      /* 1 — в контейнере ещё ничего нет */
} gw_jsonw_t;

void    gw_jsonw_init(gw_jsonw_t *w, char *buf, size_t cap);
void    gw_jsonw_reset(gw_jsonw_t *w);       /* начать заново в том же буфере */
uint8_t gw_jsonw_ok(const gw_jsonw_t *w);    /* 1 — всё записано, ошибок не было */
size_t  gw_jsonw_len(const gw_jsonw_t *w);   /* длина записанного без '\0' */

/* Контейнеры. key == NULL — значение без ключа (в массиве или в корне). */
uint8_t gw_jsonw_obj(gw_jsonw_t *w, const char *key);
uint8_t gw_jsonw_arr(gw_jsonw_t *w, const char *key);
uint8_t gw_jsonw_end(gw_jsonw_t *w);         /* закрыть текущий контейнер */

/* Значения. Возвращают 1, если записано, и 0 при ошибке (флаг fail). */
uint8_t gw_jsonw_str(gw_jsonw_t *w, const char *key, const char *val);
uint8_t gw_jsonw_int(gw_jsonw_t *w, const char *key, int64_t val);
uint8_t gw_jsonw_uint(gw_jsonw_t *w, const char *key, uint64_t val);
uint8_t gw_jsonw_bool(gw_jsonw_t *w, const char *key, uint8_t val);
uint8_t gw_jsonw_null(gw_jsonw_t *w, const char *key);

/* Закрыть оставшиеся контейнеры, поставить '\0' и вернуть длину готового
 * документа (w->buf). При ошибке возвращает 0 — смотрите gw_jsonw_ok. */
size_t  gw_jsonw_finish(gw_jsonw_t *w);

#endif /* GW_JSON_H */
