/* Проверка пути к файловой системе прибора (main/fs_path.c).
 *
 * Клиентский путь приходит из сети — от файлового менеджера (листинг sys.files,
 * запись/удаление/переименование sys.file.*). Модуль платформы не знает, поэтому
 * проверяется здесь: на устройстве эти же правила стоят на всех файловых
 * сервисах сразу, и «дырка» в одном из них означала бы доступ за пределы
 * файловой системы.
 *
 * Список проверок:
 * [x] нормальный путь остаётся как есть, хвостовой слэш и двойные слэши схлопываются
 * [x] корень — это "/"
 * [x] относительный путь, пустой, NULL и путь без ведущего слэша — отказ
 * [x] "." и ".." как сегмент пути — отказ (в том числе в середине)
 * [x] путь длиннее буфера — отказ, а не обрезание
 * [x] временный файл — "<каталог>/.<имя>.part" рядом с целью (в листинге не виден)
 * [x] у каталога и корня временного файла нет; тесный буфер — отказ
 */

#include "CppUTest/TestHarness.h"

#include <string.h>

#include "fs_path.h"

TEST_GROUP(FsPath) {
    char out[64];

    void setup() {
        memset(out, 0x5a, sizeof(out));
    }

    /* Разбор удался и дал ожидаемый путь */
    void checkOk(const char *in, const char *expect) {
        const char *ret = fs_path_sanitize(in, out, sizeof(out));
        CHECK_TRUE(ret != NULL);
        if (ret) STRCMP_EQUAL(expect, out);
    }

    void checkRejected(const char *in) {
        CHECK_TRUE(fs_path_sanitize(in, out, sizeof(out)) == NULL);
    }
};

TEST(FsPath, AbsolutePathsAreKept) {
    checkOk("/", "/");
    checkOk("/index.html", "/index.html");
    checkOk("/maps/x.jsonc", "/maps/x.jsonc");
    checkOk("/terminal/script.js", "/terminal/script.js");
}

TEST(FsPath, SlashesAreNormalized) {
    checkOk("//", "/");
    checkOk("///maps", "/maps");
    checkOk("/maps/", "/maps");
    checkOk("/maps//x.jsonc", "/maps/x.jsonc");
    checkOk("/maps/x.jsonc/", "/maps/x.jsonc");
}

TEST(FsPath, BadPathsAreRejected) {
    checkRejected(NULL);
    checkRejected("");
    checkRejected("maps/x.jsonc");     /* нет ведущего слэша */
    checkRejected("./maps");
    checkRejected("/.");
    checkRejected("/./maps");
    checkRejected("/..");
    checkRejected("/../etc/passwd");
    checkRejected("/maps/../secret");
    checkRejected("/maps/..");
    /* ".." как ЧАСТЬ имени — не выход за пределы: это обычное имя файла */
    checkOk("/maps/..hidden", "/maps/..hidden");
    checkOk("/maps/x..y", "/maps/x..y");
}

TEST(FsPath, TooLongPathIsRejectedNotTruncated) {
    char small[9];
    /* "/abcdef" — слэш и шесть символов: в буфер из 9 байт влезает вместе с
     * завершающим нулём (сам разбор оставляет один байт запаса) */
    CHECK_TRUE(fs_path_sanitize("/abcdef", small, sizeof(small)) != NULL);
    STRCMP_EQUAL("/abcdef", small);
    /* На символ длиннее — уже отказ: обрезанного пути быть не должно */
    CHECK_TRUE(fs_path_sanitize("/abcdefg", small, sizeof(small)) == NULL);
    CHECK_TRUE(fs_path_sanitize("/maps/x.jsonc", small, sizeof(small)) == NULL);
    /* Буфера под сам путь и ноль не хватает */
    CHECK_TRUE(fs_path_sanitize("/", small, 1) == NULL);
    CHECK_TRUE(fs_path_sanitize("/", NULL, sizeof(small)) == NULL);
}

TEST(FsPath, TempNameSitsNextToTheTargetAndIsHidden) {
    char tmp[64];

    CHECK_TRUE(fs_path_temp("/maps/x.jsonc", tmp, sizeof(tmp)));
    STRCMP_EQUAL("/maps/.x.jsonc.part", tmp);      /* та же папка, имя с точки */

    CHECK_TRUE(fs_path_temp("/x", tmp, sizeof(tmp)));
    STRCMP_EQUAL("/.x.part", tmp);
}

TEST(FsPath, TempNameIsRefusedForDirectoriesAndTightBuffers) {
    char tmp[9];

    CHECK_FALSE(fs_path_temp("/", tmp, sizeof(tmp)));            /* корень */
    CHECK_FALSE(fs_path_temp("/maps/", tmp, sizeof(tmp)));       /* каталог */
    CHECK_FALSE(fs_path_temp("/mm", tmp, sizeof(tmp)));          /* "/.mm.part" — 10 байт */
    CHECK_TRUE (fs_path_temp("/m", tmp, sizeof(tmp)));           /* "/.m.part" — 9 байт */
    STRCMP_EQUAL("/.m.part", tmp);
    CHECK_FALSE(fs_path_temp(NULL, tmp, sizeof(tmp)));
}
