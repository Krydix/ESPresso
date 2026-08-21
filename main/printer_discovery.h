#pragma once

#include <stddef.h>

#include "app_state.h"
#include "esp_err.h"

#define ESPRESSO_PRINTER_MAX 12

esp_err_t printer_discovery_init(void);
esp_err_t printer_discovery_scan(void);
size_t printer_discovery_count(void);
bool printer_discovery_get(size_t index, printer_target_t *target);
esp_err_t printer_discovery_select(size_t index);
esp_err_t printer_discovery_clear_selection(void);
esp_err_t printer_discovery_advertise_selected(void);
void printer_discovery_network_ready(void);
