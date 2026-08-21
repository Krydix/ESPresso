#pragma once

#include <stddef.h>

/* Stable bridge identity; intentionally different from the physical printer UUID. */
void printer_identity_uuid(char *buffer, size_t buffer_size);
