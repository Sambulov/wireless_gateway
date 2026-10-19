/* Разбор входящего JSON: jsmn, без дерева узлов.
 *
 * Разбор итеративный и без выделения памяти: ни на объект, ни на массив, ни на
 * ключ память не выделяется, токены лежат в массиве, который задаёт
 * вызывающий, а их нехватка — это отказ разбора. Поэтому один пакет не может
 * съесть стек или кучу задачи.
 */
#include "CppUTest/TestHarness.h"

extern "C" {
#include "GwJson.h"
}

TEST_GROUP(GwJsonParse) {
    GW_JSON_TOKENS(toks, 48);
    gw_json_t j;

    uint8_t parse(const char *s) {
        return gw_json_parse(&j, s, strlen(s), toks, 48);
    }
};

TEST(GwJsonParse, SimpleFrameFields) {
    const char *s = "{\"FID\":\"uart2.modbus\",\"FLAGS\":0,\"SID\":7,\"ARG\":{\"ADR\":1}}";
    CHECK(parse(s));
    char fid[24];
    int32_t flags = -1, sid = -1;
    CHECK(gw_json_str(&j, "FID", fid, sizeof(fid)));
    STRCMP_EQUAL("uart2.modbus", fid);
    CHECK(gw_json_int(&j, "FLAGS", &flags));
    LONGS_EQUAL(0, flags);
    CHECK(gw_json_int(&j, "SID", &sid));
    LONGS_EQUAL(7, sid);
}

TEST(GwJsonParse, NumericFidIsNotAString) {
    /* Числовой код — не имя: gw_json_str на числе не срабатывает, и такой кадр
     * отклоняется. */
    CHECK(parse("{\"FID\":4115,\"FLAGS\":0}"));
    char fid[24];
    CHECK_FALSE(gw_json_str(&j, "FID", fid, sizeof(fid)));
}

TEST(GwJsonParse, MissingKeyIsNotFound) {
    CHECK(parse("{\"FID\":1}"));
    int32_t v = 0;
    CHECK_FALSE(gw_json_int(&j, "FLAGS", &v));
    LONGS_EQUAL(-1, gw_json_find(&j, "FLAGS"));
}

TEST(GwJsonParse, ArgSlicePointsIntoSource) {
    const char *s = "{\"FID\":1,\"ARG\":{\"ADR\":1,\"FN\":3}}";
    CHECK(parse(s));
    const char *start = NULL;
    size_t len = 0;
    CHECK(gw_json_raw(&j, "ARG", &start, &len));
    STRCMP_EQUAL("{\"ADR\":1,\"FN\":3}", std::string(start, len).c_str());
}

TEST(GwJsonParse, StringWithEscapes) {
    const char *s = "{\"SSID\":\"a\\\"b\\\\c\\n\",\"PASS\":\"p\"}";
    CHECK(parse(s));
    char out[32];
    CHECK(gw_json_str(&j, "SSID", out, sizeof(out)));
    STRCMP_EQUAL("a\"b\\c\n", out);
}

TEST(GwJsonParse, StringTooLongIsRefused) {
    const char *s = "{\"PASS\":\"0123456789\"}";
    CHECK(parse(s));
    char out[4];
    CHECK_FALSE(gw_json_str(&j, "PASS", out, sizeof(out)));
}

TEST(GwJsonParse, IntArrayValues) {
    const char *s = "{\"RD\":[1,2,-3,4000]}";
    CHECK(parse(s));
    int n = 0;
    CHECK(gw_json_arr_len(&j, "RD", &n));
    LONGS_EQUAL(4, n);
    int32_t v = 0;
    CHECK(gw_json_arr_int(&j, "RD", 0, &v)); LONGS_EQUAL(1, v);
    CHECK(gw_json_arr_int(&j, "RD", 2, &v)); LONGS_EQUAL(-3, v);
    CHECK(gw_json_arr_int(&j, "RD", 3, &v)); LONGS_EQUAL(4000, v);
    CHECK_FALSE(gw_json_arr_int(&j, "RD", 4, &v));
}

TEST(GwJsonParse, NestedValueDoesNotBreakNextKey) {
    /* Вложенное значение пропускается целиком, поэтому следующий ключ
     * находится по смещению, а не «на глаз». */
    const char *s = "{\"DEEP\":{\"a\":[1,[2,[3]]]},\"FLAGS\":4}";
    CHECK(parse(s));
    int32_t flags = 0;
    CHECK(gw_json_int(&j, "FLAGS", &flags));
    LONGS_EQUAL(4, flags);
}

