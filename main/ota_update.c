#include "ota_update.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifndef ESPRESSO_OTA_URL
#define ESPRESSO_OTA_URL                                                        \
    "https://krydix.github.io/ESPresso/firmware/" CONFIG_IDF_TARGET            \
    "/espresso.bin"
#endif

static const char *TAG = "espresso_ota";
static portMUX_TYPE s_status_lock = portMUX_INITIALIZER_UNLOCKED;
static ota_update_status_t s_status = {
    .state = "idle",
    .message = "Ready to update",
};
static esp_ota_handle_t s_upload_handle;
static const esp_partition_t *s_upload_partition;

static void set_failure(esp_err_t error, const char *context)
{
    char message[ESPRESSO_OTA_MESSAGE_MAX];
    snprintf(message, sizeof(message), "%s: %s", context, esp_err_to_name(error));
    portENTER_CRITICAL(&s_status_lock);
    strlcpy(s_status.state, "error", sizeof(s_status.state));
    strlcpy(s_status.message, message, sizeof(s_status.message));
    s_status.active = false;
    s_status.restart_pending = false;
    portEXIT_CRITICAL(&s_status_lock);
    ESP_LOGE(TAG, "%s", message);
}

static void set_progress(size_t bytes_written, size_t total_bytes)
{
    portENTER_CRITICAL(&s_status_lock);
    s_status.bytes_written = bytes_written;
    if (total_bytes > 0) {
        s_status.total_bytes = total_bytes;
    }
    portEXIT_CRITICAL(&s_status_lock);
}

static void set_message(const char *message)
{
    portENTER_CRITICAL(&s_status_lock);
    strlcpy(s_status.message, message, sizeof(s_status.message));
    portEXIT_CRITICAL(&s_status_lock);
}

static void set_next_version(const char *version)
{
    portENTER_CRITICAL(&s_status_lock);
    strlcpy(s_status.next_version, version ? version : "", sizeof(s_status.next_version));
    portEXIT_CRITICAL(&s_status_lock);
}

static void set_ready(const char *version)
{
    portENTER_CRITICAL(&s_status_lock);
    strlcpy(s_status.state, "ready", sizeof(s_status.state));
    strlcpy(s_status.message, "Update installed; restarting", sizeof(s_status.message));
    strlcpy(s_status.next_version, version ? version : "", sizeof(s_status.next_version));
    s_status.active = false;
    s_status.restart_pending = true;
    portEXIT_CRITICAL(&s_status_lock);
}

esp_err_t ota_update_init(void)
{
    const esp_partition_t *partition = esp_ota_get_next_update_partition(NULL);
    if (!partition) {
        ESP_LOGE(TAG, "no inactive OTA app partition found");
        return ESP_ERR_NOT_FOUND;
    }
    portENTER_CRITICAL(&s_status_lock);
    s_status.partition_size = partition->size;
    portEXIT_CRITICAL(&s_status_lock);
    ESP_LOGI(TAG, "inactive OTA slot %s has %lu bytes", partition->label,
             (unsigned long)partition->size);
    return ESP_OK;
}

const char *ota_update_github_url(void)
{
    return ESPRESSO_OTA_URL;
}

void ota_update_get_status(ota_update_status_t *status)
{
    if (!status) {
        return;
    }
    portENTER_CRITICAL(&s_status_lock);
    *status = s_status;
    portEXIT_CRITICAL(&s_status_lock);
}

static esp_err_t reserve_update(const char *state, const char *message, size_t total)
{
    esp_err_t result = ESP_OK;
    portENTER_CRITICAL(&s_status_lock);
    if (s_status.active || s_status.restart_pending) {
        result = ESP_ERR_INVALID_STATE;
    } else {
        strlcpy(s_status.state, state, sizeof(s_status.state));
        strlcpy(s_status.message, message, sizeof(s_status.message));
        s_status.next_version[0] = '\0';
        s_status.bytes_written = 0;
        s_status.total_bytes = total;
        s_status.active = true;
    }
    portEXIT_CRITICAL(&s_status_lock);
    return result;
}

