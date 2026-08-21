#include "ipp_proxy_core.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static ipp_request_info_t valid_request(uint16_t operation)
{
    ipp_request_info_t request = {0};
    request.major = 2;
    request.minor = 0;
    request.operation_id = operation;
    request.request_id = 17;
    request.attributes_length = 128;
    request.has_attributes_charset = true;
    request.has_natural_language = true;
    request.has_target_uri = true;
    request.operation_attributes_valid = true;
    snprintf(request.attributes_charset, sizeof(request.attributes_charset),
             "utf-8");
    return request;
}

static printer_target_t relay_target(void)
{
    printer_target_t target = {0};
    target.capability_queried = true;
    target.upstream_ipp_major = 1;
    target.upstream_ipp_minor = 1;
    target.operations_supported =
        (1ULL << IPP_OPERATION_PRINT_JOB) |
        (1ULL << IPP_OPERATION_PRINT_URI) |
        (1ULL << IPP_OPERATION_VALIDATE_JOB) |
        (1ULL << IPP_OPERATION_CREATE_JOB) |
        (1ULL << IPP_OPERATION_SEND_DOCUMENT) |
        (1ULL << IPP_OPERATION_CANCEL_JOB) |
        (1ULL << IPP_OPERATION_GET_JOB_ATTRIBUTES) |
        (1ULL << IPP_OPERATION_GET_JOBS) |
        (1ULL << IPP_OPERATION_GET_PRINTER_ATTRIBUTES);
    snprintf(target.pdl, sizeof(target.pdl), "image/urf,application/pdf");
    return target;
}

static void assert_status(ipp_request_info_t request, printer_target_t target,
                          size_t length, uint16_t expected)
{
    ipp_proxy_plan_t plan;
    ipp_proxy_plan_request(&request, &target, length, &plan);
    assert(plan.action == IPP_PROXY_LOCAL_STATUS);
    assert(plan.status_code == expected);
    assert(plan.status_message && plan.status_message[0]);
}

static void test_accepts_safe_relay_and_selects_upstream_version(void)
{
    printer_target_t target = relay_target();
    ipp_request_info_t request = valid_request(IPP_OPERATION_PRINT_JOB);
    snprintf(request.document_format, sizeof(request.document_format), "image/urf");
    ipp_proxy_plan_t plan;
    ipp_proxy_plan_request(&request, &target, request.attributes_length + 4096,
                           &plan);
    assert(plan.action == IPP_PROXY_RELAY);
    assert(plan.document_operation);
    assert(plan.upstream_major == 1 && plan.upstream_minor == 1);
    assert(plan.response_major == 2 && plan.response_minor == 0);

    target.upstream_ipp_major = 2;
    target.upstream_ipp_minor = 0;
    ipp_proxy_plan_request(&request, &target, request.attributes_length + 1,
                           &plan);
    assert(plan.upstream_major == 2 && plan.upstream_minor == 0);
}

static void test_rejects_versions_and_required_attribute_errors(void)
{
    printer_target_t target = relay_target();
    ipp_request_info_t request = valid_request(
        IPP_OPERATION_GET_PRINTER_ATTRIBUTES);
    request.major = 2;
    request.minor = 1;
    ipp_proxy_plan_t plan;
    ipp_proxy_plan_request(&request, &target, request.attributes_length, &plan);
    assert(plan.status_code == IPP_STATUS_SERVER_ERROR_VERSION_NOT_SUPPORTED);
    assert(plan.response_major == 2 && plan.response_minor == 0);

    request = valid_request(IPP_OPERATION_GET_PRINTER_ATTRIBUTES);
    request.request_id = 0;
    assert_status(request, target, request.attributes_length,
                  IPP_STATUS_CLIENT_ERROR_BAD_REQUEST);
    request = valid_request(IPP_OPERATION_GET_PRINTER_ATTRIBUTES);
    request.operation_attributes_valid = false;
    assert_status(request, target, request.attributes_length,
                  IPP_STATUS_CLIENT_ERROR_BAD_REQUEST);
    request = valid_request(IPP_OPERATION_GET_PRINTER_ATTRIBUTES);
    request.has_target_uri = false;
    assert_status(request, target, request.attributes_length,
                  IPP_STATUS_CLIENT_ERROR_BAD_REQUEST);
    request = valid_request(IPP_OPERATION_GET_PRINTER_ATTRIBUTES);
    snprintf(request.attributes_charset, sizeof(request.attributes_charset),
             "us-ascii");
    assert_status(request, target, request.attributes_length,
                  IPP_STATUS_CLIENT_ERROR_CHARSET_NOT_SUPPORTED);
    assert(IPP_STATUS_CLIENT_ERROR_CHARSET_NOT_SUPPORTED == 0x040d);
}