TEST(GwJsonParse, DeepNestingIsRefusedWithoutCrash) {
    /* Ровно то, что роняло платформу: 500 уровней вложенности. Разбор
     * итеративный, стек не растёт — просто не хватает токенов. */
    std::string s = "{\"ARG\":";
    s.append(500, '[');
    s.append(500, ']');
    s += "}";
    CHECK_FALSE(gw_json_parse(&j, s.c_str(), s.size(), toks, 48));
}

TEST(GwJsonParse, TooManyElementsIsRefused) {
    std::string s = "{\"RD\":[";
    for (int i = 0; i < 200; i++) s += (i ? ",1" : "1");
    s += "]}";
    CHECK_FALSE(gw_json_parse(&j, s.c_str(), s.size(), toks, 48));
}

TEST(GwJsonParse, NotJsonIsRefused) {
    CHECK_FALSE(parse("не json"));
    CHECK_FALSE(parse(""));
    CHECK_FALSE(parse("[1,2,3]"));   /* корень обязан быть объектом */
}

TEST(GwJsonParse, EmptyArgObjectIsParsed) {
    CHECK(parse("{\"FID\":16,\"FLAGS\":0,\"ARG\":{}}"));
    const char *start = NULL;
    size_t len = 0;
    CHECK(gw_json_raw(&j, "ARG", &start, &len));
    LONGS_EQUAL(2, len);
}

/* ── Запись JSON ─────────────────────────────────────────────────────────── */

TEST_GROUP(GwJsonWrite) {
    char buf[256];
    gw_jsonw_t w;

    void setup() { gw_jsonw_init(&w, buf, sizeof(buf)); }
    const char *out() { return buf; }
};

TEST(GwJsonWrite, FlatObjectMatchesPreviousOutput) {
    /* Плоский объект печатается ровно так:
     * {"STA":"0x00000000","SSID":"HomeNet","CLIENTS":1,"FROM_AP":true} */
    gw_jsonw_obj(&w, NULL);
    gw_jsonw_str(&w, "STA", "0x00000000");
    gw_jsonw_str(&w, "SSID", "HomeNet");
    gw_jsonw_int(&w, "CLIENTS", 1);
    gw_jsonw_bool(&w, "FROM_AP", 1);
    gw_jsonw_finish(&w);
    STRCMP_EQUAL("{\"STA\":\"0x00000000\",\"SSID\":\"HomeNet\",\"CLIENTS\":1,\"FROM_AP\":true}",
                 out());
}

TEST(GwJsonWrite, NestedObjects) {
    gw_jsonw_obj(&w, NULL);
    gw_jsonw_str(&w, "STA", "0x00000000");
    gw_jsonw_obj(&w, "STA_CFG");
    gw_jsonw_str(&w, "MODE", "dhcp");
    gw_jsonw_str(&w, "IP", "0.0.0.0");
    gw_jsonw_end(&w);
    gw_jsonw_obj(&w, "AP_CFG");
    gw_jsonw_str(&w, "MODE", "static");
    gw_jsonw_end(&w);
    gw_jsonw_finish(&w);
    STRCMP_EQUAL("{\"STA\":\"0x00000000\",\"STA_CFG\":{\"MODE\":\"dhcp\",\"IP\":\"0.0.0.0\"},"
                 "\"AP_CFG\":{\"MODE\":\"static\"}}", out());
}

TEST(GwJsonWrite, ArrayOfObjects) {
    gw_jsonw_obj(&w, NULL);
    gw_jsonw_arr(&w, "FILES");
    gw_jsonw_obj(&w, NULL);
    gw_jsonw_str(&w, "NAME", "a.jsonc");
    gw_jsonw_int(&w, "SIZE", 1234);
    gw_jsonw_bool(&w, "DIR", 0);
    gw_jsonw_end(&w);
    gw_jsonw_obj(&w, NULL);
    gw_jsonw_str(&w, "NAME", "regmaps");
    gw_jsonw_int(&w, "SIZE", 0);
    gw_jsonw_bool(&w, "DIR", 1);
    gw_jsonw_end(&w);
    gw_jsonw_end(&w);
    gw_jsonw_finish(&w);
    STRCMP_EQUAL("{\"FILES\":[{\"NAME\":\"a.jsonc\",\"SIZE\":1234,\"DIR\":false},"
                 "{\"NAME\":\"regmaps\",\"SIZE\":0,\"DIR\":true}]}", out());
}

