#include "GwJson.h"

#include <string.h>

/* jsmn: разбор итеративный, без выделения памяти. Токенов столько, сколько
 * помещается в массив вызывающего; при переполнении возвращается
 * JSMN_ERROR_NOMEM, и мы честно говорим «не разобрал», ничего не ломая.
 *
 * JSMN_PARENT_LINKS не включаем: переход по объекту делается через
 * gw_json_skip (см. ниже), которому достаточно поля size. */
#include "jsmn.h"

uint8_t gw_json_parse(gw_json_t *j, const char *js, size_t len,
                      jsmntok_t *tok, int cap)
{
    if (!j || !js || !tok || cap <= 0)
        return 0;

    jsmn_parser p;
    jsmn_init(&p);
    int n = jsmn_parse(&p, js, len, tok, (unsigned)cap);
    if (n < 1)
        return 0;                    /* не JSON, пусто или токенов не хватило */

    /* Корнем должен быть объект: наш протокол — объект с полями. Массив или
     * число в корне — это не запрос. */
    if (tok[0].type != JSMN_OBJECT)
        return 0;

    j->js    = js;
    j->len   = len;
    j->tok   = tok;
    j->cap   = cap;
    j->count = n;
    return 1;
}

/* Индекс токена, следующего за значением idx со всем его поддеревом.
 * Итеративно: берём значение, добавляем к счётчику его детей и идём вперёд —
 * потомки в jsmn всегда лежат подряд за родителем. */
int gw_json_skip(const gw_json_t *j, int idx)
{
    if (!j || idx < 0 || idx >= j->count)
        return -1;

    int end  = idx + 1;
    int todo = j->tok[idx].size;
    while (todo > 0 && end < j->count) {
        todo--;
        todo += j->tok[end].size;
        end++;
    }
    return end;
}

/* Сравнение ключа токена с именем: длина берётся из токена, поэтому входа
 * с нулём внутри ключа это не ломает. */
static int bKeyIs(const gw_json_t *j, int idx, const char *key)
{
    if (idx < 0 || idx >= j->count || j->tok[idx].type != JSMN_STRING)
        return 0;
    size_t klen = strlen(key);
    size_t tlen = (size_t)(j->tok[idx].end - j->tok[idx].start);
    return (klen == tlen) && (memcmp(j->js + j->tok[idx].start, key, klen) == 0);
}

int gw_json_find(const gw_json_t *j, const char *key)
{
    if (!j || !key || j->count < 1 || j->tok[0].type != JSMN_OBJECT)
        return -1;

    int i = 1;                       /* 0 — сам корневой объект */
    while (i + 1 < j->count) {
        int val = i + 1;             /* пары «ключ: значение» идут подряд */
        if (bKeyIs(j, i, key))
            return val;
        int next = gw_json_skip(j, val);
        if (next <= val)
            return -1;               /* защита от зацикливания на мусоре */
        i = next;
    }
    return -1;
}

/* Разбор целого из среза токена. Своя арифметика вместо strtol: входа без
 * нуля в конце (срез), а strtol ждёт C-строку. */
static uint8_t bTokToInt(const gw_json_t *j, int idx, int32_t *out)
{
    if (idx < 0 || idx >= j->count || j->tok[idx].type != JSMN_PRIMITIVE)
        return 0;

    const char *s = j->js + j->tok[idx].start;
    int len = j->tok[idx].end - j->tok[idx].start;
    if (len <= 0)
        return 0;

    int i = 0, neg = 0;
    if (s[i] == '-') { neg = 1; i++; }
    if (i >= len)
        return 0;

    int64_t v = 0;
    for (; i < len; i++) {
        if (s[i] < '0' || s[i] > '9')
            return 0;                /* true/false/null и дробные нам не числа */
        v = v * 10 + (s[i] - '0');
        if (v > 2147483647LL)
            v = 2147483647LL;        /* дальше только насыщение: значение поля
                                      * всё равно проверяется вызывающим */
    }
    *out = (int32_t)(neg ? -v : v);
    return 1;
}

uint8_t gw_json_int_at(const gw_json_t *j, int idx, int32_t *out)
{
    return out ? bTokToInt(j, idx, out) : 0;
}

uint8_t gw_json_int(const gw_json_t *j, const char *key, int32_t *out)
{
    if (!out)
        return 0;
    int idx = gw_json_find(j, key);
    return (idx >= 0) ? bTokToInt(j, idx, out) : 0;
}

