/* Разбор входящего JSON: jsmn вместо cJSON.
 *
 * cJSON выделял память на каждый объект, массив и ключ, а разбор был
 * рекурсивным — на устройстве это давало падение от одного пакета
 * (переполнение стека задачи httpd на глубине ~400 и паника на вложенных
 * объектах). Здесь разбор итеративный и без выделения памяти: токены лежат в
 * массиве, который задаёт вызывающий, и их нехватка — это отказ разбора.
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
    const char *s = "{\"FID\":4115,\"FLAGS\":0,\"SID\":7,\"ARG\":{\"ADR\":1}}";
    CHECK(parse(s));
    uint32_t fid = 0;
    int32_t flags = -1, sid = -1;
    CHECK(gw_json_id(&j, "FID", &fid));
    LONGS_EQUAL(4115, fid);
    CHECK(gw_json_int(&j, "FLAGS", &flags));
    LONGS_EQUAL(0, flags);
    CHECK(gw_json_int(&j, "SID", &sid));
    LONGS_EQUAL(7, sid);
}

TEST(GwJsonParse, FidAsHexString) {
    const char *s = "{\"FID\":\"0x1012\",\"FLAGS\":4}";
    CHECK(parse(s));
    uint32_t fid = 0;
    CHECK(gw_json_id(&j, "FID", &fid));
    LONGS_EQUAL(0x1012, fid);
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