TEST(GwJsonWrite, EmptyContainers) {
    gw_jsonw_obj(&w, NULL);
    gw_jsonw_obj(&w, "O");
    gw_jsonw_end(&w);
    gw_jsonw_arr(&w, "A");
    gw_jsonw_end(&w);
    gw_jsonw_null(&w, "N");
    gw_jsonw_finish(&w);
    STRCMP_EQUAL("{\"O\":{},\"A\":[],\"N\":null}", out());
}

TEST(GwJsonWrite, EscapesQuotesBackslashAndControlChars) {
    /* Управляющие символы — короткими последовательностями, остальные
     * \u00xx в нижнем регистре. */
    gw_jsonw_obj(&w, NULL);
    /* "\x01" отдельной строкой: \x01f компилятор прочитал бы как \x1f */
    gw_jsonw_str(&w, "S", "a\"b\\c\nd\te" "\x01" "f");
    gw_jsonw_finish(&w);
    STRCMP_EQUAL("{\"S\":\"a\\\"b\\\\c\\nd\\te\\u0001f\"}", out());
}

TEST(GwJsonWrite, NonAsciiIsWrittenAsUtf8Bytes) {
    /* Кириллица пишется байтами как есть: ответ не раздувается, а клиент после
     * JSON.parse получает ровно ту же строку. */
    gw_jsonw_obj(&w, NULL);
    gw_jsonw_str(&w, "N", "тест");
    gw_jsonw_finish(&w);
    STRCMP_EQUAL("{\"N\":\"тест\"}", out());
}

TEST(GwJsonWrite, NonBmpIsWrittenAsUtf8Bytes) {
    /* Символ вне BMP (U+1F600) — четыре байта как есть, суррогатной пары нет */
    gw_jsonw_obj(&w, NULL);
    gw_jsonw_str(&w, "E", "\xf0\x9f\x98\x80");
    gw_jsonw_finish(&w);
    STRCMP_EQUAL("{\"E\":\"\xf0\x9f\x98\x80\"}", out());
}

TEST(GwJsonWrite, InvalidUtf8ByteIsEscaped) {
    /* Байт, не образующий UTF-8 (одиночный старший байт: имя файла в чужой
     * кодировке), экранируется — иначе в ответе была бы невалидная UTF-8. */
    gw_jsonw_obj(&w, NULL);
    gw_jsonw_str(&w, "N", "\xe9x");
    gw_jsonw_finish(&w);
    STRCMP_EQUAL("{\"N\":\"\\u00e9x\"}", out());
}

TEST(GwJsonWrite, TruncatedSequenceIsEscaped) {
    /* Обрезанная на конце последовательность (только первый байт «т») */
    gw_jsonw_obj(&w, NULL);
    gw_jsonw_str(&w, "N", "\xd1");
    gw_jsonw_finish(&w);
    STRCMP_EQUAL("{\"N\":\"\\u00d1\"}", out());
}

TEST(GwJsonWrite, OverlongAndSurrogateSequencesAreEscaped) {
    /* Переросшая (C0 AF) и суррогат в UTF-8 (ED A0 80) корректными не считаются:
     * каждый байт экранируется, в ответе остаётся валидная UTF-8. */
    gw_jsonw_obj(&w, NULL);
    gw_jsonw_str(&w, "A", "\xc0\xaf");
    gw_jsonw_str(&w, "B", "\xed\xa0\x80");
    gw_jsonw_finish(&w);
    STRCMP_EQUAL("{\"A\":\"\\u00c0\\u00af\",\"B\":\"\\u00ed\\u00a0\\u0080\"}",
                 out());
}

TEST(GwJsonWrite, NonAsciiRoundTripsThroughReader) {
    /* Что записали, то и прочитали: и байтами, и обратно. */
    gw_jsonw_obj(&w, NULL);
    gw_jsonw_str(&w, "N", "Кириллица и 😀");
    size_t len = gw_jsonw_finish(&w);

    GW_JSON_TOKENS(toks, 16);
    gw_json_t j;
    CHECK(gw_json_parse(&j, buf, len, toks, 16));
    char back[64];
    CHECK(gw_json_str(&j, "N", back, sizeof(back)));
    STRCMP_EQUAL("Кириллица и 😀", back);
}

