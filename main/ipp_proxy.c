#include "ipp_proxy.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <strings.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "app_state.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "ipp_codec.h"
#include "ipp_http_request.h"
#include "ipp_proxy_core.h"
#include "ipp_stream.h"
#include "job_history.h"
#include "lwip/inet.h"
#include "printer_discovery.h"
#include "printer_identity.h"
#include "printer_transport.h"
#include "raster_converter.h"
#include "tls_identity.h"

#define REQUEST_PREFIX_MAX (64 * 1024)
#define RESPONSE_MAX (128 * 1024)
#define STREAM_CHUNK 4096
#define RECEIVE_TIMEOUT_RETRIES 3
#define IPP_SERVER_PORT 631
#define IPPS_SERVER_PORT 8631
#define IPP_SERVER_BACKLOG 12
#define IPP_SERVER_WORKERS 2
#define IPP_SERVER_TASK_STACK 4096
#define IPP_WORKER_TASK_STACK 16384

static const char *TAG = "espresso_proxy";
static SemaphoreHandle_t worker_slots;
static SemaphoreHandle_t job_history_lock;
static espresso_job_history_t job_history;

typedef struct {
    int socket_fd;
    ipp_http_request_t *request;
    espresso_tls_connection_t *tls;
    char peer[64];
} ipp_connection_t;

typedef struct {
    int socket_fd;
    bool secure;
} ipp_worker_argument_t;

typedef struct {
    int plain_socket_fd;
    int secure_socket_fd;
} ipp_listener_t;

static uint64_t monotonic_ms(void)
{
    return (uint64_t)esp_timer_get_time() / 1000ULL;
}

static uint32_t monitor_request_start(const ipp_request_info_t *info)
{
    if (!job_history_lock) {
        return 0;
    }
    xSemaphoreTake(job_history_lock, portMAX_DELAY);
    uint32_t id = 0;
    if (info->operation_id == IPP_OPERATION_PRINT_JOB) {
        id = espresso_job_history_begin(&job_history, info->job_name,
                                        info->document_format,
                                        ESPRESSO_JOB_SENDING, monotonic_ms());
    } else if (info->operation_id == IPP_OPERATION_CREATE_JOB) {
        id = espresso_job_history_begin(&job_history, info->job_name,
                                        info->document_format,
                                        ESPRESSO_JOB_QUEUED, monotonic_ms());
    } else if (info->operation_id == IPP_OPERATION_SEND_DOCUMENT) {
        if (info->has_job_id) {
            id = espresso_job_history_find_upstream(&job_history, info->job_id);
        }
        if (!id) {
            id = espresso_job_history_begin(&job_history, info->job_name,
                                            info->document_format,
                                            ESPRESSO_JOB_SENDING, monotonic_ms());
            if (info->has_job_id) {
                espresso_job_history_attach_upstream(&job_history, id,
                                                      info->job_id,
                                                      monotonic_ms());
            }
        } else {
            espresso_job_history_update(&job_history, id, ESPRESSO_JOB_SENDING,
                                        info->document_format, 0,
                                        monotonic_ms());
        }
    } else if (info->operation_id == IPP_OPERATION_CANCEL_JOB &&
               info->has_job_id) {
        id = espresso_job_history_find_upstream(&job_history, info->job_id);
    }
    xSemaphoreGive(job_history_lock);
    return id;
}

static void monitor_update(uint32_t id, espresso_job_state_t state,
                           const char *format, size_t document_bytes)
{
    if (!id || !job_history_lock) {
        return;
    }
    xSemaphoreTake(job_history_lock, portMAX_DELAY);
    espresso_job_history_update(&job_history, id, state, format,
                                document_bytes, monotonic_ms());
    xSemaphoreGive(job_history_lock);
}

static void monitor_submission_failed(uint32_t id,
                                      const ipp_request_info_t *info,
                                      size_t document_bytes)
{
    if (info->operation_id == IPP_OPERATION_PRINT_JOB ||
        info->operation_id == IPP_OPERATION_CREATE_JOB ||
        info->operation_id == IPP_OPERATION_SEND_DOCUMENT) {
        monitor_update(id, ESPRESSO_JOB_FAILED, info->document_format,
                       document_bytes);
    }
}

static void monitor_attach_upstream(uint32_t id, uint32_t upstream_job_id)
{
    if (!id || !job_history_lock) {
        return;
    }
    xSemaphoreTake(job_history_lock, portMAX_DELAY);
    espresso_job_history_attach_upstream(&job_history, id, upstream_job_id,
                                          monotonic_ms());
    xSemaphoreGive(job_history_lock);
}

size_t ipp_proxy_job_snapshot(espresso_job_record_t *records, size_t capacity,
                              uint64_t now_ms)
{
    if (!records || !capacity || !job_history_lock) {
        return 0;
    }
    xSemaphoreTake(job_history_lock, portMAX_DELAY);
    size_t count = espresso_job_history_snapshot(&job_history, records,
                                                 capacity, now_ms);
    xSemaphoreGive(job_history_lock);
    return count;
}

