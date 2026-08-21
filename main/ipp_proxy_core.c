#include "ipp_proxy_core.h"

#include <strings.h>
#include <string.h>

static void reject(ipp_proxy_plan_t *plan, uint16_t status, const char *message)
{
    plan->action = IPP_PROXY_LOCAL_STATUS;
    plan->status_code = status;
    plan->status_message = message;
}

void ipp_proxy_plan_request(const ipp_request_info_t *request,
                            const printer_target_t *target,
                            size_t content_length,
                            ipp_proxy_plan_t *plan)
{
    memset(plan, 0, sizeof(*plan));
    plan->action = IPP_PROXY_RELAY;
    plan->response_major = request->major;
    plan->response_minor = request->minor;
    plan->upstream_major = target->capability_queried &&
                                   target->upstream_ipp_major >= 2
                               ? target->upstream_ipp_major
                               : 1;
    plan->upstream_minor = plan->upstream_major >= 2
                               ? target->upstream_ipp_minor
                               : 1;
    plan->document_operation =
        request->operation_id == IPP_OPERATION_PRINT_JOB ||
        request->operation_id == IPP_OPERATION_SEND_DOCUMENT;

    if (!((request->major == 1 && request->minor == 1) ||
          (request->major == 2 && request->minor == 0))) {
        plan->response_major = 2;
        plan->response_minor = 0;
        reject(plan, IPP_STATUS_SERVER_ERROR_VERSION_NOT_SUPPORTED,
               "ESPresso supports IPP/1.1 and IPP/2.0");
        return;
    }
    if (request->request_id == 0 || !request->operation_attributes_valid ||
        !request->has_attributes_charset || !request->has_natural_language ||
        !request->has_target_uri) {
        reject(plan, IPP_STATUS_CLIENT_ERROR_BAD_REQUEST,
               "Missing required IPP operation attribute");
        return;
    }
    if (strcasecmp(request->attributes_charset, "utf-8") != 0) {
        reject(plan, IPP_STATUS_CLIENT_ERROR_CHARSET_NOT_SUPPORTED,
               "Only utf-8 IPP attributes are supported");
        return;
    }

    uint64_t operations = ipp_codec_relay_operations(
        target->operations_supported);
    if (request->operation_id >= 64 ||
        (operations & (1ULL << request->operation_id)) == 0) {
        reject(plan, IPP_STATUS_SERVER_ERROR_OPERATION_NOT_SUPPORTED,
               "Operation is not supported by the selected printer");
        return;
    }
    if (request->attributes_length > content_length) {
        reject(plan, IPP_STATUS_CLIENT_ERROR_BAD_REQUEST,
               "IPP attributes exceed the request body");
        return;
    }
    if (request->operation_id == IPP_OPERATION_PRINT_JOB &&
        content_length == request->attributes_length) {
        reject(plan, IPP_STATUS_CLIENT_ERROR_BAD_REQUEST,
               "Print-Job requires document data");
        return;
    }
    if (!plan->document_operation &&
        content_length != request->attributes_length) {
        reject(plan, IPP_STATUS_CLIENT_ERROR_BAD_REQUEST,
               "This operation cannot contain document data");
        return;
    }
    bool format_must_be_relayable =
        request->operation_id == IPP_OPERATION_PRINT_JOB ||
        request->operation_id == IPP_OPERATION_VALIDATE_JOB ||
        request->operation_id == IPP_OPERATION_CREATE_JOB ||
        request->operation_id == IPP_OPERATION_SEND_DOCUMENT;
    if (format_must_be_relayable && request->document_format[0] &&
        !ipp_codec_format_supported(target, request->document_format)) {
        reject(plan, IPP_STATUS_CLIENT_ERROR_DOCUMENT_FORMAT_NOT_SUPPORTED,
               "Document format is not accepted unchanged by the selected printer");
    }
}