esp_err_t ota_update_begin_upload(size_t image_size)
{
    const esp_partition_t *partition = esp_ota_get_next_update_partition(NULL);
    if (!partition || image_size == 0 || image_size > partition->size) {
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t error = reserve_update("uploading", "Receiving firmware", image_size);
    if (error != ESP_OK) {
        return error;
    }
    s_upload_partition = partition;
    error = esp_ota_begin(partition, image_size, &s_upload_handle);
    if (error != ESP_OK) {
        s_upload_partition = NULL;
        set_failure(error, "Could not prepare OTA slot");
    }
    return error;
}

esp_err_t ota_update_write(const void *data, size_t length)
{
    if (!s_upload_partition || !data || length == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t error = esp_ota_write(s_upload_handle, data, length);
    if (error == ESP_OK) {
        ota_update_status_t status;
        ota_update_get_status(&status);
        set_progress(status.bytes_written + length, status.total_bytes);
    }
    return error;
}

esp_err_t ota_update_finish_upload(void)
{
    if (!s_upload_partition) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_partition_t *partition = s_upload_partition;
    s_upload_partition = NULL;
    esp_err_t error = esp_ota_end(s_upload_handle);
    if (error != ESP_OK) {
        set_failure(error, "Firmware validation failed");
        return error;
    }
    esp_app_desc_t description = {0};
    error = esp_ota_get_partition_description(partition, &description);
    if (error != ESP_OK) {
        set_failure(error, "Upload is not an ESP32 app firmware image");
        return error;
    }
    error = esp_ota_set_boot_partition(partition);
    if (error != ESP_OK) {
        set_failure(error, "Could not select updated firmware");
        return error;
    }
    set_ready(description.version);
    ESP_LOGI(TAG, "uploaded firmware %s is ready in %s", description.version,
             partition->label);
    return ESP_OK;
}

void ota_update_abort_upload(const char *message)
{
    if (s_upload_partition) {
        esp_ota_abort(s_upload_handle);
        s_upload_partition = NULL;
    }
    portENTER_CRITICAL(&s_status_lock);
    strlcpy(s_status.state, "error", sizeof(s_status.state));
    strlcpy(s_status.message, message ? message : "Upload interrupted",
            sizeof(s_status.message));
    s_status.active = false;
    s_status.restart_pending = false;
    portEXIT_CRITICAL(&s_status_lock);
}

static void github_update_task(void *parameter)
{
    (void)parameter;
    if (time(NULL) < 1704067200) {
        set_message("Synchronizing clock for secure download");
        esp_sntp_config_t sntp_config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
        esp_err_t time_error = esp_netif_sntp_init(&sntp_config);
        if (time_error != ESP_OK && time_error != ESP_ERR_INVALID_STATE) {
            set_failure(time_error, "Could not start clock synchronization");
            vTaskDelete(NULL);
            return;
        }
        for (int attempt = 0; attempt < 5 && time(NULL) < 1704067200; ++attempt) {
            esp_netif_sntp_sync_wait(pdMS_TO_TICKS(2000));
        }
        if (time(NULL) < 1704067200) {
            set_failure(ESP_ERR_TIMEOUT, "Clock synchronization timed out");
            vTaskDelete(NULL);
            return;
        }
    }
    set_message("Connecting to GitHub Pages");
    esp_http_client_config_t http_config = {
        .url = ESPRESSO_OTA_URL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .keep_alive_enable = true,
        .buffer_size = 4096,
    };
    esp_https_ota_config_t ota_config = {
        .http_config = &http_config,
    };
    esp_https_ota_handle_t handle = NULL;
    esp_err_t error = esp_https_ota_begin(&ota_config, &handle);
    if (error != ESP_OK) {
        set_failure(error, "Could not connect to GitHub Pages");
        vTaskDelete(NULL);
        return;
    }

    esp_app_desc_t description = {0};
    error = esp_https_ota_get_img_desc(handle, &description);
    if (error != ESP_OK) {
        esp_https_ota_abort(handle);
        set_failure(error, "Could not read firmware metadata");
        vTaskDelete(NULL);
        return;
    }
    set_next_version(description.version);
    int image_size = esp_https_ota_get_image_size(handle);
    if (image_size > 0) {
        set_progress(0, (size_t)image_size);
    }
    ESP_LOGI(TAG, "downloading firmware %s from GitHub Pages", description.version);

    do {
        error = esp_https_ota_perform(handle);
        int written = esp_https_ota_get_image_len_read(handle);
        image_size = esp_https_ota_get_image_size(handle);
        if (written >= 0) {
            set_progress((size_t)written, image_size > 0 ? (size_t)image_size : 0);
        }
    } while (error == ESP_ERR_HTTPS_OTA_IN_PROGRESS);

    if (error != ESP_OK || !esp_https_ota_is_complete_data_received(handle)) {
        esp_https_ota_abort(handle);
        set_failure(error == ESP_OK ? ESP_ERR_INVALID_SIZE : error,
                    "Firmware download failed");
        vTaskDelete(NULL);
        return;
    }
    error = esp_https_ota_finish(handle);
    if (error != ESP_OK) {
        set_failure(error, "Firmware validation failed");
        vTaskDelete(NULL);
        return;
    }

    set_ready(description.version);
    ESP_LOGI(TAG, "GitHub firmware %s installed; restarting", description.version);
    vTaskDelay(pdMS_TO_TICKS(2500));
    esp_restart();
}

esp_err_t ota_update_start_github(void)
{
    esp_err_t error = reserve_update("downloading", "Connecting to GitHub Pages", 0);
    if (error != ESP_OK) {
        return error;
    }
    if (xTaskCreate(github_update_task, "espresso_ota", 10240, NULL, 5, NULL) != pdPASS) {
        set_failure(ESP_ERR_NO_MEM, "Could not start update task");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void restart_task(void *parameter)
{
    (void)parameter;
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

esp_err_t ota_update_schedule_restart(void)
{
    if (xTaskCreate(restart_task, "ota_restart", 2048, NULL, 5, NULL) != pdPASS) {
        set_failure(ESP_ERR_NO_MEM, "Update installed; automatic restart failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