static const char *operation_name(uint16_t operation_id)
{
    switch (operation_id) {
        case IPP_OPERATION_PRINT_JOB:
            return "Print-Job";
        case IPP_OPERATION_PRINT_URI:
            return "Print-URI";
        case IPP_OPERATION_VALIDATE_JOB:
            return "Validate-Job";
        case IPP_OPERATION_CREATE_JOB:
            return "Create-Job";
        case IPP_OPERATION_SEND_DOCUMENT:
            return "Send-Document";
        case IPP_OPERATION_SEND_URI:
            return "Send-URI";
        case IPP_OPERATION_CANCEL_JOB:
            return "Cancel-Job";
        case IPP_OPERATION_GET_JOB_ATTRIBUTES:
            return "Get-Job-Attributes";
        case IPP_OPERATION_GET_JOBS:
            return "Get-Jobs";
        case IPP_OPERATION_GET_PRINTER_ATTRIBUTES:
            return "Get-Printer-Attributes";
        case IPP_OPERATION_CANCEL_MY_JOBS:
            return "Cancel-My-Jobs";
        case IPP_OPERATION_CLOSE_JOB:
            return "Close-Job";
        case IPP_OPERATION_IDENTIFY_PRINTER:
            return "Identify-Printer";
        default:
            return "Unknown";
    }
}

static void format_peer(int socket_fd, char *output, size_t output_size)
{
    struct sockaddr_storage peer;
    socklen_t peer_length = sizeof(peer);
    char address[INET6_ADDRSTRLEN] = "unknown";
    uint16_t port = 0;
    if (getpeername(socket_fd, (struct sockaddr *)&peer, &peer_length) == 0) {
        if (peer.ss_family == AF_INET) {
            const struct sockaddr_in *peer4 = (const struct sockaddr_in *)&peer;
            inet_ntop(AF_INET, &peer4->sin_addr, address, sizeof(address));
            port = ntohs(peer4->sin_port);
        } else if (peer.ss_family == AF_INET6) {
            const struct sockaddr_in6 *peer6 =
                (const struct sockaddr_in6 *)&peer;
            inet_ntop(AF_INET6, &peer6->sin6_addr, address, sizeof(address));
            port = ntohs(peer6->sin6_port);
        }
    }
    snprintf(output, output_size, "%s:%u", address, port);
}

static int socket_write(void *context, const uint8_t *data, size_t length)
{
    ipp_connection_t *connection = context;
    if (connection->tls) {
        return tls_identity_write(connection->tls, data, length);
    }
    ssize_t sent;
    do {
        sent = send(connection->socket_fd, data, length, 0);
    } while (sent < 0 && errno == EINTR);
    return sent > INT_MAX ? INT_MAX : (int)sent;
}

static esp_err_t send_http_response(ipp_connection_t *connection,
                                    const char *status,
                                    const char *content_type,
                                    const uint8_t *body, size_t body_length,
                                    bool no_store)
{
    char headers[320];
    int length = snprintf(
        headers, sizeof(headers),
        "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
        "Connection: close\r\n%s\r\n",
        status, content_type, body_length,
        no_store ? "Cache-Control: no-store\r\n" : "");
    if (length < 0 || (size_t)length >= sizeof(headers)) {
        return ESP_FAIL;
    }
    if (ipp_stream_write_all(socket_write, connection,
                             (const uint8_t *)headers, (size_t)length) !=
        IPP_STREAM_OK) {
        return ESP_FAIL;
    }
    return ipp_stream_write_all(socket_write, connection, body, body_length) ==
                   IPP_STREAM_OK
               ? ESP_OK
               : ESP_FAIL;
}

static esp_err_t send_http_error(ipp_connection_t *connection,
                                 const char *status, const char *message)
{
    return send_http_response(connection, status, "text/plain; charset=utf-8",
                              (const uint8_t *)message, strlen(message), false);
}

static esp_err_t send_gateway_fault(ipp_connection_t *connection,
                                    ipp_proxy_fault_t fault,
                                    bool document_operation)
{
    ipp_proxy_fault_policy_t policy;
    ipp_proxy_plan_fault(fault, document_operation, &policy);
    return send_http_error(connection, policy.http_status, policy.message);
}

static esp_err_t send_ipp_status(ipp_connection_t *connection,
                                 const ipp_request_info_t *info,
                                 uint16_t status, const char *message)
{
    uint8_t *response = NULL;
    size_t response_length = 0;
    ipp_codec_result_t result = ipp_codec_build_status_response(
        info->major, info->minor, status, info->request_id, message,
        &response, &response_length);
    if (result != IPP_CODEC_OK) {
        return send_http_error(connection, "500 Internal Server Error",
                               "Could not build IPP response");
    }
    esp_err_t err = send_http_response(connection, "200 OK", "application/ipp",
                                       response, response_length, false);
    free(response);
    return err;
}

static esp_err_t send_continue_if_requested(ipp_connection_t *connection)
{
    if (!ipp_http_request_expects_continue(connection->request)) {
        return ESP_OK;
    }
    static const char response[] = "HTTP/1.1 100 Continue\r\n\r\n";
    return ipp_stream_write_all(socket_write, connection,
                                (const uint8_t *)response,
                                sizeof(response) - 1) == IPP_STREAM_OK
               ? ESP_OK
               : ESP_FAIL;
}

static int receive_socket(void *context, uint8_t *buffer, size_t length)
{
    ipp_connection_t *connection = context;
    if (connection->tls) {
        return tls_identity_read(connection->tls, buffer, length);
    }
    for (unsigned attempt = 0; attempt < RECEIVE_TIMEOUT_RETRIES; ++attempt) {
        ssize_t received = recv(connection->socket_fd, buffer, length, 0);
        if (received >= 0) {
            return received > INT_MAX ? INT_MAX : (int)received;
        }
        if (errno == EINTR) {
            --attempt;
            continue;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            return -1;
        }
    }
    return -1;
}

