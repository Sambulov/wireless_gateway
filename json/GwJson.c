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

/* Раскрытие escape-последовательностей JSON. Раньше это делалось при разборе;
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

/* ── Запись JSON ─────────────────────────────────────────────────────────── *
 * U+XXXX печатается в нижнем регистре (как %04x), чтобы ответы по возможности
 * совпадали с прежними байт в байт. */

static void w_ch(gw_jsonw_t *w, char c)
{
    if (w->fail)
        return;
    if (w->len + 1 >= w->cap) {          /* место под завершающий '\0' держим всегда */
        w->fail = 1;
        return;
    }
    w->buf[w->len++] = c;
}

static void w_mem(gw_jsonw_t *w, const char *s, size_t n)
{
    if (w->fail || n == 0)
        return;
    if (w->len + n >= w->cap) {
        w->fail = 1;
        return;
    }
    memcpy(w->buf + w->len, s, n);
    w->len += n;
}

static void w_hex4(gw_jsonw_t *w, uint32_t v)
{
    static const char digits[] = "0123456789abcdef";
    w_ch(w, 'u');
    w_ch(w, digits[(v >> 12) & 0xf]);
    w_ch(w, digits[(v >> 8) & 0xf]);
    w_ch(w, digits[(v >> 4) & 0xf]);
    w_ch(w, digits[v & 0xf]);
}

/* Длина корректной последовательности UTF-8, начинающейся с байта p (доступно
 * n байт), либо 0 — байт не начинает корректную последовательность. Проверка
 * строгая: обрезанные, переросшие (overlong) последовательности и суррогаты
 * (U+D800..U+DFFF, которых в UTF-8 быть не должно) корректными не считаются. */
static int u_seq_len(const unsigned char *p, size_t n)
{
    if (p[0] < 0x80)
        return 1;
    if (p[0] >= 0xc2 && p[0] <= 0xdf)
        return (n >= 2 && (p[1] & 0xc0) == 0x80) ? 2 : 0;
    if (p[0] >= 0xe0 && p[0] <= 0xef) {
        if (n < 3 || (p[1] & 0xc0) != 0x80 || (p[2] & 0xc0) != 0x80)
            return 0;
        if (p[0] == 0xe0 && p[1] < 0xa0) return 0;      /* переросшая */
        if (p[0] == 0xed && p[1] > 0x9f) return 0;      /* суррогат */
        return 3;
    }
    if (p[0] >= 0xf0 && p[0] <= 0xf4) {
        if (n < 4 || (p[1] & 0xc0) != 0x80 || (p[2] & 0xc0) != 0x80 ||
            (p[3] & 0xc0) != 0x80)
            return 0;
        if (p[0] == 0xf0 && p[1] < 0x90) return 0;      /* переросшая */
        if (p[0] == 0xf4 && p[1] > 0x8f) return 0;      /* выше U+10FFFF */
        return 4;
    }
    return 0;
}

/* Строка в кавычках с экранированием. Кавычка, обратный слеш и управляющие
 * символы — короткими последовательностями, остальные управляющие — \u00xx.
 * Корректный UTF-8 пишется БАЙТАМИ КАК ЕСТЬ, как это делал прежний сборщик JSON:
 * имена файлов, SSID и пароли с кириллицей не раздуваются, а клиент получает по
 * JSON ровно те же символы. Экранируется только байт, не образующий корректной
 * последовательности: иначе в ответе оказалась бы невалидная UTF-8, а текст
 * JSON обязан быть корректным UTF-8. */

static void w_quoted(gw_jsonw_t *w, const char *s, size_t n)
{
    w_ch(w, '"');
    for (size_t i = 0; i < n && !w->fail; ) {
        unsigned char c = (unsigned char)s[i];

        if (c < 0x20 || c == '"' || c == '\\') {
            switch (c) {
            case '"':  w_mem(w, "\\\"", 2); break;
            case '\\': w_mem(w, "\\\\", 2); break;
            case '\b': w_mem(w, "\\b", 2);  break;
            case '\f': w_mem(w, "\\f", 2);  break;
            case '\n': w_mem(w, "\\n", 2);  break;
            case '\r': w_mem(w, "\\r", 2);  break;
            case '\t': w_mem(w, "\\t", 2);  break;
            default:
                w_mem(w, "\\", 1);
                w_hex4(w, c);
                break;
            }
            i++;
            continue;
        }

        if (c < 0x80) {
            w_ch(w, (char)c);
            i++;
            continue;
        }

        int used = u_seq_len((const unsigned char *)s + i, n - i);
        if (used > 1) {                   /* корректный UTF-8 — байты как есть */
            w_mem(w, s + i, (size_t)used);
            i += (size_t)used;
            continue;
        }

        /* Байт не образует корректного UTF-8 (обрезанная последовательность,
         * суррогат, одиночный старший байт) — экранируем его одного и идём дальше. */
        w_mem(w, "\\", 1);
        w_hex4(w, c);
        i++;
    }
    w_ch(w, '"');
}

void gw_jsonw_init(gw_jsonw_t *w, char *buf, size_t cap)
{
    if (!w)
        return;
    memset(w, 0, sizeof(*w));
    w->buf = buf;
    w->cap = cap;
    if (!buf || cap == 0)
        w->fail = 1;
}

