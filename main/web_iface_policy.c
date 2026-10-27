#include <string.h>
#include "net_iface.h"

#include "web_iface_policy.h"

/* ── Таблица «путь → кому сервис оказан» ─────────────────────────────────────
 *
 * Ключ таблицы — URI из httpd_uri_t.uri, то есть тот же шаблон, которым httpd
 * выбирает обработчик (сравнение как у httpd_uri_match_wildcard). Поэтому политика
 * привязана к сервису, а не к отдельному запросу. Порядок важен: берётся ПЕРВОЕ
 * совпадение, поэтому частные пути стоят раньше общего.
 *
 * ── Где проходит граница «только AP» ────────────────────────────────────────
 * В HTTP-таблице её нет: всё, что отдаётся по HTTP (страницы, карты регистров,
 * терминал, чтение файлов, WebSocket-API /api), доступно с любого интерфейса.
 * Вшитая страница конфигурации (/config) оказана только точке доступа не таблицей,
 * а регистрацией маршрута в объекте интерфейса (см. app.c).
 *
 * Файлового менеджера здесь нет: файлы изменяются вызовами по WebSocket
 * (sys.file.*, apih_sys.c), и там граница задана регистрацией сервиса на
 * конкретном ядре.
 *
 * Интерфейс неизвестен (GW_IFACE_NONE) — не точка доступа: сервис оказывается
 * ему, только если его маска «любой интерфейс» (см. web_iface_mask_allows). */
static const struct {
    const char *path;   /* путь или шаблон пути из httpd_uri_t.uri */
    uint32_t    mask;   /* GW_IFACE_MASK_* — кому этот сервис оказан */
} web_services[] = {
    /* Всё прочее — общий обработчик файлов: страницы, терминал, файлы карт
     * регистров. WebSocket-API (/api) открыт на уровне HTTP всем осознанно: свои
     * вызовы он различает сам, регистрацией сервисов на конкретных ядрах. */
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
    /* Таблица заканчивается общим правилом, поэтому сюда попадает только URI, не
     * начинающийся с '/'. Открывать незнакомое молча нельзя: маска «никому» —
     * это отказ. */
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
