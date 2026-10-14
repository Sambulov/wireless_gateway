#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_image_format.h"
#include "freertos/task.h"

/* Firmware update over the web: the browser posts the .bin the build produces,
 * the handler streams it into the spare OTA partition and reboots into it. The
 * upload is accepted only from the softAP side with a single client — the same
 * rule every other configuration endpoint follows, since a wrong image locks
 * the operator out of the device completely. */

#define OTA_REBOOT_DELAY_MS 1500
#define OTA_CHUNK_SIZE      1024

static void vOtaRebootTask(void *arg) {
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(OTA_REBOOT_DELAY_MS));
    ESP_LOGW(TAG, "Rebooting into the uploaded firmware");
    esp_restart();
}

static esp_err_t ota_fail(httpd_req_t *req, const char *status, const char *msg) {
    ESP_LOGE(TAG, "OTA failed: %s", msg);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(req, msg);
}

esp_err_t ota_update_handler(httpd_req_t *req) {
    const int fd = httpd_req_to_sockfd(req);
    const uint8_t clients = wifi_ap_clients_count();

    if (!request_from_softap(fd) || clients != 1) {
        ESP_LOGW(TAG, "OTA refused (from_ap=%u clients=%u)", request_from_softap(fd), clients);
        return ota_fail(req, "403 Forbidden",
                        "Firmware can only be updated from the access point "
                        "while exactly one client is connected");
    }

    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part)
        return ota_fail(req, "500 Internal Server Error", "No OTA partition available");

    if (req->content_len < (int)sizeof(esp_image_header_t) || req->content_len > part->size) {
        char msg[128];
        snprintf(msg, sizeof(msg), "Firmware must be between %u and %u bytes",
                 (unsigned)sizeof(esp_image_header_t), (unsigned)part->size);
        return ota_fail(req, "400 Bad Request", msg);
    }

    ESP_LOGW(TAG, "Receiving %u bytes of firmware into partition '%s'",
             (unsigned)req->content_len, part->label);

    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(part, OTA_SIZE_UNKNOWN, &handle);
    if (err != ESP_OK) {
        char msg[96];
        snprintf(msg, sizeof(msg), "Cannot start the update: %s", esp_err_to_name(err));
        return ota_fail(req, "500 Internal Server Error", msg);
    }

    char *buf = malloc(OTA_CHUNK_SIZE);
    if (!buf) {
        esp_ota_abort(handle);
        return ota_fail(req, "500 Internal Server Error", "Out of memory");
    }

    int remaining = req->content_len;
    while (remaining > 0) {
        int received = httpd_req_recv(req, buf, (remaining < OTA_CHUNK_SIZE) ? remaining : OTA_CHUNK_SIZE);
        if (received == HTTPD_SOCK_ERR_TIMEOUT)
            continue;
        if (received <= 0) {
            free(buf);
            esp_ota_abort(handle);
            return ota_fail(req, "500 Internal Server Error", "Upload interrupted");
        }

        err = esp_ota_write(handle, buf, received);
        if (err != ESP_OK) {
            free(buf);
            esp_ota_abort(handle);
            /* A wrong file is a client mistake, not a server failure: the image
             * header is checked as soon as the first bytes arrive. */
            if (err == ESP_ERR_OTA_VALIDATE_FAILED)
                return ota_fail(req, "400 Bad Request",
                                "This file is not a firmware image for this device");
            char msg[96];
            snprintf(msg, sizeof(msg), "Writing failed: %s", esp_err_to_name(err));
            return ota_fail(req, "500 Internal Server Error", msg);
        }
        remaining -= received;
    }
    free(buf);

    /* esp_ota_end() checks that what arrived really is a bootable image */
    err = esp_ota_end(handle);
    if (err != ESP_OK) {
        char msg[96];
        snprintf(msg, sizeof(msg), "Not a valid firmware image: %s", esp_err_to_name(err));
        return ota_fail(req, "400 Bad Request", msg);
    }

    err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) {
        char msg[96];
        snprintf(msg, sizeof(msg), "Cannot select the new firmware: %s", esp_err_to_name(err));
        return ota_fail(req, "500 Internal Server Error", msg);
    }

    ESP_LOGW(TAG, "Firmware written to '%s', rebooting in %d ms", part->label, OTA_REBOOT_DELAY_MS);

    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_sendstr(req, "Firmware accepted, the device is rebooting into it");

    if (xTaskCreate(vOtaRebootTask, "ota_reboot", 2560, NULL, 5, NULL) != pdPASS)
        ESP_LOGE(TAG, "Failed to schedule the reboot");
    return ESP_OK;
}

/* URI handler for uploading new firmware */
httpd_uri_t ota_update = {
    .uri          = "/update",
    .method       = HTTP_POST,
    .handler      = ota_update_handler,
    .user_ctx     = NULL,
    .is_websocket = false
};
