#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "printer_types.h"

typedef enum {
    IPP_CODEC_OK = 0,
    IPP_CODEC_INCOMPLETE,
    IPP_CODEC_MALFORMED,
    IPP_CODEC_NO_MEMORY,
} ipp_codec_result_t;

/*
 * Rewrites URI-valued IPP attributes and copies any document bytes following
 * end-of-attributes. The caller owns *output and must free it.
 */
ipp_codec_result_t ipp_codec_rewrite(
    const uint8_t *input,
    size_t input_length,
    const char *printer_uri,
    const char *uri_authority,
    uint8_t **output,
    size_t *output_length,
    size_t *attributes_length);

/* Build the small CUPS-style Get-Printer-Attributes probe request. */
ipp_codec_result_t ipp_codec_build_get_printer_attributes(
    uint8_t major,
    uint8_t minor,
    uint32_t request_id,
    const char *printer_uri,
    bool include_media_col_database,
    uint8_t **output,
    size_t *output_length);

/* Merge relayed capabilities from an IPP response into a compact profile. */
ipp_codec_result_t ipp_codec_apply_printer_attributes(
    const uint8_t *input,
    size_t input_length,
    printer_target_t *target);

/*
 * Rewrite the legacy response and synthesize only metadata that ESPresso can
 * truthfully provide itself. No media data or document payload is converted.
 */
ipp_codec_result_t ipp_codec_normalize_printer_response(
    const uint8_t *input,
    size_t input_length,
    const char *printer_uri,
    const char *uri_authority,
    const char *local_uuid,
    const printer_target_t *target,
    uint8_t **output,
    size_t *output_length,
    size_t *attributes_length);

uint16_t ipp_codec_message_code(const uint8_t *input, size_t input_length);

/* Derive inexpensive secondary fields from DNS-SD/IPP strings such as URF RS. */
void ipp_codec_finalize_profile(printer_target_t *target);