static void test_rejects_unsafe_operations_and_payload_shapes(void)
{
    printer_target_t target = relay_target();
    ipp_request_info_t request = valid_request(IPP_OPERATION_PRINT_URI);
    ipp_proxy_plan_t plan;
    ipp_proxy_plan_request(&request, &target, request.attributes_length, &plan);
    assert(plan.action == IPP_PROXY_RELAY);
    assert(!plan.document_operation);

    request = valid_request(IPP_OPERATION_PRINT_JOB);
    assert_status(request, target, request.attributes_length,
                  IPP_STATUS_CLIENT_ERROR_BAD_REQUEST);

    request = valid_request(IPP_OPERATION_VALIDATE_JOB);
    assert_status(request, target, request.attributes_length + 1,
                  IPP_STATUS_CLIENT_ERROR_BAD_REQUEST);

    request = valid_request(IPP_OPERATION_GET_PRINTER_ATTRIBUTES);
    request.attributes_length = 129;
    assert_status(request, target, 128, IPP_STATUS_CLIENT_ERROR_BAD_REQUEST);
}

static void test_rejects_unconvertible_formats_and_atomic_job_pairs(void)
{
    printer_target_t target = relay_target();
    ipp_request_info_t request = valid_request(IPP_OPERATION_VALIDATE_JOB);
    snprintf(request.document_format, sizeof(request.document_format),
             "image/pwg-raster");
    ipp_proxy_plan_t plan;
    ipp_proxy_plan_request(&request, &target, request.attributes_length, &plan);
    assert(plan.action == IPP_PROXY_RELAY);
    assert(plan.document_transform == IPP_PROXY_DOCUMENT_PWG_TO_URF);
    assert(strcmp(plan.upstream_document_format, "image/urf") == 0);

    snprintf(request.document_format, sizeof(request.document_format),
             "image/jpeg");
    assert_status(request, target, request.attributes_length,
                  IPP_STATUS_CLIENT_ERROR_DOCUMENT_FORMAT_NOT_SUPPORTED);

    request = valid_request(IPP_OPERATION_GET_PRINTER_ATTRIBUTES);
    snprintf(request.document_format, sizeof(request.document_format),
             "application/octet-stream");
    ipp_proxy_plan_request(&request, &target, request.attributes_length, &plan);
    assert(plan.action == IPP_PROXY_RELAY);

    target.operations_supported &= ~(1ULL << IPP_OPERATION_SEND_DOCUMENT);
    request = valid_request(IPP_OPERATION_CREATE_JOB);
    assert_status(request, target, request.attributes_length,
                  IPP_STATUS_SERVER_ERROR_OPERATION_NOT_SUPPORTED);
}

static void test_transport_faults_are_stable_and_never_retry_jobs(void)
{
    ipp_proxy_fault_policy_t policy;
    ipp_proxy_plan_fault(IPP_PROXY_FAULT_UPSTREAM_TIMEOUT, true, &policy);
    assert(strcmp(policy.http_status, "504 Gateway Timeout") == 0);
    assert(strcmp(policy.message, "Legacy printer timed out") == 0);
    assert(!policy.retry_allowed);

    ipp_proxy_plan_fault(IPP_PROXY_FAULT_RESPONSE_TOO_LARGE, false, &policy);
    assert(strcmp(policy.http_status, "502 Bad Gateway") == 0);
    assert(strstr(policy.message, "safety limit") != NULL);
    assert(!policy.retry_allowed);

    ipp_proxy_plan_fault(IPP_PROXY_FAULT_UPSTREAM_DISCONNECT, true, &policy);
    assert(strcmp(policy.http_status, "502 Bad Gateway") == 0);
    assert(strstr(policy.message, "disconnected") != NULL);
    assert(!policy.retry_allowed);

    ipp_proxy_plan_fault(IPP_PROXY_FAULT_UPSTREAM_TIMEOUT, false, &policy);
    assert(policy.retry_allowed);
}

int main(void)
{
    test_accepts_safe_relay_and_selects_upstream_version();
    test_rejects_versions_and_required_attribute_errors();
    test_rejects_unsafe_operations_and_payload_shapes();
    test_rejects_unconvertible_formats_and_atomic_job_pairs();
    test_transport_faults_are_stable_and_never_retry_jobs();
    puts("IPP proxy core tests passed");
    return 0;
}
