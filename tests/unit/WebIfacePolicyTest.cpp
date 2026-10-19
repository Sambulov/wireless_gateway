/* Политика «какой сервис веб-сервера каким интерфейсам оказан».
 *
 * Граница «доступно только из точки доступа» задана таблицей в
 * web_iface_policy.c. Здесь она проверяется как данные: тест держит её на месте
 * и описывает словами то, что на устройстве сверяется запросами с двух
 * интерфейсов. Проверяется только решение политики — сам httpd и определение
 * интерфейса соединения (gw_iface_of_socket) в хостовой сборке отсутствуют.
 *
 * Исключений «только точка доступа» в таблице больше нет: прошивка, которая
 * была последним таким правилом (POST /update), стала службой ядра API
 * (sys.fw.*, apih_fw.c), и её граница — регистрация на AP-ядре, а не маска
 * пути. Файлового менеджера (sys.file.*) и настроек (wifi.ap/sta, sys.net) в
 * таблице не было и нет: у них та же граница в ядре API.
 *
 * Список проверок:
 * [x] Границы «только AP» в HTTP-таблице нет: /update — обычный путь
 * [x] Файловых HTTP-методов в таблице нет: они попадают под общее правило
 * [x] Страницы, карты, терминал, файлы и /api → «любой интерфейс»
 * [x] «Любой интерфейс» включает и неизвестный (GW_IFACE_NONE)
 * [x] query-строка на решение не влияет
 * [x] Шаблон правила и шаблон httpd совпадают: "/uploads/..." — не отдельный сервис
 * [x] Пустой URI — сервиса нет ни у кого (отказ)
 * [x] Семантика маски: AP-маска закрыта всем, кроме точки доступа
 */

#include "CppUTest/TestHarness.h"
#include "net_iface.h"

#include "transport.h"
#include "web_iface_policy.h"

#define AP_MASK   GW_IFACE_MASK(GW_IFACE_AP)
#define STA_MASK  GW_IFACE_MASK(GW_IFACE_STA)
#define ETH_MASK  GW_IFACE_MASK(GW_IFACE_ETH)

TEST_GROUP(WebIfacePolicy) {
};

/* Прошивки по HTTP больше нет: правила «/update — только AP» в таблице не
 * осталось, и путь попадает под общее. Само 404 на устройстве получается не
 * политикой, а отсутствием маршрута: обработчика с таким URI не регистрирует ни
 * один объект веб-сервера (см. app.c, register_pages). */
TEST(WebIfacePolicy, FirmwarePathIsNotAnHttpServiceAnyMore) {
    const char *uris[] = {
        "/update",                   /* прежний POST прошивки */
        "/update?clients=1",         /* как приходит запрос с query-строкой */
    };

    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        UNSIGNED_LONGS_EQUAL(GW_IFACE_MASK_ANY, web_iface_service_mask(uris[i]));
        CHECK_TRUE(web_iface_service_allowed(GW_IFACE_STA, uris[i]));
        CHECK_TRUE(web_iface_service_allowed(GW_IFACE_ETH, uris[i]));
    }
}

/* Файловый менеджер по HTTP отсутствует: этих путей нет ни в таблице, ни среди
 * зарегистрированных обработчиков (POST на них httpd отвечает 404), поэтому
 * политика о них ничего особенного не говорит — они попадают под общее правило,
 * как и любой незнакомый путь. Проверка держит на месте сам факт «правила
 * файлового менеджера в таблице нет»: если кто-то добавит сюда правило
 * загрузки с маской AP, тест упадёт. */
TEST(WebIfacePolicy, RemovedFileManagerPathsAreNotSpecialAnyMore) {
    const char *uris[] = {
        "/upload/*", "/upload/index.html", "/upload/regmaps/a.jsonc",
        "/delete/*", "/delete/index.html", "/rename/*", "/rename/terminal/script.js",
    };

    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        UNSIGNED_LONGS_EQUAL(GW_IFACE_MASK_ANY, web_iface_service_mask(uris[i]));
        CHECK_TRUE(web_iface_service_allowed(GW_IFACE_STA, uris[i]));
    }
}

/* Страницы, карты регистров, терминал, файлы файловой системы и WebSocket-API
 * доступны с любого интерфейса. Запись настроек ограничивает не веб-сервер, а
 * политика ядра API. */
