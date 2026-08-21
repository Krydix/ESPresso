#include "printer_identity.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_mac.h"

static uint64_t fnv1a(uint64_t hash, const void *data, size_t length)
{
    const uint8_t *bytes = data;
    for (size_t i = 0; i < length; ++i) {
        hash ^= bytes[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static uint64_t hash_text(uint64_t hash, const char *value)
{
    static const uint8_t separator = 0xff;
    if (value) {
        hash = fnv1a(hash, value, strlen(value));
    }
    return fnv1a(hash, &separator, sizeof(separator));
}

void printer_identity_uuid(char *buffer, size_t buffer_size)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(buffer, buffer_size,
             "00000000-0000-4000-8000-%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void printer_identity_target_uuid(const printer_target_t *target,
                                  char *buffer, size_t buffer_size)
{
    if (!target) {
        printer_identity_uuid(buffer, buffer_size);
        return;
    }

    uint64_t hash = UINT64_C(14695981039346656037);
    if (target->uuid[0]) {
        hash = hash_text(hash, target->uuid);
    } else {
        hash = hash_text(hash, target->hostname);
        hash = hash_text(hash, target->instance);
    }
    hash = hash_text(hash, target->resource_path);
    hash = fnv1a(hash, &target->port, sizeof(target->port));

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    uint32_t time_low = (uint32_t)(hash >> 32);
    uint16_t time_mid = (uint16_t)(hash >> 16);
    uint16_t time_high = (uint16_t)(hash & 0x0fff);
    uint16_t clock_seq = (uint16_t)((hash >> 4) & 0x0fff);
    snprintf(buffer, buffer_size,
             "%08lx-%04x-4%03x-8%03x-%02x%02x%02x%02x%02x%02x",
             (unsigned long)time_low, time_mid, time_high, clock_seq,
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}
