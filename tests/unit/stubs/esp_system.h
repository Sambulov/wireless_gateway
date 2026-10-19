#pragma once
/* Shadow of the ESP-IDF esp_system.h — host unit tests only.
 *
 * Службе прошивки от системного слоя нужен один вызов: перезагрузка после
 * успешной записи образа. На хосте она не перезагружает, а считает вызов
 * (system_stub.c, system_stub_restart_count). */

#ifdef __cplusplus
extern "C" {
#endif

void esp_restart(void);

#ifdef __cplusplus
}
#endif
