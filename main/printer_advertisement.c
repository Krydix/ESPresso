#include "printer_advertisement.h"

#include <stdio.h>
#include <string.h>

static bool csv_contains(const char *csv, const char *wanted)
{
    size_t wanted_length = strlen(wanted);
    const char *cursor = csv;
    while (cursor && *cursor) {
        const char *comma = strchr(cursor, ',');
        const char *end = comma ? comma : cursor + strlen(cursor);
        while (cursor < end && (*cursor == ' ' || *cursor == '\t')) {
            ++cursor;
        }
        while (end > cursor && (end[-1] == ' ' || end[-1] == '\t')) {
            --end;
        }
        if ((size_t)(end - cursor) == wanted_length &&
            memcmp(cursor, wanted, wanted_length) == 0) {
            return true;
        }
        cursor = comma ? comma + 1 : NULL;
    }
    return false;
}

void printer_advertisement_build(const printer_target_t *target,
                                 const char *bridge_uuid,
                                 const char *custom_name,
                                 printer_advertisement_t *advertisement)
{
    memset(advertisement, 0, sizeof(*advertisement));
    const char *label = target->label[0] ? target->label : target->instance;
    if (!label[0]) {
        label = "Legacy AirPrint printer";
    }
    const char *identity_suffix = bridge_uuid ? bridge_uuid : "";
    size_t uuid_length = strlen(identity_suffix);
    if (uuid_length > 4) {
        identity_suffix += uuid_length - 4;
    }
    if (custom_name && custom_name[0]) {
        snprintf(advertisement->instance, sizeof(advertisement->instance),
                 "%s", custom_name);
    } else if (*identity_suffix) {
        snprintf(advertisement->instance, sizeof(advertisement->instance),
                 "ESPresso - %.45s (%.4s)", label, identity_suffix);
    } else {
        snprintf(advertisement->instance, sizeof(advertisement->instance),
                 "ESPresso - %.51s", label);
    }
    snprintf(advertisement->make_model, sizeof(advertisement->make_model), "%s",
             label);
    snprintf(advertisement->product, sizeof(advertisement->product), "(%s)",
             label);
    snprintf(advertisement->pdl, sizeof(advertisement->pdl), "%.251s",
             target->pdl);
    if (csv_contains(target->pdl, "image/urf") &&
        !csv_contains(target->pdl, "image/pwg-raster")) {
        size_t length = strlen(advertisement->pdl);
        static const char suffix[] = ",image/pwg-raster";
        if (length + sizeof(suffix) <= sizeof(advertisement->pdl)) {
            memcpy(advertisement->pdl + length, suffix, sizeof(suffix));
        }
    }
    snprintf(advertisement->urf, sizeof(advertisement->urf), "%s", target->urf);
    snprintf(advertisement->uuid, sizeof(advertisement->uuid), "%s",
             bridge_uuid ? bridge_uuid : "");
    snprintf(advertisement->note, sizeof(advertisement->note), "%s",
             target->location[0] ? target->location :
                                   "Legacy printer bridged by ESPresso");
    advertisement->color[0] = target->color ? 'T' : 'F';
    advertisement->duplex[0] = target->duplex ? 'T' : 'F';
    advertisement->copies[0] = target->copies ? 'T' : 'F';
    advertisement->collate[0] = target->collate ? 'T' : 'F';
    advertisement->color[1] = '\0';
    advertisement->duplex[1] = '\0';
    advertisement->copies[1] = '\0';
    advertisement->collate[1] = '\0';
    const uint64_t required_operations =
        (1ULL << 2) | (1ULL << 4) | (1ULL << 5) | (1ULL << 6) |
        (1ULL << 8) | (1ULL << 9) | (1ULL << 10) | (1ULL << 11) |
        (1ULL << 57) | (1ULL << 59) | (1ULL << 60);
    bool has_page_description = csv_contains(target->pdl, "application/pdf") ||
                                csv_contains(target->pdl,
                                             "application/openxps");
    advertisement->ipp_everywhere =
        target->ipp_everywhere && has_page_description &&
        csv_contains(target->pdl, "image/urf") &&
        (!target->color || csv_contains(target->pdl, "image/jpeg")) &&
        (target->operations_supported & required_operations) ==
            required_operations &&
        target->page_ranges_supported && target->overrides_document_number &&
        target->overrides_pages;
    snprintf(advertisement->printer_state,
             sizeof(advertisement->printer_state), "%u",
             target->printer_state ? target->printer_state : 3);
}

size_t printer_advertisement_txt(const printer_advertisement_t *advertisement,
                                 printer_txt_item_t *items, size_t capacity)
{
    if (!advertisement || !items || capacity < ESPRESSO_DNSSD_TXT_BASE) {
        return 0;
    }
    const printer_txt_item_t values[ESPRESSO_DNSSD_TXT_BASE] = {
        {"txtvers", "1"},
        {"qtotal", "1"},
        {"rp", "ipp/print"},
        {"ty", advertisement->make_model},
        {"product", advertisement->product},
        {"pdl", advertisement->pdl},
        {"URF", advertisement->urf},
        {"Color", advertisement->color},
        {"Duplex", advertisement->duplex},
        {"Copies", advertisement->copies},
        {"Collate", advertisement->collate},
        {"Transparent", "T"},
        {"Binary", "T"},
        {"printer-state", advertisement->printer_state},
        {"kind", "document"},
        {"priority", "0"},
        {"adminurl", "http://espresso.local/"},
        {"UUID", advertisement->uuid},
        {"note", advertisement->note},
    };
    memcpy(items, values, sizeof(values));
    return ESPRESSO_DNSSD_TXT_BASE;
}

size_t printer_advertisement_ipps_txt(
    const printer_advertisement_t *advertisement, printer_txt_item_t *items,
    size_t capacity)
{
    if (capacity < ESPRESSO_DNSSD_TXT_MAX) {
        return 0;
    }
    size_t count = printer_advertisement_txt(advertisement, items, capacity);
    if (count != ESPRESSO_DNSSD_TXT_BASE) {
        return 0;
    }
    items[count++] = (printer_txt_item_t){"TLS", "1.2"};
    return count;
}
