#include "printer_transport.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static printer_target_t target(void)
{
    printer_target_t value = {0};
    strcpy(value.address, "192.0.2.4");
    strcpy(value.hostname, "legacy-printer.local");
    strcpy(value.resource_path, "/ipp/print");
    value.port = 631;
    return value;
}

int main(void)
{
    printer_target_t value = target();
    printer_transport_endpoint_t endpoint;
    assert(printer_transport_build(&value, &endpoint));
    assert(strcmp(endpoint.http_url,
                  "http://192.0.2.4:631/ipp/print") == 0);
    assert(strcmp(endpoint.printer_uri,
                  "ipp://192.0.2.4:631/ipp/print") == 0);
    assert(!endpoint.verify_certificate);

    value.secure_transport = true;
    value.port = 443;
    assert(printer_transport_build(&value, &endpoint));
    assert(strcmp(endpoint.http_url,
                  "https://192.0.2.4:443/ipp/print") == 0);
    assert(strcmp(endpoint.printer_uri,
                  "ipps://192.0.2.4:443/ipp/print") == 0);
    assert(endpoint.verify_certificate);
    assert(strcmp(endpoint.certificate_name, "legacy-printer.local") == 0);

    value.hostname[0] = '\0';
    assert(!printer_transport_build(&value, &endpoint));
    puts("IPPS transport policy tests passed");
    return 0;
}
