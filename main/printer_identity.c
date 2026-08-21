#include "printer_identity.h"

#include <stdio.h>

#include "esp_mac.h"

void printer_identity_uuid(char *buffer, size_t buffer_size)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(buffer, buffer_size,
             "00000000-0000-4000-8000-%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}
