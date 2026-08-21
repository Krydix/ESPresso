#pragma once

#include <stddef.h>

#include "printer_types.h"

/* Stable bridge identity; intentionally different from the physical printer UUID. */
void printer_identity_uuid(char *buffer, size_t buffer_size);

/* Stable logical-printer identity.  The same target survives rescans and
 * reboots, while switching physical printers cannot reuse a client's cached
 * capabilities for the previous target. */
void printer_identity_target_uuid(const printer_target_t *target,
                                  char *buffer, size_t buffer_size);
