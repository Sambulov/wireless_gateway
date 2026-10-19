#include <string.h>

#include "fs_path.h"

/* Единственная проверка клиентского пути к файловой системе прибора.
 *
 * Одна проверка на все сервисы: листинг (sys.files) и изменение файлов
 * (sys.file.write/delete/rename). Своя, «чуть более мягкая» проверка в каждом
 * обработчике — это ровно тот случай, когда дырка появляется в одном из них;
 * здесь отсекаются "..", двойные и хвостовые слэши. Модуль не зависит от
 * платформы, поэтому проверяется хостовым тестом (tests/unit/FsPathTest.cpp). */

const char *fs_path_sanitize(const char *in, char *out, size_t out_size) {
    if (!in || !out || out_size < 2 || in[0] != '/')
        return NULL;
    size_t o = 0;
    const char *p = in;
    while (*p) {
        while (*p == '/') p++;                    /* схлопываем слэши */
        const char *seg = p;
        while (*p && *p != '/') p++;
        size_t len = (size_t)(p - seg);
        if (len == 0)
            break;
        if ((len == 1 && seg[0] == '.') ||
            (len == 2 && seg[0] == '.' && seg[1] == '.'))
            return NULL;                          /* выход за пределы корня */
        if (o + len + 2 >= out_size)
            return NULL;
        out[o++] = '/';
        memcpy(out + o, seg, len);
        o += len;
    }
    if (o == 0)
        out[o++] = '/';
    out[o] = '\0';
    return out;
}

uint8_t fs_path_temp(const char *path, char *out, size_t out_size) {
    if (!path || !out || out_size < 2)
        return 0;

    const char *slash = strrchr(path, '/');
    const char *name = slash ? (slash + 1) : path;
    /* Путь без имени — это каталог или корень: у них нет временного файла. */
    if (name[0] == '\0')
        return 0;

    const size_t dir_len  = slash ? (size_t)(slash - path) + 1 : 0;
    const size_t name_len = strlen(name);
    const char   suffix[] = ".part";
    /* "<каталог>" + "." + "<имя>" + ".part" + '\0' */
    if (dir_len + 1 + name_len + sizeof(suffix) > out_size)
        return 0;

    memcpy(out, path, dir_len);
    out[dir_len] = '.';
    memcpy(out + dir_len + 1, name, name_len);
    memcpy(out + dir_len + 1 + name_len, suffix, sizeof(suffix));
    return 1;
}
