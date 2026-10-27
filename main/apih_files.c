/* Файловый менеджер: sys.files и sys.file.write/delete/rename/mkdir/rmdir.
 *
 * Политики на клиента у этих методов нет (контекста регистрации им не дают):
 * регистрируются одинаково на обоих ядрах — доступны с любого интерфейса, как и
 * отдача файлов по HTTP. Общая обвязка — apih_sys_common.c, сборка — apih_sys.c.
 */
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>   /* unlink, rmdir: правка файлов идёт через VFS LittleFS */

#include "esp_littlefs.h"

#include "app.h"
#include "fs_path.h"

#include "apih_sys_internal.h"

#define GW_LOG_MODULE GW_LOG_API
#include "gw_log.h"

/* Путь с базовым префиксом монтирования ("/lfs/maps/x.jsonc") нужен и листингу, и
 * записи. Разбор клиентского пути общий, в fs_path.c: своей, более мягкой проверки
 * здесь быть не должно. */
#define FS_PATH_MAX 256
#define FS_FULL_MAX (FS_PATH_MAX + 64)

/* Путь целиком в смонтированной ФС: base_path + нормализованный путь */
static uint8_t fs_full_path(const char *path, char *out, size_t out_size) {
    int written = snprintf(out, out_size, "%s%s", conf.base_path, path);
    return (written > 0) && (written < (int)out_size);
}

/* Проверить путь из ARG и собрать отказ в ответ, если он недопустим. field —
 * имя поля запроса (PATH, FROM, TO): по нему видно, что именно не понравилось. */
static uint8_t fs_path_check(const char *requested, const char *field,
                             char *path, size_t path_size) {
    if (!requested || !fs_path_sanitize(requested, path, path_size) ||
        strcmp(path, "/") == 0) {
        resp_error(SYS_BAD_ARG, field,
                   "An absolute path inside the filesystem is required, without '.' or '..', e.g. /maps/x.jsonc");
        return 0;
    }
    return 1;
}

/* Разбирает ARG: {"PATH": "/regmaps"} — каталог для листинга (по умолчанию корень) */
static void fs_arg_path(const uint8_t *data, uint32_t len, char *out, size_t out_size) {
    out[0] = '\0';
    GW_JSON_TOKENS(toks, SYS_ARG_MAX_TOKENS);
    gw_json_t json;
    if ((data && len) && gw_json_parse(&json, (const char *)data, len, toks, SYS_ARG_MAX_TOKENS))
        gw_json_str(&json, "PATH", out, out_size);
    if (out[0] == '\0')
        strlcpy(out, "/", out_size);
}

static void handle_files(const uint8_t *data, uint32_t len) {
    size_t fs_total = 0, fs_used = 0;
    esp_littlefs_info(conf.partition_label, &fs_total, &fs_used);

    char requested[FS_PATH_MAX], path[FS_PATH_MAX];
    fs_arg_path(data, len, requested, sizeof(requested));

    resp_ok();
    gw_jsonw_uint(&sys_w, "TOTAL", (uint64_t)fs_total);
    gw_jsonw_uint(&sys_w, "USED", (uint64_t)fs_used);

    if (!fs_path_sanitize(requested, path, sizeof(path))) {
        gw_jsonw_str(&sys_w, "ERR", "PATH");
        gw_jsonw_str(&sys_w, "STA", SYS_BAD_ARG);
        return;
    }
    gw_jsonw_str(&sys_w, "PATH", path);

    /* каталог выше по дереву, чтобы страница могла нарисовать «наверх» */
    char parent[FS_PATH_MAX];
    strlcpy(parent, path, sizeof(parent));
    if (strcmp(parent, "/") != 0) {
        char *slash = strrchr(parent, '/');
        if (slash == parent)
            strlcpy(parent, "/", sizeof(parent));
        else if (slash)
            *slash = '\0';
        gw_jsonw_str(&sys_w, "PARENT", parent);
    }

    char full[FS_FULL_MAX];
    if (!fs_full_path(path, full, sizeof(full))) {
        gw_jsonw_str(&sys_w, "ERR", "PATH");
        return;
    }

    gw_jsonw_arr(&sys_w, "FILES");

    DIR *dir = opendir(full);
    if (!dir) {
        ESP_LOGW(TAG, "Cannot open %s", full);
        gw_jsonw_str(&sys_w, "ERR", "FS");
        gw_jsonw_str(&sys_w, "STA", SYS_BAD_ARG);
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.')
            continue;
        char item_path[600]; /* base path + каталог + '/' + NAME_MAX */
        int n = snprintf(item_path, sizeof(item_path), "%s/%s", full, entry->d_name);
        if (n < 0 || n >= (int)sizeof(item_path))
            continue;
        struct stat st;
        if (stat(item_path, &st) != 0)
            continue;

        /* буфер не резиновый: как только он кончился, прекращаем перечисление */
        if (!gw_jsonw_ok(&sys_w))
            break;

        gw_jsonw_obj(&sys_w, NULL);
        gw_jsonw_str(&sys_w, "NAME", entry->d_name);
        gw_jsonw_int(&sys_w, "SIZE", (int64_t)st.st_size);
        gw_jsonw_bool(&sys_w, "DIR", S_ISDIR(st.st_mode) ? 1 : 0);
        gw_jsonw_end(&sys_w);
    }
    closedir(dir);
}