static int receive_request(ipp_connection_t *connection, void *buffer,
                           size_t length)
{
    size_t received = 0;
    ipp_http_result_t result = ipp_http_request_read(
        connection->request, buffer, length, &received);
    if (result == IPP_HTTP_DONE) {
        return 0;
    }
    return result == IPP_HTTP_OK && received <= INT_MAX ? (int)received : -1;
}

static esp_err_t receive_more(ipp_connection_t *connection, uint8_t **buffer,
                              size_t *length, size_t *capacity)
{
    if (*length >= REQUEST_PREFIX_MAX) {
        return ESP_ERR_NO_MEM;
    }
    size_t wanted = *capacity ? *capacity * 2 : STREAM_CHUNK;
    if (wanted > REQUEST_PREFIX_MAX) {
        wanted = REQUEST_PREFIX_MAX;
    }
    if (wanted <= *length) {
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t *resized = realloc(*buffer, wanted);
    if (!resized) {
        return ESP_ERR_NO_MEM;
    }
    *buffer = resized;
    *capacity = wanted;

    int received = receive_request(connection, (char *)*buffer + *length,
                                   wanted - *length);
    if (received == 0) {
        return ESP_ERR_NOT_FINISHED;
    }
    if (received < 0) {
        return ESP_FAIL;
    }
    *length += (size_t)received;
    return ESP_OK;
}

static int upstream_write(void *context, const uint8_t *data, size_t length)
{
    return esp_http_client_write((esp_http_client_handle_t)context,
                                 (const char *)data, length);
}

static esp_err_t write_all(esp_http_client_handle_t client, const uint8_t *data,
                           size_t length)
{
    return ipp_stream_write_all(upstream_write, client, data, length) ==
                   IPP_STREAM_OK
               ? ESP_OK
               : ESP_FAIL;
}

static int downstream_read(void *context, uint8_t *buffer, size_t length)
{
    return receive_request((ipp_connection_t *)context, buffer, length);
}

static esp_err_t stream_remaining_request(ipp_connection_t *connection,
                                          esp_http_client_handle_t client,
                                          size_t remaining)
{
    ipp_stream_result_t result = ipp_stream_copy(
        downstream_read, connection, upstream_write, client, remaining,
        STREAM_CHUNK);
    if (result == IPP_STREAM_NO_MEMORY) {
        return ESP_ERR_NO_MEM;
    }
    return result == IPP_STREAM_OK ? ESP_OK : ESP_FAIL;
}

static esp_err_t write_upstream_chunk(esp_http_client_handle_t client,
                                      const uint8_t *data, size_t length)
{
    return ipp_stream_write_http_chunk(upstream_write, client, data, length) ==
                   IPP_STREAM_OK
               ? ESP_OK
               : ESP_FAIL;
}

static esp_err_t stream_chunked_request(ipp_connection_t *connection,
                                        esp_http_client_handle_t client)
{
    uint8_t *buffer = malloc(STREAM_CHUNK);
    if (!buffer) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t error = ESP_OK;
    while (true) {
        int received = receive_request(connection, buffer, STREAM_CHUNK);
        if (received == 0) {
            error = write_upstream_chunk(client, NULL, 0);
            break;
        }
        if (received < 0 ||
            write_upstream_chunk(client, buffer, (size_t)received) != ESP_OK) {
            error = ESP_FAIL;
            break;
        }
    }
    free(buffer);
    return error;
}

typedef struct {
    ipp_connection_t *connection;
    const uint8_t *prefix;
    size_t prefix_length;
    size_t prefix_offset;
} raster_document_reader_t;

typedef struct {
    esp_http_client_handle_t client;
} raster_document_writer_t;

static int raster_document_read(void *context, uint8_t *data, size_t length)
{
    raster_document_reader_t *reader = context;
    if (reader->prefix_offset < reader->prefix_length) {
        size_t available = reader->prefix_length - reader->prefix_offset;
        size_t copied = available < length ? available : length;
        memcpy(data, reader->prefix + reader->prefix_offset, copied);
        reader->prefix_offset += copied;
        return copied <= INT_MAX ? (int)copied : -1;
    }
    return receive_request(reader->connection, data, length);
}

static int raster_document_write(void *context, const uint8_t *data,
                                 size_t length)
{
    raster_document_writer_t *writer = context;
    if (length > INT_MAX ||
        write_upstream_chunk(writer->client, data, length) != ESP_OK) {
        return -1;
    }
    return (int)length;
}

static espresso_raster_result_t stream_converted_document(
    ipp_connection_t *connection, esp_http_client_handle_t client,
    const uint8_t *prefix, size_t prefix_length)
{
    raster_document_reader_t reader = {
        .connection = connection,
        .prefix = prefix,
        .prefix_length = prefix_length,
    };
    raster_document_writer_t writer = {.client = client};
    espresso_raster_result_t result = espresso_pwg_to_urf(
        raster_document_read, &reader, raster_document_write, &writer);
    if (result == ESPRESSO_RASTER_OK &&
        write_upstream_chunk(client, NULL, 0) != ESP_OK) {
        return ESPRESSO_RASTER_WRITE_ERROR;
    }
    return result;
}

static esp_err_t read_upstream_response(esp_http_client_handle_t client,
                                        uint8_t **body, size_t *body_length)
{
    size_t capacity = 4096;
    uint8_t *data = malloc(capacity);
    if (!data) {
        return ESP_ERR_NO_MEM;
    }
    size_t length = 0;
    unsigned timeout_retries = 0;
    while (true) {
        if (length == capacity) {
            if (capacity >= RESPONSE_MAX) {
                free(data);
                return ESP_ERR_INVALID_SIZE;
            }
            size_t next = capacity * 2;
            if (next > RESPONSE_MAX) {
                next = RESPONSE_MAX;
            }
            uint8_t *resized = realloc(data, next);
            if (!resized) {
                free(data);
                return ESP_ERR_NO_MEM;
            }
            data = resized;
            capacity = next;
        }
        int received = esp_http_client_read(client, (char *)data + length,
                                            capacity - length);
        if (received == -ESP_ERR_HTTP_EAGAIN) {
            if (++timeout_retries >= RECEIVE_TIMEOUT_RETRIES) {
                free(data);
                return ESP_ERR_TIMEOUT;
            }
            continue;
        }
        if (received < 0) {
            free(data);
            return ESP_FAIL;
        }
        if (received == 0) {
            break;
        }
        timeout_retries = 0;
        length += (size_t)received;
    }
    *body = data;
    *body_length = length;
    return ESP_OK;
}

static esp_err_t proxy_handler(ipp_connection_t *connection)
{
    printer_target_t target;
    if (!app_state_get_target(&target)) {
        ESP_LOGW(TAG, "client %s rejected: no selected printer",
                 connection->peer);
        send_http_error(connection, "503 Service Unavailable",
                        "Select a printer at http://espresso.local first");
        return ESP_FAIL;
    }
    if (!ipp_http_request_is_chunked(connection->request) &&
        ipp_http_request_content_length(connection->request) < 8) {
        send_http_error(connection, "400 Bad Request", "Invalid IPP request");
        return ESP_FAIL;
    }
    const char *content_type =
        ipp_http_request_content_type(connection->request);
    if (!content_type[0] ||
        strncasecmp(content_type, "application/ipp", 15) != 0 ||
        (content_type[15] != '\0' && content_type[15] != ';' &&
         content_type[15] != ' ' && content_type[15] != '\t')) {
        send_http_error(connection, "400 Bad Request",
                        "Content-Type must be application/ipp");
        return ESP_FAIL;
    }
    if (send_continue_if_requested(connection) != ESP_OK) {
        return ESP_FAIL;
    }

    uint8_t *prefix = NULL;
    size_t prefix_length = 0;
    size_t prefix_capacity = 0;
    size_t attributes_length = 0;
    ipp_codec_result_t codec_result = IPP_CODEC_INCOMPLETE;
    ipp_request_info_t request_info;
    while (codec_result == IPP_CODEC_INCOMPLETE) {
        esp_err_t receive_err = receive_more(connection, &prefix, &prefix_length,
                                             &prefix_capacity);
        if (receive_err != ESP_OK) {
            free(prefix);
            send_http_error(connection, "400 Bad Request",
                            "IPP attributes are incomplete or too large");
            return receive_err;
        }
        codec_result = ipp_codec_inspect_request(prefix, prefix_length,
                                                 &request_info);
    }
    if (codec_result != IPP_CODEC_OK) {
        free(prefix);
        send_http_error(connection, "400 Bad Request", "Malformed IPP request");
        return ESP_FAIL;
    }
    attributes_length = request_info.attributes_length;

    bool incoming_chunked =
        ipp_http_request_is_chunked(connection->request);
    bool possible_document_operation =
        request_info.operation_id == IPP_OPERATION_PRINT_JOB ||
        request_info.operation_id == IPP_OPERATION_SEND_DOCUMENT;
    if (incoming_chunked && !possible_document_operation) {
        while (true) {
            esp_err_t receive_err = receive_more(
                connection, &prefix, &prefix_length, &prefix_capacity);
            if (receive_err == ESP_ERR_NOT_FINISHED) {
                break;
            }
            if (receive_err != ESP_OK) {
                free(prefix);
                send_http_error(connection, "400 Bad Request",
                                "Malformed chunked IPP request");
                return receive_err;
            }
        }
    } else if (incoming_chunked &&
               request_info.operation_id == IPP_OPERATION_PRINT_JOB &&
               prefix_length == attributes_length) {
        esp_err_t receive_err = receive_more(
            connection, &prefix, &prefix_length, &prefix_capacity);
        if (receive_err != ESP_OK && receive_err != ESP_ERR_NOT_FINISHED) {
            free(prefix);
            send_http_error(connection, "400 Bad Request",
                            "Malformed chunked IPP request");
            return receive_err;
        }
    }
    size_t request_body_length = incoming_chunked
                                     ? prefix_length
                                     : ipp_http_request_content_length(
                                           connection->request);

    ESP_LOGI(TAG,
             "client %s IPP %u.%u %s(0x%04x) id=%" PRIu32
             " transport=%s body=%zu attributes=%zu format=%s",
             connection->peer, request_info.major, request_info.minor,
             operation_name(request_info.operation_id),
             request_info.operation_id, request_info.request_id,
             incoming_chunked ? "chunked" : "content-length",
             request_body_length, attributes_length,
             request_info.document_format[0] ? request_info.document_format :
                                               "unspecified");
    if (request_info.operation_id == IPP_OPERATION_GET_PRINTER_ATTRIBUTES) {
        ESP_LOGI(TAG, "client %s requested printer attributes: %s",
                 connection->peer,
                 request_info.requested_attributes[0] ?
                     request_info.requested_attributes : "all");
    }

    uint32_t monitor_id = monitor_request_start(&request_info);

    ipp_proxy_plan_t plan;
    ipp_proxy_plan_request(&request_info, &target,
                           request_body_length, &plan);
    if (plan.action == IPP_PROXY_LOCAL_STATUS) {
        ESP_LOGW(TAG,
                 "client %s %s rejected locally: IPP status=0x%04x reason=%s",
                 connection->peer, operation_name(request_info.operation_id),
                 plan.status_code, plan.status_message);
        request_info.major = plan.response_major;
        request_info.minor = plan.response_minor;
        free(prefix);
        monitor_submission_failed(monitor_id, &request_info, 0);
        return send_ipp_status(connection, &request_info, plan.status_code,
                               plan.status_message);
    }
    bool document_operation = plan.document_operation;

    printer_transport_endpoint_t upstream;
    if (!printer_transport_build(&target, &upstream)) {
        free(prefix);
        monitor_submission_failed(monitor_id, &request_info, 0);
        send_http_error(connection, "503 Service Unavailable",
                        "Selected printer transport is invalid");
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t *rewritten_prefix = NULL;
    size_t rewritten_length = 0;
    if (request_info.operation_id != IPP_OPERATION_GET_PRINTER_ATTRIBUTES) {
        char rejected_attribute[128] = {0};
        codec_result = ipp_codec_rewrite_request_for_format_diagnostic(
            prefix, prefix_length, upstream.printer_uri, upstream.authority,
            &target, plan.upstream_document_format,
            &rewritten_prefix, &rewritten_length,
            &attributes_length, rejected_attribute,
            sizeof(rejected_attribute));
        if (codec_result != IPP_CODEC_OK) {
            ESP_LOGW(TAG,
                     "client %s %s rewrite rejected: codec=%d attribute=%s",
                     connection->peer,
                     operation_name(request_info.operation_id), codec_result,
                     rejected_attribute[0] ? rejected_attribute : "unknown");
            free(prefix);
            monitor_submission_failed(monitor_id, &request_info, 0);
            return send_ipp_status(connection, &request_info,
                                   codec_result == IPP_CODEC_UNSUPPORTED ?
                                       IPP_STATUS_CLIENT_ERROR_ATTRIBUTES_OR_VALUES :
                                       IPP_STATUS_CLIENT_ERROR_BAD_REQUEST,
                                   codec_result == IPP_CODEC_UNSUPPORTED ?
                                       "Unsupported job attribute value" :
                                       "Could not translate the IPP request");
        }
    }

    uint8_t client_major = request_info.major;
    uint8_t client_minor = request_info.minor;
    uint16_t operation_id = request_info.operation_id;
    uint8_t upstream_major = plan.upstream_major;
    uint8_t upstream_minor = plan.upstream_minor;
    if (operation_id == IPP_OPERATION_GET_PRINTER_ATTRIBUTES) {
        uint8_t *capability_request = NULL;
        size_t capability_request_length = 0;
        codec_result = ipp_codec_build_get_printer_attributes_for_format(
            upstream_major, upstream_minor, request_info.request_id,
            upstream.printer_uri,
            upstream_major >= 2,
            plan.upstream_document_format ? plan.upstream_document_format :
            (request_info.document_format[0] ? request_info.document_format : NULL),
            &capability_request, &capability_request_length);
        if (codec_result != IPP_CODEC_OK) {
            free(prefix);
            send_http_error(connection, "500 Internal Server Error",
                            "Could not build capability request");
            return ESP_FAIL;
        }
        rewritten_prefix = capability_request;
        rewritten_length = capability_request_length;
        prefix_length = request_body_length;
    } else {
        rewritten_prefix[0] = upstream_major;
        rewritten_prefix[1] = upstream_minor;
    }
    bool converting_document =
        plan.document_transform == IPP_PROXY_DOCUMENT_PWG_TO_URF &&
        document_operation;
    bool outgoing_chunked = converting_document ||
                            (incoming_chunked && document_operation);
    int64_t outgoing_length = -1;
    if (!outgoing_chunked) {
        outgoing_length = (int64_t)request_body_length +
                          (int64_t)rewritten_length -
                          (int64_t)prefix_length;
    }
    if (!outgoing_chunked &&
        (outgoing_length < 0 || outgoing_length > INT32_MAX)) {
        free(rewritten_prefix);
        free(prefix);
        send_http_error(connection, "400 Bad Request", "Invalid IPP length");
        return ESP_FAIL;
    }

    esp_http_client_config_t config = {
        .url = upstream.http_url,
        .timeout_ms = document_operation ? 120000 : 30000,
        .buffer_size = 4096,
        .buffer_size_tx = 4096,
        .disable_auto_redirect = true,
        .crt_bundle_attach = upstream.verify_certificate ?
                                 esp_crt_bundle_attach : NULL,
        .common_name = upstream.verify_certificate ?
                           upstream.certificate_name : NULL,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(rewritten_prefix);
        free(prefix);
        monitor_submission_failed(monitor_id, &request_info, 0);
        send_http_error(connection, "500 Internal Server Error",
                        "Unable to allocate printer connection");
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/ipp");
    esp_http_client_set_header(client, "Accept", "application/ipp");
    esp_http_client_set_header(client, "User-Agent", "ESPresso/phase3");

    ESP_LOGI(TAG,
             "client %s forwarding %s as IPP %u.%u to %s transport=%s",
             connection->peer, operation_name(operation_id), upstream_major,
             upstream_minor, upstream.http_url,
             outgoing_chunked ? "chunked" : "content-length");

    esp_err_t err = esp_http_client_open(
        client, outgoing_chunked ? -1 : (int)outgoing_length);
    if (err == ESP_OK) {
        size_t rewritten_send_length = converting_document ?
                                           attributes_length :
                                           rewritten_length;
        err = outgoing_chunked
                  ? write_upstream_chunk(client, rewritten_prefix,
                                         rewritten_send_length)
                  : write_all(client, rewritten_prefix,
                              rewritten_send_length);
    }
    free(rewritten_prefix);
    rewritten_prefix = NULL;
    if (err == ESP_OK) {
        if (converting_document) {
            espresso_raster_result_t raster_result = stream_converted_document(
                connection, client, prefix + request_info.attributes_length,
                prefix_length - request_info.attributes_length);
            if (raster_result == ESPRESSO_RASTER_INVALID ||
                raster_result == ESPRESSO_RASTER_UNSUPPORTED ||
                raster_result == ESPRESSO_RASTER_READ_ERROR) {
                ESP_LOGW(TAG, "client %s PWG Raster conversion rejected: %d",
                         connection->peer, raster_result);
                esp_http_client_cleanup(client);
                free(prefix);
                monitor_submission_failed(monitor_id, &request_info, 0);
                return send_ipp_status(
                    connection, &request_info,
                    raster_result == ESPRESSO_RASTER_UNSUPPORTED ?
                        IPP_STATUS_CLIENT_ERROR_DOCUMENT_FORMAT_NOT_SUPPORTED :
                        IPP_STATUS_CLIENT_ERROR_BAD_REQUEST,
                    raster_result == ESPRESSO_RASTER_UNSUPPORTED ?
                        "PWG Raster layout is not supported by the selected printer" :
                        "PWG Raster document is malformed or incomplete");
            }
            err = raster_result == ESPRESSO_RASTER_OK ? ESP_OK : ESP_FAIL;
        } else {
            err = outgoing_chunked
                      ? stream_chunked_request(connection, client)
                      : stream_remaining_request(
                            connection, client,
                            request_body_length - prefix_length);
        }
    }
    free(prefix);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "forwarding request to %s failed: %s", upstream.http_url,
                 esp_err_to_name(err));
        esp_http_client_cleanup(client);
        monitor_submission_failed(monitor_id, &request_info, 0);
        send_gateway_fault(connection,
                           err == ESP_ERR_TIMEOUT ?
                               IPP_PROXY_FAULT_UPSTREAM_TIMEOUT :
                               IPP_PROXY_FAULT_UPSTREAM_DISCONNECT,
                           document_operation);
        return err;
    }

    int64_t response_headers = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if ((response_headers < 0 && !esp_http_client_is_chunked_response(client)) ||
        status < 200 || status >= 300) {
        ESP_LOGE(TAG, "legacy printer returned HTTP %d", status);
        esp_http_client_cleanup(client);
        monitor_submission_failed(monitor_id, &request_info, 0);
        send_http_error(connection, "502 Bad Gateway",
                        "Legacy printer returned an HTTP error");
        return ESP_FAIL;
    }

    uint8_t *response_body = NULL;
    size_t response_length = 0;
    err = read_upstream_response(client, &response_body, &response_length);
    esp_http_client_cleanup(client);
    if (err != ESP_OK || response_length < 8) {
        ESP_LOGE(TAG,
                 "client %s %s upstream response invalid: result=%s bytes=%zu",
                 connection->peer, operation_name(operation_id),
                 esp_err_to_name(err), response_length);
        free(response_body);
        monitor_submission_failed(monitor_id, &request_info, 0);
        ipp_proxy_fault_t fault = IPP_PROXY_FAULT_INVALID_RESPONSE;
        if (err == ESP_ERR_TIMEOUT) {
            fault = IPP_PROXY_FAULT_UPSTREAM_TIMEOUT;
        } else if (err == ESP_ERR_INVALID_SIZE) {
            fault = IPP_PROXY_FAULT_RESPONSE_TOO_LARGE;
        } else if (err != ESP_OK) {
            fault = IPP_PROXY_FAULT_UPSTREAM_DISCONNECT;
        }
        send_gateway_fault(connection, fault, document_operation);
        return ESP_FAIL;
    }

    uint8_t *client_response = NULL;
    size_t client_response_length = 0;
    uint16_t upstream_ipp_status =
        (uint16_t)(((uint16_t)response_body[2] << 8) | response_body[3]);
    ESP_LOGI(TAG,
             "client %s %s upstream response: HTTP=%d IPP=0x%04x bytes=%zu",
             connection->peer, operation_name(operation_id), status,
             upstream_ipp_status, response_length);
    size_t decoded_length =
        ipp_http_request_decoded_length(connection->request);
    size_t document_bytes = decoded_length >= request_info.attributes_length ?
                                decoded_length - request_info.attributes_length : 0;
    bool upstream_success = upstream_ipp_status < 0x0400;
    if (!upstream_success) {
        monitor_submission_failed(monitor_id, &request_info, document_bytes);
    } else if (operation_id == IPP_OPERATION_PRINT_JOB) {
        monitor_update(monitor_id, ESPRESSO_JOB_COMPLETED,
                       request_info.document_format, document_bytes);
    } else if (operation_id == IPP_OPERATION_CREATE_JOB) {
        uint32_t upstream_job_id = 0;
        if (ipp_codec_get_u32_attribute(response_body, response_length,
                                        "job-id", &upstream_job_id)) {
            monitor_attach_upstream(monitor_id, upstream_job_id);
        }
    } else if (operation_id == IPP_OPERATION_SEND_DOCUMENT) {
        monitor_update(monitor_id,
                       !request_info.has_last_document || request_info.last_document ?
                           ESPRESSO_JOB_COMPLETED : ESPRESSO_JOB_QUEUED,
                       request_info.document_format, document_bytes);
    } else if (operation_id == IPP_OPERATION_CANCEL_JOB) {
        monitor_update(monitor_id, ESPRESSO_JOB_CANCELLED, NULL, 0);
    }
    const char *local_printer_uri = connection->tls ?
        "ipps://espresso.local:8631/ipp/print" :
        "ipp://espresso.local:631/ipp/print";
    const char *local_authority = connection->tls ?
        "ipps://espresso.local:8631" : "ipp://espresso.local:631";
    if (operation_id == IPP_OPERATION_GET_PRINTER_ATTRIBUTES) {
        char local_uuid[ESPRESSO_UUID_MAX];
        printer_identity_target_uuid(&target, local_uuid, sizeof(local_uuid));
        printer_target_t updated = target;
        updated.state_reasons[0] = '\0';
        if (ipp_codec_apply_printer_attributes(response_body, response_length,
                                               &updated) == IPP_CODEC_OK &&
            memcmp(&updated, &target, sizeof(target)) != 0) {
            app_state_update_target(&updated);
            target = updated;
            ESP_ERROR_CHECK_WITHOUT_ABORT(printer_discovery_advertise_selected());
        }
        codec_result = ipp_codec_normalize_printer_response(
            response_body, response_length, local_printer_uri, local_authority,
            local_uuid, &target, &client_response, &client_response_length,
            &attributes_length);
        if (codec_result == IPP_CODEC_OK) {
            uint8_t *filtered = NULL;
            size_t filtered_length = 0;
            codec_result = ipp_codec_filter_printer_response(
                client_response, client_response_length,
                request_info.requested_attributes, &filtered,
                &filtered_length, &attributes_length);
            free(client_response);
            client_response = filtered;
            client_response_length = filtered_length;
        }
    } else {
        codec_result = ipp_codec_rewrite(response_body, response_length,
                                         local_printer_uri, local_authority,
                                         &client_response, &client_response_length,
                                         &attributes_length);
        if (codec_result == IPP_CODEC_OK &&
            (operation_id == IPP_OPERATION_GET_JOB_ATTRIBUTES ||
             operation_id == IPP_OPERATION_GET_JOBS)) {
            const char *requested = request_info.requested_attributes;
            if (operation_id == IPP_OPERATION_GET_JOBS && !requested[0]) {
                requested = "job-uri,job-id";
            }
            uint8_t *filtered = NULL;
            size_t filtered_length = 0;
            codec_result = ipp_codec_filter_response(
                client_response, client_response_length,
                IPP_RESPONSE_KIND_JOB, requested, &filtered,
                &filtered_length, &attributes_length);
            free(client_response);
            client_response = filtered;
            client_response_length = filtered_length;
        }
    }
    free(response_body);
    if (codec_result != IPP_CODEC_OK) {
        ESP_LOGE(TAG, "client %s %s response translation failed: codec=%d",
                 connection->peer, operation_name(operation_id), codec_result);
        free(client_response);
        if (upstream_success && operation_id != IPP_OPERATION_PRINT_JOB &&
            operation_id != IPP_OPERATION_SEND_DOCUMENT &&
            operation_id != IPP_OPERATION_CANCEL_JOB) {
            monitor_update(monitor_id, ESPRESSO_JOB_FAILED,
                           request_info.document_format, document_bytes);
        }
        send_http_error(connection, "502 Bad Gateway",
                        "Could not translate the printer response");
        return ESP_FAIL;
    }
    client_response[0] = client_major;
    client_response[1] = client_minor;
    err = send_http_response(connection, "200 OK", "application/ipp",
                             client_response, client_response_length, false);
    ESP_LOGI(TAG,
             "client %s %s completed: result=%s response-bytes=%zu received-body=%zu",
             connection->peer, operation_name(operation_id),
             esp_err_to_name(err), client_response_length,
             decoded_length);
    free(client_response);
    return err;
}

static void configure_client_socket(int socket_fd)
{
    const struct timeval receive_timeout = {
        .tv_sec = 40,
        .tv_usec = 0,
    };
    const struct timeval send_timeout = {
        .tv_sec = 30,
        .tv_usec = 0,
    };
    setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &receive_timeout,
               sizeof(receive_timeout));
    setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO, &send_timeout,
               sizeof(send_timeout));
}

