#include "printer_transport.h"

#include <stdio.h>
#include <string.h>

bool printer_transport_build(const printer_target_t *target,
                             printer_transport_endpoint_t *endpoint)
{
    if (!target || !endpoint || !target->address[0] || !target->port ||
        !target->resource_path[0] ||
        (target->secure_transport && !target->hostname[0])) {
        return false;
    }
    memset(endpoint, 0, sizeof(*endpoint));
    char host[ESPRESSO_ADDRESS_MAX + 3];
    if (strchr(target->address, ':')) {
        snprintf(host, sizeof(host), "[%s]", target->address);
    } else {
        snprintf(host, sizeof(host), "%s", target->address);
    }
    const char *http_scheme = target->secure_transport ? "https" : "http";
    const char *ipp_scheme = target->secure_transport ? "ipps" : "ipp";
    int http_length = snprintf(endpoint->http_url, sizeof(endpoint->http_url),
                               "%s://%s:%u%s", http_scheme, host,
                               target->port, target->resource_path);
    int uri_length = snprintf(endpoint->printer_uri,
                              sizeof(endpoint->printer_uri), "%s://%s:%u%s",
                              ipp_scheme, host, target->port,
                              target->resource_path);
    int authority_length = snprintf(endpoint->authority,
                                    sizeof(endpoint->authority), "%s://%s:%u",
                                    ipp_scheme, host, target->port);
    if (http_length < 0 || (size_t)http_length >= sizeof(endpoint->http_url) ||
        uri_length < 0 || (size_t)uri_length >= sizeof(endpoint->printer_uri) ||
        authority_length < 0 ||
        (size_t)authority_length >= sizeof(endpoint->authority)) {
        memset(endpoint, 0, sizeof(*endpoint));
        return false;
    }
    if (target->secure_transport) {
        snprintf(endpoint->certificate_name,
                 sizeof(endpoint->certificate_name), "%s", target->hostname);
        endpoint->verify_certificate = true;
    }
    return true;
}