void gw_jsonw_reset(gw_jsonw_t *w)
{
    if (!w)
        return;
    w->len  = 0;
    w->fail = (w->buf && w->cap) ? 0 : 1;
    w->depth = 0;
    memset(w->is_arr, 0, sizeof(w->is_arr));
    memset(w->empty, 0, sizeof(w->empty));
    if (w->buf && w->cap)
        w->buf[0] = '\0';
}

uint8_t gw_jsonw_ok(const gw_jsonw_t *w)
{
    return (w && !w->fail) ? 1 : 0;
}

size_t gw_jsonw_len(const gw_jsonw_t *w)
{
    return w ? w->len : 0;
}

/* Начало очередного значения: запятая, ключ и двоеточие, если они нужны. */
static uint8_t w_value_start(gw_jsonw_t *w, const char *key)
{
    if (!w || w->fail)
        return 0;

    if (w->depth == 0) {
        if (w->len != 0) {                /* в корне допустимо одно значение */
            w->fail = 1;
            return 0;
        }
        return 1;
    }

    uint8_t top = (uint8_t)(w->depth - 1);
    if (!w->empty[top])
        w_ch(w, ',');
    w->empty[top] = 0;

    if (w->is_arr[top]) {
        if (key) {                        /* в массиве ключей не бывает */
            w->fail = 1;
            return 0;
        }
        return 1;
    }

    if (!key) {                           /* в объекте значение идёт только с ключом */
        w->fail = 1;
        return 0;
    }
    w_quoted(w, key, strlen(key));
    w_ch(w, ':');
    return w->fail ? 0 : 1;
}

static uint8_t w_push(gw_jsonw_t *w, char open, uint8_t is_arr)
{
    if (w->depth >= GW_JSONW_MAX_DEPTH) {
        w->fail = 1;
        return 0;
    }
    w_ch(w, open);
    w->is_arr[w->depth] = is_arr;
    w->empty[w->depth]  = 1;
    w->depth++;
    return w->fail ? 0 : 1;
}

uint8_t gw_jsonw_obj(gw_jsonw_t *w, const char *key)
{
    if (!w_value_start(w, key))
        return 0;
    return w_push(w, '{', 0);
}

uint8_t gw_jsonw_arr(gw_jsonw_t *w, const char *key)
{
    if (!w_value_start(w, key))
        return 0;
    return w_push(w, '[', 1);
}

uint8_t gw_jsonw_end(gw_jsonw_t *w)
{
    if (!w || w->fail)
        return 0;
    if (w->depth == 0) {
        w->fail = 1;
        return 0;
    }
    w->depth--;
    w_ch(w, w->is_arr[w->depth] ? ']' : '}');
    return w->fail ? 0 : 1;
}

uint8_t gw_jsonw_str(gw_jsonw_t *w, const char *key, const char *val)
{
    if (!w_value_start(w, key))
        return 0;
    if (!val)
        val = "";
    w_quoted(w, val, strlen(val));
    return w->fail ? 0 : 1;
}

static void w_number(gw_jsonw_t *w, const char *key, uint64_t mag, uint8_t neg)
{
    char tmp[24];
    int  n = 0;

    do {
        tmp[n++] = (char)('0' + (mag % 10));
        mag /= 10;
    } while (mag != 0 && n < (int)sizeof(tmp) - 1);
    if (neg && n < (int)sizeof(tmp))
        tmp[n++] = '-';

    if (!w_value_start(w, key))
        return;
    while (n > 0)
        w_ch(w, tmp[--n]);
}

uint8_t gw_jsonw_int(gw_jsonw_t *w, const char *key, int64_t val)
{
    if (!w || w->fail)
        return 0;
    uint64_t mag = (val < 0) ? ((uint64_t)(-(val + 1)) + 1u) : (uint64_t)val;
    w_number(w, key, mag, (val < 0) ? 1 : 0);
    return w->fail ? 0 : 1;
}

uint8_t gw_jsonw_uint(gw_jsonw_t *w, const char *key, uint64_t val)
{
    if (!w || w->fail)
        return 0;
    w_number(w, key, val, 0);
    return w->fail ? 0 : 1;
}

uint8_t gw_jsonw_bool(gw_jsonw_t *w, const char *key, uint8_t val)
{
    if (!w_value_start(w, key))
        return 0;
    if (val)
        w_mem(w, "true", 4);
    else
        w_mem(w, "false", 5);
    return w->fail ? 0 : 1;
}

uint8_t gw_jsonw_null(gw_jsonw_t *w, const char *key)
{
    if (!w_value_start(w, key))
        return 0;
    w_mem(w, "null", 4);
    return w->fail ? 0 : 1;
}

size_t gw_jsonw_finish(gw_jsonw_t *w)
{
    if (!w)
        return 0;
    while (w->depth > 0 && !w->fail)
        gw_jsonw_end(w);
    if (w->buf && w->cap) {
        if (w->len >= w->cap) {
            w->fail = 1;
            w->len  = w->cap - 1;
        }
        w->buf[w->len] = '\0';
    }
    return w->fail ? 0 : w->len;
}