/* ── sys.file.write / sys.file.delete / sys.file.rename ────────────────────── *
 *
 * Изменение файлов идёт вызовами по WebSocket; отдача файлов по HTTP GET не тронута
 * — именно так браузер получает страницы и карты регистров.
 *
 * Файл в одно сообщение не влезает (транспорт держит 16 КБ, а двоичные данные идут
 * ещё и в base64), поэтому ARG
 * {"PATH":"/maps/x.jsonc","OFFSET":<сколько байт уже принято>,"DATA":"<base64>",
 * "LAST":0|1}: OFFSET 0 начинает запись ЗАНОВО, LAST 1 завершает приём, а OFFSET
 * каждой следующей порции обязан совпасть с числом принятых байт — иначе порция
 * потерялась бы молча.
 *
 * Пишем не в цель, а во ВРЕМЕННЫЙ файл рядом (".<имя>.part"; листинг имена с точки не
 * показывает) и только на LAST переименовываем в цель: переименование заменяет файл
 * целиком, поэтому оборванная передача не оставляет полуфайл вместо рабочего.
 *
 * Состояние приёма одно на прибор: обработчики ядра гоняет одна задача, по одному
 * вызову за такт, и незавершённая запись одна. Порция пишется синхронно в этой же
 * задаче, поэтому задержка для остальных вызовов — на время записи в flash.
 */

/* раскодируем порцию блоками по 4 символа base64: память под данные не зависит от
 * размера порции */
#define FS_B64_BLOCK   512
/* предел размера файла: файловая система одна на страницы, карты и настройки,
 * забить её целиком нельзя */
#define FS_FILE_MAX     (500 * 1024)
#define FS_FILE_MAX_STR "500 KB"

typedef struct {
    uint8_t  active;
    char     path[FS_PATH_MAX];
    char     tmp[FS_PATH_MAX];    /* временный файл рядом с целью */
    FILE    *fp;
    uint32_t written;             /* сколько байт уже принято */
} fs_upload_t;

static fs_upload_t fs_up;

/* Закончить приём и забыть состояние. unlink_tmp — убрать временный файл:
 * оборванная передача не должна оставлять его до следующей попытки. */
static void fs_upload_close(uint8_t unlink_tmp) {
    if (fs_up.fp) {
        fclose(fs_up.fp);
        fs_up.fp = NULL;
    }
    if (unlink_tmp && fs_up.active && fs_up.tmp[0]) {
        char full[FS_FULL_MAX];
        if (fs_full_path(fs_up.tmp, full, sizeof(full)))
            unlink(full);
    }
    fs_up.active  = 0;
    fs_up.written = 0;
    fs_up.path[0] = '\0';
    fs_up.tmp[0]  = '\0';
}

