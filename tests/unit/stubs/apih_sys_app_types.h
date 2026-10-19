#pragma once
/* Shadow of main/app.h for the services built in host tests — host unit tests
 * only.
 *
 * Службы (apih_fw.c, apih_sys_common.c) собираются без ESP-IDF, поэтому
 * apih_sys_internal.h под APIH_HOST_TEST подключает этот файл вместо настоящего
 * app.h. Своей копии теней здесь нет: рядом лежит stubs/app.h — тот самый,
 * которым уже пользуется apih_uart.c, и второй список типов разошёлся бы с ним
 * при первой же правке. */

#include "app.h"