TEST(GwJsonWrite, NumbersSignedAndLarge) {
    gw_jsonw_obj(&w, NULL);
    gw_jsonw_int(&w, "A", -1);
    gw_jsonw_int(&w, "B", 0);
    gw_jsonw_uint(&w, "C", 4294967295u);
    gw_jsonw_int(&w, "D", 1500000);
    gw_jsonw_finish(&w);
    STRCMP_EQUAL("{\"A\":-1,\"B\":0,\"C\":4294967295,\"D\":1500000}", out());
}

TEST(GwJsonWrite, BareStringAtRoot) {
    /* Ответ UART RAW_RX — это голая строка JSON, не объект */
    gw_jsonw_str(&w, NULL, "QUJD");
    LONGS_EQUAL(6, gw_jsonw_finish(&w));
    STRCMP_EQUAL("\"QUJD\"", out());
}

TEST(GwJsonWrite, OverflowSetsFlagAndStopsWriting) {
    char small[16];
    gw_jsonw_t s;
    gw_jsonw_init(&s, small, sizeof(small));
    gw_jsonw_obj(&s, NULL);
    CHECK_FALSE(gw_jsonw_str(&s, "KEY", "0123456789"));   /* не влезло — сразу отказ */
    CHECK_FALSE(gw_jsonw_ok(&s));
    LONGS_EQUAL(0, gw_jsonw_finish(&s));
    LONGS_EQUAL(15, (long)strlen(small));    /* буфер не переполнен */
}

TEST(GwJsonWrite, KeyRequiredInsideObject) {
    gw_jsonw_obj(&w, NULL);
    CHECK_FALSE(gw_jsonw_str(&w, NULL, "x"));   /* значение без ключа в объекте */
    CHECK_FALSE(gw_jsonw_ok(&w));
}

TEST(GwJsonWrite, KeyRejectedInsideArray) {
    gw_jsonw_arr(&w, NULL);
    CHECK_FALSE(gw_jsonw_str(&w, "K", "x"));    /* ключ в массиве */
    CHECK_FALSE(gw_jsonw_ok(&w));
}

TEST(GwJsonWrite, SecondRootValueRejected) {
    gw_jsonw_obj(&w, NULL);
    gw_jsonw_end(&w);
    CHECK_FALSE(gw_jsonw_str(&w, NULL, "x"));
    CHECK_FALSE(gw_jsonw_ok(&w));
}

TEST(GwJsonWrite, TooDeepNestingIsRefused) {
    for (int i = 0; i < GW_JSONW_MAX_DEPTH + 2; i++)
        gw_jsonw_obj(&w, i ? "o" : NULL);
    CHECK_FALSE(gw_jsonw_ok(&w));
}

TEST(GwJsonWrite, FinishClosesOpenContainers) {
    gw_jsonw_obj(&w, NULL);
    gw_jsonw_arr(&w, "A");
    gw_jsonw_int(&w, NULL, 1);
    gw_jsonw_finish(&w);                     /* ничего не закрывали руками */
    STRCMP_EQUAL("{\"A\":[1]}", out());
}

TEST(GwJsonWrite, ResetReusesBuffer) {
    gw_jsonw_obj(&w, NULL);
    gw_jsonw_str(&w, "A", "1");
    gw_jsonw_finish(&w);
    gw_jsonw_reset(&w);
    gw_jsonw_obj(&w, NULL);
    gw_jsonw_str(&w, "B", "2");
    gw_jsonw_finish(&w);
    STRCMP_EQUAL("{\"B\":\"2\"}", out());
}

TEST(GwJsonWrite, DuplicateKeyIsAppendedNotReplaced) {
    /* Повторный ключ добавляется вторым, а не заменяет первый. Листинг файлов
     * на ошибке пути отдаёт STA дважды, и клиент (JSON.parse) видит последнее
     * значение — так и задумано. */
    gw_jsonw_obj(&w, NULL);
    gw_jsonw_str(&w, "STA", "0x00000000");
    gw_jsonw_uint(&w, "TOTAL", 1048576);
    gw_jsonw_str(&w, "ERR", "PATH");
    gw_jsonw_str(&w, "STA", "0x80000003");
    gw_jsonw_finish(&w);
    STRCMP_EQUAL("{\"STA\":\"0x00000000\",\"TOTAL\":1048576,\"ERR\":\"PATH\","
                 "\"STA\":\"0x80000003\"}", out());
}