/* Начать приём в path (уже проверенный): временный файл создаётся заново */
static uint8_t fs_upload_begin(const char *path) {
    char tmp[FS_PATH_MAX];
    char full[FS_FULL_MAX];

    if (!fs_path_temp(path, tmp, sizeof(tmp)) || !fs_full_path(tmp, full, sizeof(full)))
        return 0;

    FILE *fp = fopen(full, "wb");
    if (!fp) {
        ESP_LOGE(TAG, "Не удалось создать %s", full);
        return 0;
    }

    strlcpy(fs_up.path, path, sizeof(fs_up.path));
    strlcpy(fs_up.tmp, tmp, sizeof(fs_up.tmp));
    fs_up.fp      = fp;
    fs_up.written = 0;
    fs_up.active  = 1;
    return 1;
}

/* Дописать порцию base64 в открытый файл. 0 — данные не base64 или запись не
 * удалась (вызывающий убирает полуфабрикат). */
static uint8_t fs_upload_append(const char *b64, size_t b64_len) {
    uint8_t buf[FS_B64_BLOCK];
    size_t done = 0;

    while (done < b64_len) {
        size_t n = b64_len - done;
        if (n > sizeof(buf))
            n = sizeof(buf);
        n &= ~(size_t)3;                     /* блок base64 — 4 символа */
        if (n == 0)
            return 0;

        const uint8_t *src = (const uint8_t *)b64 + done;
        int32_t expect  = base64_decode_buffer_required(src, (uint32_t)n);
        int32_t decoded = base64_decode(buf, sizeof(buf), src, (uint32_t)n);
        /* decoded != expect — мусор в середине строки: декодер останавливается на
         * первом '=' и молча отдал бы обрезанный файл. */
        if (expect < 0 || decoded != expect)
            return 0;
        if (decoded > 0 && fwrite(buf, 1, (size_t)decoded, fs_up.fp) != (size_t)decoded)
            return 0;

        fs_up.written += (uint32_t)decoded;
        done += n;
    }
    return 1;
}

/* Запись файла порциями: {"PATH":"/a.txt","OFFSET":0,"DATA":"<base64>","LAST":0}
 * Ответ: {"STA":"0x00000000","WRITTEN":<сколько байт принято всего>} */
