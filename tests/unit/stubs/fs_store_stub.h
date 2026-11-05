#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Корень «смонтированной ФС» для fs_store_root() и её объём для fs_store_usage(). */
void fs_store_stub_set_root(const char *root);
void fs_store_stub_set_usage(size_t total, size_t used);
#ifdef __cplusplus
}
#endif