static void ipp_worker_task(void *argument)
{
    ipp_worker_argument_t *worker = argument;
    ipp_connection_t connection = {
        .socket_fd = worker->socket_fd,
        .request = NULL,
    };
    bool secure = worker->secure;
    free(worker);
    format_peer(connection.socket_fd, connection.peer,
                sizeof(connection.peer));
    esp_err_t tls_error = secure ?
        tls_identity_accept(connection.socket_fd, &connection.tls) : ESP_OK;
    ipp_http_result_t result = tls_error == ESP_OK ?
        ipp_http_request_open(receive_socket, &connection,
                              &connection.request) : IPP_HTTP_IO_ERROR;
    if (result != IPP_HTTP_OK) {
        ESP_LOGW(TAG, "client %s HTTP request rejected: parser=%d",
                 connection.peer, result);
        if (tls_error == ESP_OK) {
            send_http_error(&connection, result == IPP_HTTP_TOO_LARGE
                                             ? "413 Content Too Large"
                                             : "400 Bad Request",
                            "Malformed or unsupported HTTP request");
        }
    } else if (strcasecmp(ipp_http_request_method(connection.request),
                          "POST") != 0) {
        ESP_LOGW(TAG, "client %s HTTP method rejected: %s", connection.peer,
                 ipp_http_request_method(connection.request));
        send_http_error(&connection, "405 Method Not Allowed",
                        "The IPP endpoint accepts POST requests");
    } else {
        proxy_handler(&connection);
    }
    ipp_http_request_destroy(connection.request);
    tls_identity_close(connection.tls);
    shutdown(connection.socket_fd, SHUT_RDWR);
    close(connection.socket_fd);
    xSemaphoreGive(worker_slots);
    vTaskDelete(NULL);
}

