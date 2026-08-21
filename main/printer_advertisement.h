#pragma once

#include <stddef.h>

#include "printer_types.h"

#define ESPRESSO_DNSSD_SERVICE "_ipp"
#define ESPRESSO_DNSSD_PROTOCOL "_tcp"
#define ESPRESSO_DNSSD_SUBTYPE "_universal"
#define ESPRESSO_DNSSD_TXT_MAX 19

typedef struct {
    const char *key;
    const char *value;
} printer_txt_item_t;

typedef struct {
    char instance[64];
    char make_model[ESPRESSO_LABEL_MAX];
    char product[ESPRESSO_LABEL_MAX + 3];
    char pdl[252];
    char urf[ESPRESSO_URF_MAX];
    char uuid[ESPRESSO_UUID_MAX];
    char note[ESPRESSO_LOCATION_MAX];
    char color[2];
    char duplex[2];
    char copies[2];
    char collate[2];
    char printer_state[4];
} printer_advertisement_t;

/* Build the exact identity and TXT values published by printer_discovery.c. */
void printer_advertisement_build(const printer_target_t *target,
                                 const char *bridge_uuid,
                                 const char *custom_name,
                                 printer_advertisement_t *advertisement);

size_t printer_advertisement_txt(const printer_advertisement_t *advertisement,
                                 printer_txt_item_t *items, size_t capacity);
