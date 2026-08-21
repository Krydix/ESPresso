#include "printer_advertisement.h"

#include <stdio.h>
#include <string.h>

void printer_advertisement_build(const printer_target_t *target,
                                 const char *bridge_uuid,
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
    if (*identity_suffix) {
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
    snprintf(advertisement->printer_state,
             sizeof(advertisement->printer_state), "%u",
             target->printer_state ? target->printer_state : 3);
}

size_t printer_advertisement_txt(const printer_advertisement_t *advertisement,
                                 printer_txt_item_t *items, size_t capacity)
{
    if (!advertisement || !items || capacity < ESPRESSO_DNSSD_TXT_MAX) {
        return 0;
    }
    const printer_txt_item_t values[ESPRESSO_DNSSD_TXT_MAX] = {
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
    return ESPRESSO_DNSSD_TXT_MAX;
}
