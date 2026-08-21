#pragma once

#include <stdbool.h>

#include "printer_types.h"

typedef struct {
    char http_url[ESPRESSO_ADDRESS_MAX + ESPRESSO_PATH_MAX + 32];
    char printer_uri[ESPRESSO_ADDRESS_MAX + ESPRESSO_PATH_MAX + 32];
    char authority[ESPRESSO_ADDRESS_MAX + 32];
    char certificate_name[ESPRESSO_HOST_MAX];
    bool verify_certificate;
} printer_transport_endpoint_t;

/* Build the HTTP/IPPS projection of a discovered endpoint. IPPS is accepted
 * only with a discovered hostname so TLS can authenticate the peer. */
bool printer_transport_build(const printer_target_t *target,
                             printer_transport_endpoint_t *endpoint);
