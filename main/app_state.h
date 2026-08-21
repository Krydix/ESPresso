#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "printer_types.h"

esp_err_t app_state_init(void);
void app_state_set_wifi(bool connected, const char *ssid, const char *ip_address);
void app_state_get_wifi(bool *connected, char *ssid, size_t ssid_size,
                        char *ip_address, size_t ip_size);
esp_err_t app_state_set_target(const printer_target_t *target);
/* Replace the in-memory profile without another NVS write during live refresh. */
esp_err_t app_state_update_target(const printer_target_t *target);
bool app_state_get_target(printer_target_t *target);
esp_err_t app_state_clear_target(void);