static void handle_file_write(const uint8_t *data, uint32_t len) {
    char requested[FS_PATH_MAX];
    char path[FS_PATH_MAX];
    GW_JSON_TOKENS(toks, SYS_ARG_MAX_TOKENS);
    gw_json_t json;
    int32_t offset = 0, last = 0;
    const char *b64 = NULL;
    size_t b64_len = 0;

    if (!((data && len) && gw_json_parse(&json, (const char *)data, len, toks, SYS_ARG_MAX_TOKENS))) {
        resp_error(SYS_BAD_ARG, "ARG",
                   "ARG must be an object: {\"PATH\":\"/a.txt\",\"OFFSET\":0,\"DATA\":\"<base64>\",\"LAST\":1}");
        return;
    }
    if (!gw_json_str(&json, "PATH", requested, sizeof(requested))) {
        resp_error(SYS_BAD_ARG, "PATH", "PATH is required, e.g. /maps/x.jsonc");
        return;
    }
    if (!fs_path_check(requested, "PATH", path, sizeof(path)))
        return;
    if (!gw_json_int(&json, "OFFSET", &offset) || offset < 0 || (uint32_t)offset > FS_FILE_MAX) {
        resp_error(SYS_BAD_ARG, "OFFSET",
                   "OFFSET is the number of bytes already written; 0 starts a new file");
        return;
    }
    /* LAST в запросе может отсутствовать — это обычная порция */
    gw_json_int(&json, "LAST", &last);
    if (!gw_json_raw(&json, "DATA", &b64, &b64_len))
        b64_len = 0;                         /* DATA может быть пустой строкой */

    if ((b64_len & 3) != 0) {
        resp_error(SYS_BAD_ARG, "DATA", "DATA must be base64 without spaces or line breaks");
        return;
    }
    int32_t decoded_len = b64_len
                          ? base64_decode_buffer_required((const uint8_t *)b64, (uint32_t)b64_len)
                          : 0;
    if (decoded_len < 0) {
        resp_error(SYS_BAD_ARG, "DATA", "DATA must be base64 without spaces or line breaks");
        return;
    }

    if (offset == 0) {
        char dst_full[FS_FULL_MAX];
        struct stat st;
        /* Недописанная передача (любого пути) — прочь: порция с OFFSET 0
         * начинает файл заново. */
        fs_upload_close(1);
        /* Каталог файлом не заменится: отказываем до записи, а не на LAST */
        if (fs_full_path(path, dst_full, sizeof(dst_full)) &&
            stat(dst_full, &st) == 0 && S_ISDIR(st.st_mode)) {
            resp_error(SYS_BAD_ARG, "PATH", "PATH points to a directory");
            return;
        }
        if (!fs_upload_begin(path)) {
            resp_error(SYS_INTERNAL, "FS", "Failed to create the file");
            return;
        }
    } else if (!fs_up.active || strcmp(fs_up.path, path) != 0) {
        resp_error(SYS_BAD_ARG, "OFFSET", "No upload of this PATH in progress: start with OFFSET 0");
        return;
    } else if ((uint32_t)offset != fs_up.written) {
        resp_error(SYS_BAD_ARG, "OFFSET",
                   "OFFSET must equal the number of bytes the device has already accepted");
        return;
    }

    if (fs_up.written + (uint32_t)decoded_len > FS_FILE_MAX) {
        fs_upload_close(1);
        resp_error(SYS_BAD_ARG, "SIZE", "File must be no larger than " FS_FILE_MAX_STR);
        return;
    }

    if (b64_len && !fs_upload_append(b64, b64_len)) {
        fs_upload_close(1);                  /* цель не тронута: пишем во временный */
        resp_error(SYS_INTERNAL, "FS", "Failed to write the file");
        return;
    }

    if (!last) {
        resp_ok();
        gw_jsonw_uint(&sys_w, "WRITTEN", (uint64_t)fs_up.written);
        return;
    }

    /* LAST: временный файл встаёт на место цели. Переименование заменяет
     * существующий файл целиком — до этого места он не изменён ни на байт. */
    char tmp_full[FS_FULL_MAX], dst_full[FS_FULL_MAX];
    if (!fs_full_path(fs_up.tmp, tmp_full, sizeof(tmp_full)) ||
        !fs_full_path(fs_up.path, dst_full, sizeof(dst_full))) {
        fs_upload_close(1);
        resp_error(SYS_INTERNAL, "FS", "Failed to finish writing the file");
        return;
    }
    uint32_t written = fs_up.written;
    fclose(fs_up.fp);
    fs_up.fp = NULL;
    if (rename(tmp_full, dst_full) != 0) {
        ESP_LOGE(TAG, "Не удалось переименовать %s в %s", tmp_full, dst_full);
        fs_upload_close(1);
        resp_error(SYS_INTERNAL, "FS", "Failed to finish writing the file");
        return;
    }
    fs_upload_close(0);
    LOGI(TAG, "Файл %s записан (%lu байт)", dst_full, (unsigned long)written);
    resp_ok();
    gw_jsonw_uint(&sys_w, "WRITTEN", (uint64_t)written);
}

/* Удаление файла: ARG {"PATH":"/maps/x.jsonc"}
 * Ответ: {"STA":"0x00000000","PATH":"/maps/x.jsonc"} */
static void handle_file_delete(const uint8_t *data, uint32_t len) {
    char requested[FS_PATH_MAX];
    char path[FS_PATH_MAX];
    char full[FS_FULL_MAX];
    struct stat st;
    GW_JSON_TOKENS(toks, SYS_ARG_MAX_TOKENS);
    gw_json_t json;

    if (!((data && len) && gw_json_parse(&json, (const char *)data, len, toks, SYS_ARG_MAX_TOKENS) &&
          gw_json_str(&json, "PATH", requested, sizeof(requested)))) {
        resp_error(SYS_BAD_ARG, "PATH", "PATH is required, e.g. {\"PATH\":\"/maps/x.jsonc\"}");
        return;
    }
    if (!fs_path_check(requested, "PATH", path, sizeof(path)))
        return;
    if (!fs_full_path(path, full, sizeof(full))) {
        resp_error(SYS_BAD_ARG, "PATH", "PATH is too long");
        return;
    }
    if (stat(full, &st) != 0) {
        resp_error(SYS_BAD_ARG, "PATH", "File does not exist");
        return;
    }
    if (S_ISDIR(st.st_mode)) {
        resp_error(SYS_BAD_ARG, "PATH", "PATH points to a directory");
        return;
    }
    /* Незавершённая передача этого же файла уходит вместе с ним: иначе её
     * полуфабрикат остался бы и «дошёл» до файла следующей загрузкой. */
    if (fs_up.active && strcmp(fs_up.path, path) == 0)
        fs_upload_close(1);

    if (unlink(full) != 0) {
        ESP_LOGE(TAG, "Не удалось удалить %s", full);
        resp_error(SYS_INTERNAL, "FS", "Failed to delete the file");
        return;
    }
    LOGI(TAG, "Файл %s удалён", full);
    resp_ok();
    gw_jsonw_str(&sys_w, "PATH", path);
}

