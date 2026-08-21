#pragma once

#include "esp_err.h"
#include "printer_types.h"

/* Query and merge the metadata-only capability subset ESPresso can relay. */
esp_err_t printer_capabilities_probe(printer_target_t *target);

/* Refresh the selected printer at most once per minute unless forced. */
esp_err_t printer_capabilities_refresh_selected(bool force,
                                                printer_target_t *refreshed,
                                                bool *changed);
