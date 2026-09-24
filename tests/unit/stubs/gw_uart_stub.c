#include "CodeLib.h"
#include "uart.h"
#include "gw_uart_stub.h"
#include <string.h>

/* Required by uart.h but defined in the real uart.c */
const gw_uart_config_t gw_uart_config_default = {
    .bits   = GW_UART_WORD_8BIT,
    .parity = GW_UART_PARITY_NONE,
    .stop   = GW_UART_STOP_BITS1,
    .boud   = 115200
};

#define MAX_PORTS     2
#define MAX_WRITE_LEN 512

static struct {
    void            *desc;
    gw_uart_config_t cfg;
    int              write_calls;
    int              last_write_len;
    uint8_t          last_write_buf[MAX_WRITE_LEN];
    uint8_t          echo;
} g_slots[MAX_PORTS];

static int find_slot(void *desc) {
    for (int i = 0; i < MAX_PORTS; i++)
        if (g_slots[i].desc == desc) return i;
    for (int i = 0; i < MAX_PORTS; i++)
        if (!g_slots[i].desc) { g_slots[i].desc = desc; return i; }
    return 0;
}

void gw_uart_stub_reset(void) { memset(g_slots, 0, sizeof(g_slots)); }

/* ── uart.h implementation ─────────────────────────────────────────────── */

uint8_t gw_uart_init(void *desc, gw_uart_port_t port, uint32_t buffer_size) {
    (void)port; (void)buffer_size;
    find_slot(desc);
    return 1;
}

uint8_t gw_uart_get(void *desc, gw_uart_config_t *out) {
    *out = g_slots[find_slot(desc)].cfg;
    return 1;
}

uint8_t gw_uart_set(void *desc, const gw_uart_config_t *cfg) {
    g_slots[find_slot(desc)].cfg = *cfg;
    return 1;
}

int32_t gw_uart_write(void *desc, const uint8_t *buf, uint16_t size) {
    int s = find_slot(desc);
    g_slots[s].write_calls++;
    int cplen = size < MAX_WRITE_LEN ? size : MAX_WRITE_LEN;
    memcpy(g_slots[s].last_write_buf, buf, cplen);
    g_slots[s].last_write_len = size;
    return size;
}

int32_t gw_uart_read(void *desc, uint8_t *buf, uint16_t size) {
    (void)desc; (void)buf; (void)size; return 0;
}

int32_t gw_uart_available_write(void *desc) { (void)desc; return 512; }

void gw_uart_set_echo(void *desc, uint8_t enabled) {
    g_slots[find_slot(desc)].echo = enabled;
}

uint8_t gw_uart_get_echo(void *desc) {
    return g_slots[find_slot(desc)].echo;
}

void gw_uart_on_receive_subscribe(void *desc, delegate_t *d) {
    (void)desc; (void)d;
}

void gw_uart_lock_rx(void *desc)   { (void)desc; }
void gw_uart_unlock_rx(void *desc) { (void)desc; }

/* ── Inspection ────────────────────────────────────────────────────────── */

gw_uart_config_t gw_uart_stub_last_config(void *desc) {
    return g_slots[find_slot(desc)].cfg;
}
int gw_uart_stub_write_calls(void *desc) {
    return g_slots[find_slot(desc)].write_calls;
}
int gw_uart_stub_last_write_len(void *desc) {
    return g_slots[find_slot(desc)].last_write_len;
}
const uint8_t *gw_uart_stub_last_write_buf(void *desc) {
    return g_slots[find_slot(desc)].last_write_buf;
}