/* Переименование: ARG {"FROM":"/maps/x.jsonc","TO":"/maps/y.jsonc"}. TO может быть
 * и коротким именем — тогда файл остаётся в каталоге FROM; переименование на
 * существующее имя — отказ.
 * Ответ: {"STA":"0x00000000","FROM":"...","TO":"..."} */
static void handle_file_rename(const uint8_t *data, uint32_t len) {
    char from_req[FS_PATH_MAX], to_req[FS_PATH_MAX], to_arg[FS_PATH_MAX];
    char from[FS_PATH_MAX], to[FS_PATH_MAX];
    char src[FS_FULL_MAX], dst[FS_FULL_MAX];
    struct stat st;
    GW_JSON_TOKENS(toks, SYS_ARG_MAX_TOKENS);
    gw_json_t json;

    if (!((data && len) && gw_json_parse(&json, (const char *)data, len, toks, SYS_ARG_MAX_TOKENS))) {
        resp_error(SYS_BAD_ARG, "ARG",
                   "ARG must be an object: {\"FROM\":\"/a.txt\",\"TO\":\"/b.txt\"}");
        return;
    }
    if (!gw_json_str(&json, "FROM", from_req, sizeof(from_req))) {
        resp_error(SYS_BAD_ARG, "FROM", "FROM is required, e.g. /maps/x.jsonc");
        return;
    }
    if (!fs_path_check(from_req, "FROM", from, sizeof(from)))
        return;
    if (!gw_json_str(&json, "TO", to_req, sizeof(to_req)) || to_req[0] == '\0') {
        resp_error(SYS_BAD_ARG, "TO", "TO is required, e.g. /maps/y.jsonc");
        return;
    }

    if (to_req[0] == '/') {
        strlcpy(to_arg, to_req, sizeof(to_arg));
    } else {
        /* короткое имя: тот же каталог, что у FROM */
        const char *slash = strrchr(from, '/');
        size_t dir_len = slash ? (size_t)(slash - from) + 1 : 0;
        if (dir_len + strlen(to_req) + 1 > sizeof(to_arg)) {
            resp_error(SYS_BAD_ARG, "TO", "TO is too long");
            return;
        }
        memcpy(to_arg, from, dir_len);
        strlcpy(to_arg + dir_len, to_req, sizeof(to_arg) - dir_len);
    }
    if (!fs_path_check(to_arg, "TO", to, sizeof(to)))
        return;

    if (!fs_full_path(from, src, sizeof(src)) || !fs_full_path(to, dst, sizeof(dst))) {
        resp_error(SYS_BAD_ARG, "TO", "Path is too long");
        return;
    }
    if (stat(src, &st) != 0) {
        ESP_LOGW(TAG, "Переименование: нет исходного файла %s", src);
        resp_error(SYS_BAD_ARG, "FROM", "File does not exist");
        return;
    }
    if (stat(dst, &st) == 0) {
        ESP_LOGW(TAG, "Переименование: цель уже существует %s", dst);
        resp_error(SYS_BAD_ARG, "EXIST", "Target name already exists");
        return;
    }
    if (rename(src, dst) != 0) {
        ESP_LOGE(TAG, "Не удалось переименовать %s в %s", src, dst);
        resp_error(SYS_INTERNAL, "FS", "Failed to rename the file");
        return;
    }
    LOGI(TAG, "Файл %s переименован в %s", src, dst);
    resp_ok();
    gw_jsonw_str(&sys_w, "FROM", from);
    gw_jsonw_str(&sys_w, "TO", to);
}

