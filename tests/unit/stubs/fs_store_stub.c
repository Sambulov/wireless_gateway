/* fs_store.h на хосте: «смонтированная ФС» — каталог, который задал тест
 * (обычно mkdtemp), объём — какой задал тест. Файлы внутри — настоящие. */
#include <stdio.h>
#include <string.h>

#include "fs_store.h"
#include "fs_store_stub.h"

static char   stub_root[256] = "/tmp";
static size_t stub_total = 2 * 1024 * 1024;
static size_t stub_used;

void fs_store_stub_set_root(const char *root) {
    snprintf(stub_root, sizeof(stub_root), "%s", root ? root : "");
}

void fs_store_stub_set_usage(size_t total, size_t used) {
    stub_total = total;
    stub_used  = used;
}

const char *fs_store_root(void) { return stub_root; }

uint8_t fs_store_usage(size_t *total, size_t *used) {
    if (total) *total = stub_total;
    if (used)  *used  = stub_used;
    return 1;
}
