#include <string.h>
#include "net_iface.h"

#include "web_iface_policy.h"

/* ── Таблица «путь → кому сервис оказан» ─────────────────────────────────────
 *
 * Ключ таблицы — URI из httpd_uri_t.uri, то есть тот же шаблон, которым httpd
 * выбирает обработчик: путь без '*' сравнивается с путём запроса точно, путь с
 * '*' в конце — по началу (шаблон "/terminal/" со звёздочкой — это uri,
 * начинающийся с "/terminal/"). Так же читает шаблоны и httpd
 * (httpd_uri_match_wildcard), поэтому множество путей, доходящих до обработчика,
 * и множество путей, покрытых правилом, совпадают: политика привязана к сервису,
 * а не к отдельному запросу.
 *
 * Порядок важен: берётся ПЕРВОЕ совпадение, поэтому частные пути стоят раньше
 * общего.
 *
 * ── Где проходит граница «только AP» ────────────────────────────────────────
 * В HTTP-таблице её больше нет: прошивка, единственное исключение прежних лет
 * (/update, POST образа), стала службой ядра API (sys.fw.*, apih_fw.c), и
 * «только точка доступа» у неё выражено регистрацией на AP-ядре. Всё, что
 * отдаётся по HTTP, — страницы, карты регистров, терминал, чтение файлов
 * файловой системы, WebSocket-API (/api) — доступно с любого интерфейса
 * (единственное правило таблицы ниже). Вшитая страница конфигурации (/config,
 * /config.html) оказана только точке доступа не таблицей, а регистрацией
 * маршрута в объекте интерфейса (см. app.c).
 *
 * Файлового менеджера в этой таблице нет: загрузка, удаление и переименование
 * файлов идут вызовами по WebSocket (sys.file.write/delete/rename, apih_sys.c),
 * и там граница «только точка доступа» задана регистрацией сервиса на
 * конкретном ядре, как и у листинга sys.files.
 *
 * Запись настроек и так ограничена политикой ядра API: сервисы wifi.ap /
 * wifi.sta / sys.net зарегистрированы с контекстом «только чтение» для не-AP
 * (apih_sys.c).
 *
 * Интерфейс неизвестен (GW_IFACE_NONE) — не точка доступа: сервис оказывается
 * ему, только если его маска «любой интерфейс» (см. web_iface_mask_allows). */
static const struct {
    const char *path;   /* путь или шаблон пути из httpd_uri_t.uri */
    uint32_t    mask;   /* GW_IFACE_MASK_* — кому этот сервис оказан */
} web_services[] = {
    /* Всё прочее: страницы (/index.html, /modbus, /modbus.html, /innovert*.html),
     * терминал (/terminal/…), файлы карт регистров (/regmaps/…) и всё остальное
     * из файловой системы — их отдаёт общий обработчик, последнее правило
     * таблицы. Вшитая страница конфигурации (/config, /config.html) оказана
     * только точке доступа, но задаёт это не таблица, а регистрация маршрута: в
     * объекте станции маршрута нет, и запрос доходит сюда как обычный путь
     * файловой системы. Туда же попадает WebSocket-API (/api): на уровне HTTP он
     * открыт всем осознанно — свои вызовы он различает сам, регистрацией
     * сервисов на конкретных ядрах (apih_*.c), а не здесь. Сюда же попадает
     * /update: маршрута с таким путём нет ни у одного интерфейса, поэтому
     * запрос по нему заканчивается 404 в общем обработчике файлов. */
    { "/*",        GW_IFACE_MASK_ANY },
};

/* Путь запроса без query-строки и якоря: httpd сравнивает шаблоны именно с
 * путём (req->uri содержит и "?query", см. httpd_uri.c). */
static size_t path_length(const char *uri) {
    size_t len = 0;
    if (uri == NULL) return 0;
    while (uri[len] != '\0' && uri[len] != '?' && uri[len] != '#') len++;
    return len;
}

/* Совпадает ли путь правила с путём запроса. Семантика повторяет
 * httpd_uri_match_wildcard: '*' в конце шаблона — «дальше что угодно», но сама
 * часть до '*' должна совпасть целиком (шаблон загрузки не покрывает
 * "/uploads"). */
static uint8_t path_matches(const char *rule, const char *uri, size_t uri_len) {
    const size_t rule_len = strlen(rule);
    const uint8_t wildcard = (rule_len > 0) && (rule[rule_len - 1] == '*');
    const size_t fixed = wildcard ? (rule_len - 1) : rule_len;

    if (uri_len < fixed) return 0;
    if (!wildcard && (uri_len != fixed)) return 0;
    return memcmp(rule, uri, fixed) == 0;
}

uint8_t web_iface_path_matches(const char *tmpl, const char *uri) {
    if ((tmpl == NULL) || (uri == NULL)) return 0;
    return path_matches(tmpl, uri, path_length(uri));
}

uint32_t web_iface_service_mask(const char *uri) {
    const size_t uri_len = path_length(uri);
    if (uri_len == 0) return GW_IFACE_MASK_NONE;   /* ни пути, ни сервиса */

    for (size_t i = 0; i < sizeof(web_services) / sizeof(web_services[0]); i++) {
        if (path_matches(web_services[i].path, uri, uri_len))
            return web_services[i].mask;
    }
    /* Таблица заканчивается общим правилом, поэтому сюда попадает только URI,
     * не начинающийся с '/'. Такого пути у httpd быть не может, а открывать
     * незнакомое молча нельзя: маска «никому» — это отказ. */
    return GW_IFACE_MASK_NONE;
}

uint8_t web_iface_mask_allows(uint32_t iface_mask, uint32_t iface) {
    /* Идентификатор интерфейса приходит от транспорта: чужое значение — это
     * «интерфейс неизвестен», а не повод считать сдвиг маски. */
    if (iface > GW_IFACE_LAST) iface = GW_IFACE_NONE;

    if (iface == GW_IFACE_NONE) return iface_mask == GW_IFACE_MASK_ANY;
    return (iface_mask & GW_IFACE_MASK(iface)) != 0;
}

uint8_t web_iface_service_allowed(uint32_t iface, const char *uri) {
    return web_iface_mask_allows(web_iface_service_mask(uri), iface);
}
