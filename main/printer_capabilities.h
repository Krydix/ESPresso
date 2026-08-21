#pragma once

#include "esp_err.h"
#include "printer_types.h"

/* Query and merge the metadata-only capability subset ESPresso can relay. */
esp_err_t printer_capabilities_probe(printer_target_t *target);