uint8_t gw_json_id(const gw_json_t *j, const char *key, uint32_t *out)
{
    if (!out)
        return 0;

    int idx = gw_json_find(j, key);
    if (idx < 0)
        return 0;

    if (j->tok[idx].type == JSMN_STRING) {
        /* "0x1012" — hex без префикса и без знака */
        const char *s = j->js + j->tok[idx].start;
        int len = j->tok[idx].end - j->tok[idx].start;
        if (len > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
            s += 2;
            len -= 2;
        }
        uint32_t v = 0;
        if (len <= 0 || len > 8)
            return 0;
        for (int i = 0; i < len; i++) {
            char c = s[i];
            uint32_t d;
            if      (c >= '0' && c <= '9') d = (uint32_t)(c - '0');
            else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A' + 10);
            else return 0;
            v = (v << 4) | d;
        }
        *out = v;
        return 1;
    }

    int32_t v;
    if (!bTokToInt(j, idx, &v) || v < 0)
        return 0;
    *out = (uint32_t)v;
    return 1;
}

/* Раскрытие escape-последовательностей JSON. У cJSON это делалось при разборе;
 * здесь строки отдаются как есть, поэтому раскрываем сами — иначе пароль или
 * SSID с кавычкой/обратным слешем поехали бы. */
static uint8_t vCopyString(const char *s, size_t len, char *out, size_t out_size)
{
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (c == '\\' && i + 1 < len) {
            char e = s[++i];
            switch (e) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case '/': c = '/';  break;
            case '"': c = '"';  break;
            case '\\': c = '\\'; break;
            case 'u': {
                /* \uXXXX: только BMP, кодируем в UTF-8 как принято в протоколе */
                if (i + 4 >= len)
                    return 0;
                uint32_t cp = 0;
                for (int k = 1; k <= 4; k++) {
                    char h = s[i + k];
                    uint32_t d;
                    if      (h >= '0' && h <= '9') d = (uint32_t)(h - '0');
                    else if (h >= 'a' && h <= 'f') d = (uint32_t)(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') d = (uint32_t)(h - 'A' + 10);
                    else return 0;
                    cp = (cp << 4) | d;
                }
                i += 4;
                if (cp < 0x80) {
                    if (o + 1 >= out_size) return 0;
                    out[o++] = (char)cp;
                } else if (cp < 0x800) {
                    if (o + 2 >= out_size) return 0;
                    out[o++] = (char)(0xc0 | (cp >> 6));
                    out[o++] = (char)(0x80 | (cp & 0x3f));
                } else {
                    if (o + 3 >= out_size) return 0;
                    out[o++] = (char)(0xe0 | (cp >> 12));
                    out[o++] = (char)(0x80 | ((cp >> 6) & 0x3f));
                    out[o++] = (char)(0x80 | (cp & 0x3f));
                }
                continue;
            }
            default:
                return 0;
            }
        }
        if (o + 1 >= out_size)
            return 0;                /* не влезло — говорим честно */
        out[o++] = c;
    }
    out[o] = '\0';
    return 1;
}

uint8_t gw_json_str(const gw_json_t *j, const char *key, char *out, size_t out_size)
{
    if (!out || out_size == 0)
        return 0;
    out[0] = '\0';

    int idx = gw_json_find(j, key);
    if (idx < 0 || j->tok[idx].type != JSMN_STRING)
        return 0;

    const char *s = j->js + j->tok[idx].start;
    size_t len = (size_t)(j->tok[idx].end - j->tok[idx].start);
    return vCopyString(s, len, out, out_size);
}

uint8_t gw_json_raw(const gw_json_t *j, const char *key, const char **start, size_t *len)
{
    if (!start || !len)
        return 0;

    int idx = gw_json_find(j, key);
    if (idx < 0 || j->tok[idx].type == JSMN_UNDEFINED)
        return 0;

    *start = j->js + j->tok[idx].start;
    *len   = (size_t)(j->tok[idx].end - j->tok[idx].start);
    return 1;
}

uint8_t gw_json_arr_len(const gw_json_t *j, const char *key, int *count)
{
    if (!count)
        return 0;
    int idx = gw_json_find(j, key);
    if (idx < 0 || j->tok[idx].type != JSMN_ARRAY)
        return 0;
    *count = j->tok[idx].size;
    return 1;
}

uint8_t gw_json_arr_int(const gw_json_t *j, const char *key, int idx_want, int32_t *out)
{
    if (!out || idx_want < 0)
        return 0;

    int idx = gw_json_find(j, key);
    if (idx < 0 || j->tok[idx].type != JSMN_ARRAY || idx_want >= j->tok[idx].size)
        return 0;

    int cur = idx + 1;
    for (int k = 0; k < idx_want; k++) {
        int next = gw_json_skip(j, cur);
        if (next <= cur)
            return 0;
        cur = next;
    }
    return bTokToInt(j, cur, out);
}