TEST(WebIfacePolicy, PagesMapsTerminalAndApiAreServedEverywhere) {
    const char *uris[] = {
        "/", "/index.html", "/config", "/config.html", "/modbus", "/modbus.html",
        "/innovert.html", "/innovert_isd.html",
        "/terminal/", "/terminal/index.html", "/terminal/script.js",
        "/regmaps/suto_s111.jsonc", "/regmaps/innovert_isd222.jsonc",
        "/api", "/favicon.ico", "/anything/not/registered", "/update",
    };

    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        UNSIGNED_LONGS_EQUAL(GW_IFACE_MASK_ANY, web_iface_service_mask(uris[i]));
        CHECK_TRUE(web_iface_service_allowed(GW_IFACE_AP,   uris[i]));
        CHECK_TRUE(web_iface_service_allowed(GW_IFACE_STA,  uris[i]));
        CHECK_TRUE(web_iface_service_allowed(GW_IFACE_ETH,  uris[i]));
        CHECK_TRUE(web_iface_service_allowed(GW_IFACE_NONE, uris[i]));
    }
}

/* Семантика маски: неизвестному интерфейсу достаётся только сервис «любой
 * интерфейс». */
TEST(WebIfacePolicy, MaskSemantics) {
    CHECK_TRUE (web_iface_mask_allows(GW_IFACE_MASK_ANY, GW_IFACE_NONE));
    CHECK_FALSE(web_iface_mask_allows(AP_MASK,           GW_IFACE_NONE));
    CHECK_FALSE(web_iface_mask_allows(GW_IFACE_MASK_NONE, GW_IFACE_AP));
    CHECK_TRUE (web_iface_mask_allows(AP_MASK,           GW_IFACE_AP));
    CHECK_FALSE(web_iface_mask_allows(AP_MASK,           GW_IFACE_STA));
    CHECK_TRUE (web_iface_mask_allows(AP_MASK | STA_MASK, GW_IFACE_STA));
    /* Чужое значение идентификатора интерфейса — это «неизвестен», а не
     * попадание в чужой бит маски. */
    CHECK_FALSE(web_iface_mask_allows(AP_MASK,           GW_IFACE_LAST + 1));
    CHECK_TRUE (web_iface_mask_allows(GW_IFACE_MASK_ANY, GW_IFACE_LAST + 1));
}

/* Запрос с query-строкой доходит до обработчика как путь без неё: решение
 * политики от query-строки не зависит (её отрезает path_length). */
TEST(WebIfacePolicy, QueryStringDoesNotChangeTheDecision) {
    UNSIGNED_LONGS_EQUAL(GW_IFACE_MASK_ANY, web_iface_service_mask("/update?x=1&y=2"));
    UNSIGNED_LONGS_EQUAL(GW_IFACE_MASK_ANY, web_iface_service_mask("/config?tab=net"));
    CHECK_TRUE(web_iface_service_allowed(GW_IFACE_STA, "/regmaps/a.jsonc?v=2"));
}

/* Правило с '*' покрывает ровно те пути, что и шаблон httpd: правило — это шаблон
 * со звёздочкой в конце, поэтому "/uploads/…" под правило загрузки файла не
 * попало бы (и до обработчика не дошло бы). Политика не должна быть строже httpd
 * — иначе открытый сервис мог бы остаться без ответа. */
TEST(WebIfacePolicy, WildcardRuleBoundaryMatchesHttpdTemplate) {
    UNSIGNED_LONGS_EQUAL(GW_IFACE_MASK_ANY, web_iface_service_mask("/uploads/index.html"));
    UNSIGNED_LONGS_EQUAL(GW_IFACE_MASK_ANY, web_iface_service_mask("/upload"));
    UNSIGNED_LONGS_EQUAL(GW_IFACE_MASK_ANY, web_iface_service_mask("/update/x"));
    CHECK_TRUE(web_iface_service_allowed(GW_IFACE_STA, "/uploads/index.html"));
}

TEST(WebIfacePolicy, NoUriMeansNoService) {
    UNSIGNED_LONGS_EQUAL(GW_IFACE_MASK_NONE, web_iface_service_mask(NULL));
    UNSIGNED_LONGS_EQUAL(GW_IFACE_MASK_NONE, web_iface_service_mask(""));
    CHECK_FALSE(web_iface_service_allowed(GW_IFACE_AP, NULL));
}
