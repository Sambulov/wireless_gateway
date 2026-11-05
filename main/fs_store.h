/* Хранилище файлов прибора — то, что нужно файловой службе (apih_files.c) от
 * платформы: где смонтирована файловая система и насколько она заполнена.
 * Работа с файлами внутри неё — обычный POSIX (opendir/stat/fopen/rename…),
 * он есть и на прошивке (VFS), и на хосте.
 *
 * Интерфейс принадлежит службе (правило 3): прошивка реализует его поверх
 * LittleFS (file_system_helpers.c), хостовые тесты — временным каталогом
 * (tests/unit/stubs/fs_store_stub.c), и служба проверяется на хосте с
 * настоящими файлами. */
#ifndef FS_STORE_H
#define FS_STORE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Каталог, в котором смонтирована ФС ("/lfs"), без хвостового '/'. */
const char *fs_store_root(void);

/* Объём и занятое место ФС в байтах. 0 — узнать не удалось (оба обнулены). */
uint8_t fs_store_usage(size_t *total, size_t *used);

#ifdef __cplusplus
}
#endif

#endif /* FS_STORE_H */
