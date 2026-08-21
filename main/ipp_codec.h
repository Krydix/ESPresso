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

typedef enum {
    IPP_RESPONSE_KIND_PRINTER = 0,
    IPP_RESPONSE_KIND_JOB,
} ipp_response_kind_t;

enum {
    IPP_OPERATION_PRINT_JOB = 0x0002,
    IPP_OPERATION_VALIDATE_JOB = 0x0004,
    IPP_OPERATION_CREATE_JOB = 0x0005,
    IPP_OPERATION_SEND_DOCUMENT = 0x0006,
    IPP_OPERATION_CANCEL_JOB = 0x0008,
    IPP_OPERATION_GET_JOB_ATTRIBUTES = 0x0009,
    IPP_OPERATION_GET_JOBS = 0x000a,
    IPP_OPERATION_GET_PRINTER_ATTRIBUTES = 0x000b,
};

enum {
    IPP_STATUS_SUCCESSFUL_OK = 0x0000,
    IPP_STATUS_CLIENT_ERROR_BAD_REQUEST = 0x0400,
    IPP_STATUS_CLIENT_ERROR_DOCUMENT_FORMAT_NOT_SUPPORTED = 0x040a,
    IPP_STATUS_CLIENT_ERROR_CHARSET_NOT_SUPPORTED = 0x040d,
    IPP_STATUS_SERVER_ERROR_OPERATION_NOT_SUPPORTED = 0x0501,
    IPP_STATUS_SERVER_ERROR_SERVICE_UNAVAILABLE = 0x0502,
    IPP_STATUS_SERVER_ERROR_VERSION_NOT_SUPPORTED = 0x0503,
};

typedef struct {
    uint8_t major;
    uint8_t minor;
    uint16_t operation_id;
    uint32_t request_id;
    size_t attributes_length;
    bool has_document;
    bool has_attributes_charset;
    bool has_natural_language;
    bool has_target_uri;
    bool operation_attributes_valid;
    char attributes_charset[32];
    char document_format[64];
    char requested_attributes[512];
} ipp_request_info_t;

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

/* Add document-format to a capability query for format-specific constraints. */
ipp_codec_result_t ipp_codec_build_get_printer_attributes_for_format(
    uint8_t major,
    uint8_t minor,
    uint32_t request_id,
    const char *printer_uri,
    bool include_media_col_database,
    const char *document_format,
    uint8_t **output,
    size_t *output_length);

/* Inspect a complete IPP attribute envelope without copying document data. */
ipp_codec_result_t ipp_codec_inspect_request(
    const uint8_t *input,
    size_t input_length,
    ipp_request_info_t *info);

/* Build a standards-shaped IPP error/success response with a status message. */
ipp_codec_result_t ipp_codec_build_status_response(
    uint8_t major,
    uint8_t minor,
    uint16_t status_code,
    uint32_t request_id,
    const char *status_message,
    uint8_t **output,
    size_t *output_length);

/* Operations that ESPresso can safely expose when the upstream supports them. */
uint64_t ipp_codec_relay_operations(uint64_t upstream_operations);

bool ipp_codec_format_supported(const printer_target_t *target,
                                const char *document_format);

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

/*
 * Honor requested-attributes for the selected response object. Operation and
 * unsupported-attributes groups are preserved. This is shared by printer
 * filtering now and future job response filtering. Before filtering Get-Jobs,
 * callers must expand its omitted-selector default to "job-uri,job-id"; an
 * omitted selector passed here means the normal "all" default.
 */
ipp_codec_result_t ipp_codec_filter_response(
    const uint8_t *input,
    size_t input_length,
    ipp_response_kind_t response_kind,
    const char *requested_attributes,
    uint8_t **output,
    size_t *output_length,
    size_t *attributes_length);

/* Compatibility wrapper for Get-Printer-Attributes response filtering. */
ipp_codec_result_t ipp_codec_filter_printer_response(
    const uint8_t *input,
    size_t input_length,
    const char *requested_attributes,
    uint8_t **output,
    size_t *output_length,
    size_t *attributes_length);

uint16_t ipp_codec_message_code(const uint8_t *input, size_t input_length);

/* Derive inexpensive secondary fields from DNS-SD/IPP strings such as URF RS. */
void ipp_codec_finalize_profile(printer_target_t *target);
