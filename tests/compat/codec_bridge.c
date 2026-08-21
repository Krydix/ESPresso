#include "ipp_codec.h"
#include "ipp_proxy_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    printer_target_t target;
} espresso_bridge_t;

espresso_bridge_t *espresso_bridge_new(const char *label, const char *dns_pdl,
                                       const char *dns_urf,
                                       const char *dns_media)
{
    espresso_bridge_t *bridge = calloc(1, sizeof(*bridge));
    if (!bridge) {
        return NULL;
    }
    bridge->target.profile_schema = ESPRESSO_PROFILE_SCHEMA;
    bridge->target.port = 18632;
    snprintf(bridge->target.instance, sizeof(bridge->target.instance), "%s",
             label ? label : "Legacy fixture");
    snprintf(bridge->target.label, sizeof(bridge->target.label), "%s",
             label ? label : "Legacy fixture");
    snprintf(bridge->target.address, sizeof(bridge->target.address), "127.0.0.1");
    snprintf(bridge->target.resource_path,
             sizeof(bridge->target.resource_path), "/ipp/print");
    snprintf(bridge->target.pdl, sizeof(bridge->target.pdl), "%s",
             dns_pdl ? dns_pdl : "");
    snprintf(bridge->target.urf, sizeof(bridge->target.urf), "%s",
             dns_urf ? dns_urf : "");
    snprintf(bridge->target.media, sizeof(bridge->target.media), "%s",
             dns_media ? dns_media : "");
    ipp_codec_finalize_profile(&bridge->target);
    return bridge;
}

void espresso_bridge_delete(espresso_bridge_t *bridge)
{
    free(bridge);
}

void espresso_bridge_free(void *buffer)
{
    free(buffer);
}

int espresso_bridge_apply(espresso_bridge_t *bridge, const uint8_t *message,
                          size_t message_length)
{
    return bridge && ipp_codec_apply_printer_attributes(
                         message, message_length, &bridge->target) == IPP_CODEC_OK;
}

int espresso_bridge_query(uint8_t major, uint8_t minor, uint32_t request_id,
                          const char *format, uint8_t **output,
                          size_t *output_length)
{
    return ipp_codec_build_get_printer_attributes_for_format(
               major, minor, request_id,
               "ipp://127.0.0.1:18632/ipp/print", major >= 2,
               format && *format ? format : NULL, output, output_length) ==
           IPP_CODEC_OK;
}

int espresso_bridge_inspect(const uint8_t *message, size_t message_length,
                            ipp_request_info_t *info)
{
    return ipp_codec_inspect_request(message, message_length, info) ==
           IPP_CODEC_OK;
}

int espresso_bridge_rewrite(const uint8_t *message, size_t message_length,
                            const char *printer_uri, const char *authority,
                            uint8_t **output, size_t *output_length,
                            size_t *attributes_length)
{
    return ipp_codec_rewrite(message, message_length, printer_uri, authority,
                             output, output_length, attributes_length) ==
           IPP_CODEC_OK;
}

int espresso_bridge_normalize(espresso_bridge_t *bridge,
                              const uint8_t *message, size_t message_length,
                              const char *requested_attributes,
                              uint8_t **output, size_t *output_length,
                              size_t *attributes_length)
{
    if (!bridge) {
        return 0;
    }
    uint8_t *normalized = NULL;
    size_t normalized_length = 0;
    size_t normalized_attributes = 0;
    if (ipp_codec_normalize_printer_response(
               message, message_length,
               "ipp://127.0.0.1:18631/ipp/print",
               "ipp://127.0.0.1:18631", "compat-lab-bridge",
               &bridge->target, &normalized, &normalized_length,
               &normalized_attributes) != IPP_CODEC_OK) {
        return 0;
    }
    ipp_codec_result_t result = ipp_codec_filter_printer_response(
        normalized, normalized_length, requested_attributes, output,
        output_length, attributes_length);
    free(normalized);
    return result == IPP_CODEC_OK;
}

int espresso_bridge_status(uint8_t major, uint8_t minor, uint16_t status,
                           uint32_t request_id, const char *message,
                           uint8_t **output, size_t *output_length)
{
    return ipp_codec_build_status_response(major, minor, status, request_id,
                                           message, output, output_length) ==
           IPP_CODEC_OK;
}

uint64_t espresso_bridge_operations(const espresso_bridge_t *bridge)
{
    return bridge ? ipp_codec_relay_operations(
                        bridge->target.operations_supported) : 0;
}

int espresso_bridge_format_supported(const espresso_bridge_t *bridge,
                                     const char *format)
{
    return bridge && ipp_codec_format_supported(&bridge->target, format);
}

uint8_t espresso_bridge_upstream_major(const espresso_bridge_t *bridge)
{
    return bridge ? bridge->target.upstream_ipp_major : 1;
}

uint8_t espresso_bridge_upstream_minor(const espresso_bridge_t *bridge)
{
    return bridge ? bridge->target.upstream_ipp_minor : 1;
}

uint16_t espresso_bridge_plan(const espresso_bridge_t *bridge,
                              const ipp_request_info_t *request,
                              size_t content_length, uint8_t *response_major,
                              uint8_t *response_minor, uint8_t *upstream_major,
                              uint8_t *upstream_minor, int *document_operation)
{
    if (!bridge || !request) {
        return IPP_STATUS_CLIENT_ERROR_BAD_REQUEST;
    }
    ipp_proxy_plan_t plan;
    ipp_proxy_plan_request(request, &bridge->target, content_length, &plan);
    if (response_major) {
        *response_major = plan.response_major;
    }
    if (response_minor) {
        *response_minor = plan.response_minor;
    }
    if (upstream_major) {
        *upstream_major = plan.upstream_major;
    }
    if (upstream_minor) {
        *upstream_minor = plan.upstream_minor;
    }
    if (document_operation) {
        *document_operation = plan.document_operation;
    }
    return plan.action == IPP_PROXY_RELAY ? IPP_STATUS_SUCCESSFUL_OK :
                                            plan.status_code;
}