static void ipp_accept_task(void *argument)
{
    ipp_listener_t *listener = argument;
    while (true) {
        /* Reserve capacity before accept(). Excess Apple capability probes
         * remain in the TCP backlog instead of receiving a transient 503 or
         * consuming heap needed by active upstream connections. */
        if (xSemaphoreTake(worker_slots, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(listener->plain_socket_fd, &read_fds);
        FD_SET(listener->secure_socket_fd, &read_fds);
        int maximum = listener->plain_socket_fd > listener->secure_socket_fd ?
                          listener->plain_socket_fd :
                          listener->secure_socket_fd;
        if (select(maximum + 1, &read_fds, NULL, NULL, NULL) <= 0) {
            xSemaphoreGive(worker_slots);
            continue;
        }
        bool secure = FD_ISSET(listener->secure_socket_fd, &read_fds);
        int listen_fd = secure ? listener->secure_socket_fd :
                                 listener->plain_socket_fd;
        struct sockaddr_storage address;
        socklen_t address_length = sizeof(address);
        int client_fd = accept(listen_fd, (struct sockaddr *)&address,
                               &address_length);
        if (client_fd < 0) {
            xSemaphoreGive(worker_slots);
            ESP_LOGE(TAG, "IPP accept failed: errno %d", errno);
            continue;
        }
        configure_client_socket(client_fd);
        ipp_worker_argument_t *worker = malloc(sizeof(*worker));
        if (!worker) {
            xSemaphoreGive(worker_slots);
            close(client_fd);
            continue;
        }
        worker->socket_fd = client_fd;
        worker->secure = secure;
        if (xTaskCreate(ipp_worker_task, "espresso_ipp_client",
                        IPP_WORKER_TASK_STACK, worker, 5,
                        NULL) != pdPASS) {
            free(worker);
            xSemaphoreGive(worker_slots);
            close(client_fd);
        }
    }
}

static int open_listener(uint16_t port)
{
#if CONFIG_LWIP_IPV6
    int listen_fd = socket(AF_INET6, SOCK_STREAM, IPPROTO_IP);
#else
    int listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
#endif
    if (listen_fd < 0) {
        return -1;
    }
    int reuse = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#if CONFIG_LWIP_IPV6
    const struct in6_addr any_address = IN6ADDR_ANY_INIT;
    struct sockaddr_in6 address = {
        .sin6_family = AF_INET6,
        .sin6_port = htons(port),
        .sin6_addr = any_address,
    };
#else
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
#endif
    if (bind(listen_fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(listen_fd, IPP_SERVER_BACKLOG) != 0) {
        close(listen_fd);
        return -1;
    }
    return listen_fd;
}

esp_err_t ipp_proxy_start(void)
{
    espresso_job_history_init(&job_history);
    job_history_lock = xSemaphoreCreateMutex();
    if (!job_history_lock) {
        return ESP_ERR_NO_MEM;
    }
    ESP_RETURN_ON_ERROR(tls_identity_init(), TAG,
                        "IPPS identity initialization failed");
    int listen_fd = open_listener(IPP_SERVER_PORT);
    int secure_listen_fd = open_listener(IPPS_SERVER_PORT);
    if (listen_fd < 0) {
        ESP_LOGE(TAG, "IPP socket allocation failed: errno %d", errno);
        vSemaphoreDelete(job_history_lock);
        job_history_lock = NULL;
        return ESP_FAIL;
    }
    if (secure_listen_fd < 0) {
        ESP_LOGE(TAG, "IPPS server bind/listen failed: errno %d", errno);
        close(listen_fd);
        vSemaphoreDelete(job_history_lock);
        job_history_lock = NULL;
        return ESP_FAIL;
    }
    worker_slots = xSemaphoreCreateCounting(IPP_SERVER_WORKERS,
                                            IPP_SERVER_WORKERS);
    if (!worker_slots) {
        close(listen_fd);
        close(secure_listen_fd);
        vSemaphoreDelete(job_history_lock);
        job_history_lock = NULL;
        return ESP_ERR_NO_MEM;
    }
    ipp_listener_t *listener = malloc(sizeof(*listener));
    if (!listener) {
        close(listen_fd);
        close(secure_listen_fd);
        vSemaphoreDelete(worker_slots);
        worker_slots = NULL;
        vSemaphoreDelete(job_history_lock);
        job_history_lock = NULL;
        return ESP_ERR_NO_MEM;
    }
    *listener = (ipp_listener_t){
        .plain_socket_fd = listen_fd,
        .secure_socket_fd = secure_listen_fd,
    };
    if (xTaskCreate(ipp_accept_task, "espresso_ipp", IPP_SERVER_TASK_STACK,
                    listener, 5, NULL) != pdPASS) {
        free(listener);
        vSemaphoreDelete(worker_slots);
        worker_slots = NULL;
        close(listen_fd);
        close(secure_listen_fd);
        vSemaphoreDelete(job_history_lock);
        job_history_lock = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG,
             "IPP compatibility endpoints listening on ports %d and %d "
             "(%d workers, "
             "%d-socket lwIP table)",
             IPP_SERVER_PORT, IPPS_SERVER_PORT, IPP_SERVER_WORKERS,
             CONFIG_LWIP_MAX_SOCKETS);
    return ESP_OK;
}