/* ── sys.file.mkdir: создание каталога ─────────────────────────────────────── */

static void handle_file_mkdir(const uint8_t *data, uint32_t len) {
    char path_req[FS_PATH_MAX], path[FS_PATH_MAX];
    char full[FS_FULL_MAX];
    struct stat st;
    GW_JSON_TOKENS(toks, SYS_ARG_MAX_TOKENS);
    gw_json_t json;

    if (!((data && len) && gw_json_parse(&json, (const char *)data, len, toks, SYS_ARG_MAX_TOKENS))) {
        resp_error(SYS_BAD_ARG, "ARG", "ARG must be an object: {\"PATH\":\"/maps\"}");
        return;
    }
    if (!gw_json_str(&json, "PATH", path_req, sizeof(path_req))) {
        resp_error(SYS_BAD_ARG, "PATH", "PATH is required, e.g. /maps");
        return;
    }
    if (!fs_path_check(path_req, "PATH", path, sizeof(path)))
        return;
    if (path[0] == '/' && path[1] == '\0') {
        resp_error(SYS_BAD_ARG, "EXIST", "Root directory already exists");
        return;
    }
    if (!fs_full_path(path, full, sizeof(full))) {
        resp_error(SYS_BAD_ARG, "PATH", "Path is too long");
        return;
    }
    if (stat(full, &st) == 0) {
        resp_error(SYS_BAD_ARG, "EXIST", st.st_mode & S_IFDIR ? "Directory already exists"
                                                              : "A file with this name exists");
        return;
    }
    if (mkdir(full, 0777) != 0) {
        ESP_LOGE(TAG, "Не удалось создать каталог %s", full);
        resp_error(SYS_INTERNAL, "FS", "Failed to create the directory");
        return;
    }
    LOGI(TAG, "Каталог %s создан", path);
    resp_ok();
    gw_jsonw_str(&sys_w, "PATH", path);
}

static void handle_file_rmdir(const uint8_t *data, uint32_t len) {
    char path_req[FS_PATH_MAX], path[FS_PATH_MAX];
    char full[FS_FULL_MAX];
    struct stat st;
    GW_JSON_TOKENS(toks, SYS_ARG_MAX_TOKENS);
    gw_json_t json;

    if (!((data && len) && gw_json_parse(&json, (const char *)data, len, toks, SYS_ARG_MAX_TOKENS))) {
        resp_error(SYS_BAD_ARG, "ARG", "ARG must be an object: {\"PATH\":\"/maps\"}");
        return;
    }
    if (!gw_json_str(&json, "PATH", path_req, sizeof(path_req))) {
        resp_error(SYS_BAD_ARG, "PATH", "PATH is required, e.g. /maps");
        return;
    }
    if (!fs_path_check(path_req, "PATH", path, sizeof(path)))
        return;
    if (path[0] == '/' && path[1] == '\0') {
        resp_error(SYS_BAD_ARG, "PATH", "The root directory cannot be removed");
        return;
    }
    if (!fs_full_path(path, full, sizeof(full))) {
        resp_error(SYS_BAD_ARG, "PATH", "Path is too long");
        return;
    }
    if (stat(full, &st) != 0 || !(st.st_mode & S_IFDIR)) {
        resp_error(SYS_BAD_ARG, "PATH", "Directory does not exist");
        return;
    }
    /* rmdir сам откажет, если каталог не пуст: считать содержимое не нужно —
     * файловая система это уже знает. */
    if (rmdir(full) != 0) {
        ESP_LOGW(TAG, "Не удалось удалить каталог %s (возможно, он не пуст)", full);
        resp_error(SYS_BAD_ARG, "NOTEMPTY", "Directory is not empty");
        return;
    }
    LOGI(TAG, "Каталог %s удалён", path);
    resp_ok();
    gw_jsonw_str(&sys_w, "PATH", path);
}

