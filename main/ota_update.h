#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#define ESPRESSO_OTA_MESSAGE_MAX 128
#define ESPRESSO_OTA_VERSION_MAX 32

typedef struct {
    char state[16];
    char message[ESPRESSO_OTA_MESSAGE_MAX];
    char next_version[ESPRESSO_OTA_VERSION_MAX];
    size_t bytes_written;
    size_t total_bytes;
    size_t partition_size;
    bool active;
    bool restart_pending;
} ota_update_status_t;

esp_err_t ota_update_init(void);
const char *ota_update_github_url(void);
void ota_update_get_status(ota_update_status_t *status);

esp_err_t ota_update_begin_upload(size_t image_size);
esp_err_t ota_update_write(const void *data, size_t length);
esp_err_t ota_update_finish_upload(void);
void ota_update_abort_upload(const char *message);

esp_err_t ota_update_start_github(void);
esp_err_t ota_update_schedule_restart(void);
