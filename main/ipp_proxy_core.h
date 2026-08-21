#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ipp_codec.h"
#include "printer_types.h"

typedef enum {
    IPP_PROXY_RELAY = 0,
    IPP_PROXY_LOCAL_STATUS,
} ipp_proxy_action_t;

typedef struct {
    ipp_proxy_action_t action;
    bool document_operation;
    uint8_t response_major;
    uint8_t response_minor;
    uint8_t upstream_major;
    uint8_t upstream_minor;
    uint16_t status_code;
    const char *status_message;
} ipp_proxy_plan_t;

typedef enum {
    IPP_PROXY_FAULT_UPSTREAM_TIMEOUT = 0,
    IPP_PROXY_FAULT_UPSTREAM_DISCONNECT,
    IPP_PROXY_FAULT_RESPONSE_TOO_LARGE,
    IPP_PROXY_FAULT_INVALID_RESPONSE,
} ipp_proxy_fault_t;

typedef struct {
    const char *http_status;
    const char *message;
    bool retry_allowed;
} ipp_proxy_fault_policy_t;

/*
 * Pure, transport-independent policy used by both the ESP HTTP handler and the
 * host CUPS compatibility lab. `content_length` includes the IPP attribute
 * envelope and any following document bytes.
 */
void ipp_proxy_plan_request(const ipp_request_info_t *request,
                            const printer_target_t *target,
                            size_t content_length,
                            ipp_proxy_plan_t *plan);

/* Stable gateway behavior. Non-idempotent document delivery is never retried. */
void ipp_proxy_plan_fault(ipp_proxy_fault_t fault, bool document_operation,
                          ipp_proxy_fault_policy_t *policy);
