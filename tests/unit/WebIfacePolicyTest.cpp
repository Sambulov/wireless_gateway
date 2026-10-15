/* Политика «какой сервис веб-сервера каким интерфейсам оказан».
 *
 * Граница «доступно только из точки доступа» задана таблицей в
 * web_iface_policy.c. Здесь она проверяется как данные: тест держит её на месте
 * и описывает словами то, что на устройстве сверяется запросами с двух
 * интерфейсов. Проверяется только решение политики — сам httpd и определение
 * интерфейса соединения (gw_iface_of_socket) в хостовой сборке отсутствуют.
 *
 * Файловый менеджер (загрузка, удаление, переименование) в этой таблице больше не
 * описан: он переехал на WebSocket-сервисы sys.file.write/delete/rename, и
 * граница «только точка доступа» у него — маска регистрации в ядре API
 * (apih_sys.c), как у листинга sys.files. HTTP-методов /upload/…, /delete/…,
 * /rename/… нет вовсе, поэтому «только AP» здесь остаётся один /update.
 *
 * Список проверок:
 * [x] /update → маска «только AP»
 * [x] AP-only сервис: AP — да, STA/ETH/неизвестный — нет
 * [x] Убранных файловых HTTP-методов в таблице нет: они попадают под общее правило
 * [x] Страницы, карты, терминал, файлы и /ws → «любой интерфейс»
 * [x] «Любой интерфейс» включает и неизвестный (GW_IFACE_NONE)
 * [x] query-строка на решение не влияет
 * [x] Шаблон правила и шаблон httpd совпадают: "/uploads/..." — не отдельный сервис
 * [x] Пустой URI — сервиса нет ни у кого (отказ)
 */

#include "CppUTest/TestHarness.h"

#include "ws_transport.h"
#include "web_iface_policy.h"

#define AP_MASK   GW_IFACE_MASK(GW_IFACE_AP)
#define STA_MASK  GW_IFACE_MASK(GW_IFACE_STA)
#define ETH_MASK  GW_IFACE_MASK(GW_IFACE_ETH)

TEST_GROUP(WebIfacePolicy) {
};

/* Прошивка — единственное, что сегодня только для точки доступа. Шаблон и
 * реальные пути: правило должно покрывать и то, и другое. */
TEST(WebIfacePolicy, ApOnlyMaskForFirmware) {
    const char *uris[] = {
        "/update",                   /* как зарегистрировано */
        "/update?clients=1",         /* как приходит запрос с query-строкой */
    };

    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++)
        UNSIGNED_LONGS_EQUAL(AP_MASK, web_iface_service_mask(uris[i]));
}

TEST(WebIfacePolicy, ApOnlyServiceIsDeniedToEveryOtherIface) {
    const char *uris[] = { "/update" };

    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        CHECK_TRUE(web_iface_service_allowed(GW_IFACE_AP,   uris[i]));
        CHECK_FALSE(web_iface_service_allowed(GW_IFACE_STA, uris[i]));
        CHECK_FALSE(web_iface_service_allowed(GW_IFACE_ETH, uris[i]));
        /* Интерфейс неизвестен — считаем как «не AP» (иначе сервис утёк бы
         * всякому, чей интерфейс транспорт не смог определить). */
        CHECK_FALSE(web_iface_service_allowed(GW_IFACE_NONE, uris[i]));
    }
}

/* Файловый менеджер по HTTP убран целиком: этих путей нет ни в таблице, ни среди
 * зарегистрированных обработчиков (POST на них httpd отвечает 404), поэтому
 * политика о них ничего особенного не говорит — они попадают под общее правило,
 * как и любой незнакомый путь. Проверка держит на месте сам факт «правила
 * файлового менеджера в таблице нет»: если кто-то вернёт сюда строку
 * правило загрузки с маской AP, тест упадёт. */
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

/* Страницы, карты регистров, терминал, файлы файловой системы и WebSocket-API:
 * снаружи они и раньше были доступны с любого интерфейса — так и остаётся.
 * Запись настроек ограничивает не веб-сервер, а политика ядра API. */
TEST(WebIfacePolicy, PagesMapsTerminalAndApiAreServedEverywhere) {
    const char *uris[] = {
        "/", "/index.html", "/config", "/config.html", "/modbus", "/modbus.html",
        "/innovert.html", "/innovert_isd.html",
        "/terminal/", "/terminal/index.html", "/terminal/script.js",
        "/regmaps/suto_s111.jsonc", "/regmaps/innovert_isd222.jsonc",
        "/ws", "/favicon.ico", "/anything/not/registered",
    };

    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        UNSIGNED_LONGS_EQUAL(GW_IFACE_MASK_ANY, web_iface_service_mask(uris[i]));
        CHECK_TRUE(web_iface_service_allowed(GW_IFACE_AP,   uris[i]));
        CHECK_TRUE(web_iface_service_allowed(GW_IFACE_STA,  uris[i]));
        CHECK_TRUE(web_iface_service_allowed(GW_IFACE_ETH,  uris[i]));
        CHECK_TRUE(web_iface_service_allowed(GW_IFACE_NONE, uris[i]));
    }
}

/* Семантика маски — та же, что у ядра API (bIfaceMaskServes в ws_server.c):
 * неизвестному интерфейсу достаётся только сервис «любой интерфейс». */
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

/* /update?clients=1 доходит до обработчика как путь "/update": проверка
 * интерфейса не должна зависеть от query-строки. */
TEST(WebIfacePolicy, QueryStringDoesNotChangeTheDecision) {
    UNSIGNED_LONGS_EQUAL(AP_MASK, web_iface_service_mask("/update?x=1&y=2"));
    CHECK_FALSE(web_iface_service_allowed(GW_IFACE_STA, "/update?clients=1"));
    UNSIGNED_LONGS_EQUAL(GW_IFACE_MASK_ANY, web_iface_service_mask("/config?tab=net"));
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
