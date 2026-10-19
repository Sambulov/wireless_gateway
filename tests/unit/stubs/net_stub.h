#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Тень сети для хостовых тестов (stubs/net_stub.c): сколько станций на точке
 * доступа. Служба прошивки спрашивает это число пропуском — принимать образ
 * можно, только пока администратор на точке один. */
void net_stub_set_clients(uint8_t clients);
void net_stub_reset(void);

#ifdef __cplusplus
}
#endif
