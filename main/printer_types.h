#pragma once

#include <stdbool.h>
#include <stdint.h>

#define ESPRESSO_PROFILE_SCHEMA 3
#define ESPRESSO_INSTANCE_MAX 64
#define ESPRESSO_HOST_MAX 64
#define ESPRESSO_ADDRESS_MAX 48
#define ESPRESSO_PATH_MAX 128
#define ESPRESSO_PDL_MAX 256
#define ESPRESSO_URF_MAX 160
#define ESPRESSO_MEDIA_MAX 384
#define ESPRESSO_MEDIA_NAME_MAX 64
#define ESPRESSO_LABEL_MAX 96
#define ESPRESSO_LOCATION_MAX 96
#define ESPRESSO_UUID_MAX 64
#define ESPRESSO_IPP_VERSIONS_MAX 32
#define ESPRESSO_COLOR_MODE_MAX 16
#define ESPRESSO_STATE_REASONS_MAX 192

/*
 * Compact, persistable result of the same DNS-SD + Get-Printer-Attributes
 * discovery sequence used by CUPS. It intentionally contains only metadata
 * that an ESP can relay without rendering or converting a document.
 */
typedef struct {
    uint32_t profile_schema;
    char instance[ESPRESSO_INSTANCE_MAX];
    char hostname[ESPRESSO_HOST_MAX];
    char address[ESPRESSO_ADDRESS_MAX];
    uint16_t port;
    char resource_path[ESPRESSO_PATH_MAX];
    char pdl[ESPRESSO_PDL_MAX];
    char urf[ESPRESSO_URF_MAX];
    char media[ESPRESSO_MEDIA_MAX];
    char media_default[ESPRESSO_MEDIA_NAME_MAX];
    char label[ESPRESSO_LABEL_MAX];
    char location[ESPRESSO_LOCATION_MAX];
    char uuid[ESPRESSO_UUID_MAX];
    char ipp_versions[ESPRESSO_IPP_VERSIONS_MAX];
    char color_mode_default[ESPRESSO_COLOR_MODE_MAX];
    char state_reasons[ESPRESSO_STATE_REASONS_MAX];
    uint64_t operations_supported;
    uint16_t copies_upper;
    uint16_t resolution_low_dpi;
    uint16_t resolution_high_dpi;
    uint8_t upstream_ipp_major;
    uint8_t upstream_ipp_minor;
    uint8_t printer_state;
    bool capability_queried;
    bool accepting_jobs;
    bool accepting_jobs_known;
    bool color;
    bool duplex;
    bool copies;
    bool collate;
} printer_target_t;
