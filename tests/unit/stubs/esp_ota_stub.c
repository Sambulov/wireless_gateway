#include "esp_ota_ops.h"
#include "esp_ota_stub.h"

#include <string.h>

/* Прибор для этой заглушки: раздел обновления, OTA-сессия и сам образ — в
 * памяти. Ведёт себя как esp_ota: без begin писать нечего, end закрывает сессию
 * и освобождает её (после отказа esp_ota_end в настоящем IDF тоже освобождает
 * сессию сам — поэтому abort после него служба не зовёт). */
typedef struct {
    esp_partition_t part;
    esp_ota_handle_t handle;
    uint8_t  active;
    int      begin_err, write_err, end_err, boot_err;
    int      begin_calls, write_calls, end_calls, abort_calls, boot_calls;
    size_t   begin_size;
    uint32_t bytes;
    uint8_t  data[ESP_OTA_STUB_DATA_MAX];
} ota_stub_t;

static ota_stub_t stub;

static void stub_init(void) {
    memset(&stub, 0, sizeof(stub));
    stub.part.address = 0x110000;
    stub.part.size    = ESP_OTA_STUB_PART_SIZE;
    stub.part.label   = "ota_1";
}

static void stub_ensure_init(void) {
    if (stub.part.label == NULL) stub_init();
}

void esp_ota_stub_reset(void) { stub_init(); }

void esp_ota_stub_set_partition_size(uint32_t size) {
    stub_ensure_init();
    stub.part.size = size;
}

void esp_ota_stub_set_begin_error(int err) { stub_ensure_init(); stub.begin_err = err; }
void esp_ota_stub_set_write_error(int err) { stub_ensure_init(); stub.write_err = err; }
void esp_ota_stub_set_end_error(int err)   { stub_ensure_init(); stub.end_err = err; }
void esp_ota_stub_set_boot_error(int err)  { stub_ensure_init(); stub.boot_err = err; }

int      esp_ota_stub_begin_calls(void) { return stub.begin_calls; }
int      esp_ota_stub_write_calls(void) { return stub.write_calls; }
int      esp_ota_stub_end_calls(void)   { return stub.end_calls; }
int      esp_ota_stub_abort_calls(void) { return stub.abort_calls; }
int      esp_ota_stub_set_boot_calls(void) { return stub.boot_calls; }
size_t   esp_ota_stub_begin_size(void)  { return stub.begin_size; }
uint32_t esp_ota_stub_bytes(void)       { return stub.bytes; }
const uint8_t *esp_ota_stub_data(void)  { return stub.data; }
int      esp_ota_stub_active(void)      { return stub.active; }

/* ── esp_ota_ops.h ──────────────────────────────────────────────────────── */

const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *start_from) {
    (void)start_from;
    stub_ensure_init();
    return stub.part.size ? &stub.part : NULL;
}

const esp_partition_t *esp_ota_get_running_partition(void) {
    stub_ensure_init();
    return &stub.part;
}

esp_err_t esp_ota_begin(const esp_partition_t *partition, size_t image_size,
                        esp_ota_handle_t *out_handle) {
    stub_ensure_init();
    stub.begin_calls++;
    stub.begin_size = image_size;
    if (!partition || !out_handle) return ESP_ERR_INVALID_ARG;
    if (stub.begin_err) return stub.begin_err;

    stub.handle = 0x1234;
    stub.active = 1;
    stub.bytes  = 0;
    *out_handle = stub.handle;
    return ESP_OK;
}

esp_err_t esp_ota_write(esp_ota_handle_t handle, const void *data, size_t size) {
    stub_ensure_init();
    stub.write_calls++;
    if (!stub.active || handle != stub.handle) return ESP_ERR_INVALID_STATE;
    if (stub.write_err) return stub.write_err;
    if (stub.bytes + size > sizeof(stub.data)) return ESP_ERR_INVALID_SIZE;

    memcpy(stub.data + stub.bytes, data, size);
    stub.bytes += (uint32_t)size;
    return ESP_OK;
}

esp_err_t esp_ota_end(esp_ota_handle_t handle) {
    stub_ensure_init();
    stub.end_calls++;
    if (!stub.active || handle != stub.handle) return ESP_ERR_NOT_FOUND;
    stub.active = 0;             /* сессия освобождается в любом случае */
    return stub.end_err;
}

esp_err_t esp_ota_abort(esp_ota_handle_t handle) {
    stub_ensure_init();
    stub.abort_calls++;
    if (!stub.active || handle != stub.handle) return ESP_ERR_NOT_FOUND;
    stub.active = 0;
    return ESP_OK;
}

esp_err_t esp_ota_set_boot_partition(const esp_partition_t *partition) {
    stub_ensure_init();
    stub.boot_calls++;
    if (!partition) return ESP_ERR_INVALID_ARG;
    return stub.boot_err;
}