/* ── Обработчики вызовов ─────────────────────────────────────────────────── *
 *
 * Контракт (api.h): обработчик не блокирующий, отвечает сам и возвращает 0; call ==
 * NULL — вызов сняли. Ответ отправляется ровно один раз за вызов. Файловые сервисы
 * изменения — без политики на клиента: условия «сколько клиентов на точке доступа» к
 * правке файлов не относятся. */

static uint8_t sys_files_handler(void *call, void **context, uint8_t *data, uint32_t data_len) {
    (void)context;
    if (!call) return 0;
    resp_begin();
    handle_files(data, data_len);
    send_response(call);
    return 0;
}

static uint8_t sys_file_write_handler(void *call, void **context, uint8_t *data, uint32_t data_len) {
    (void)context;
    if (!call) return 0;
    resp_begin();
    handle_file_write(data, data_len);
    send_response(call);
    return 0;
}

static uint8_t sys_file_delete_handler(void *call, void **context, uint8_t *data, uint32_t data_len) {
    (void)context;
    if (!call) return 0;
    resp_begin();
    handle_file_delete(data, data_len);
    send_response(call);
    return 0;
}

static uint8_t sys_file_rename_handler(void *call, void **context, uint8_t *data, uint32_t data_len) {
    (void)context;
    if (!call) return 0;
    resp_begin();
    handle_file_rename(data, data_len);
    send_response(call);
    return 0;
}

static uint8_t sys_file_mkdir_handler(void *call, void **context, uint8_t *data, uint32_t data_len) {
    (void)context;
    if (!call) return 0;
    resp_begin();
    handle_file_mkdir(data, data_len);
    send_response(call);
    return 0;
}

static uint8_t sys_file_rmdir_handler(void *call, void **context, uint8_t *data, uint32_t data_len) {
    (void)context;
    if (!call) return 0;
    resp_begin();
    handle_file_rmdir(data, data_len);
    send_response(call);
    return 0;
}

esp_err_t apih_files_run(app_context_t *app) {
    uint8_t ok = 1;
    api_t *ap  = app->api_ap;
    api_t *sta = app->api_sta;

    /* Файловый менеджер доступен с ЛЮБОГО интерфейса, как и отдача файлов по HTTP,
     * поэтому регистрируется на обоих ядрах одинаково: страницы, карты регистров и
     * прошивку правят с рабочего места в сети, а не только с самой точки доступа.
     * Перестройка шлюза по-прежнему только у администратора точки доступа: wifi.sta/ap
     * и sys.net на STA-ядре — с контекстом «только чтение». */
    ok &= register_service(ap,  sys_files_handler,       ESP_WS_API_SYS_FILES,       NULL);
    ok &= register_service(sta, sys_files_handler,       ESP_WS_API_SYS_FILES,       NULL);
    ok &= register_service(ap,  sys_file_write_handler,  ESP_WS_API_SYS_FILE_WRITE,  NULL);
    ok &= register_service(sta, sys_file_write_handler,  ESP_WS_API_SYS_FILE_WRITE,  NULL);
    ok &= register_service(ap,  sys_file_delete_handler, ESP_WS_API_SYS_FILE_DELETE, NULL);
    ok &= register_service(sta, sys_file_delete_handler, ESP_WS_API_SYS_FILE_DELETE, NULL);
    ok &= register_service(ap,  sys_file_rename_handler, ESP_WS_API_SYS_FILE_RENAME, NULL);
    ok &= register_service(sta, sys_file_rename_handler, ESP_WS_API_SYS_FILE_RENAME, NULL);
    ok &= register_service(ap,  sys_file_mkdir_handler,  ESP_WS_API_SYS_FILE_MKDIR,  NULL);
    ok &= register_service(sta, sys_file_mkdir_handler,  ESP_WS_API_SYS_FILE_MKDIR,  NULL);
    ok &= register_service(ap,  sys_file_rmdir_handler,  ESP_WS_API_SYS_FILE_RMDIR,  NULL);
    ok &= register_service(sta, sys_file_rmdir_handler,  ESP_WS_API_SYS_FILE_RMDIR,  NULL);

    return ok ? ESP_OK : ESP_FAIL;
}
