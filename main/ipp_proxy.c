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
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "ipp_codec.h"
#include "ipp_http_request.h"
#include "ipp_proxy_core.h"
#include "ipp_stream.h"
#include "printer_discovery.h"
#include "printer_identity.h"

#define REQUEST_PREFIX_MAX (64 * 1024)
#define RESPONSE_MAX (128 * 1024)
#define STREAM_CHUNK 4096
#define RECEIVE_TIMEOUT_RETRIES 3
#define IPP_SERVER_PORT 631
#define IPP_SERVER_BACKLOG 4
#define IPP_SERVER_WORKERS 2
#define IPP_SERVER_TASK_STACK 4096
#define IPP_WORKER_TASK_STACK 8192

static const char *TAG = "espresso_proxy";
static SemaphoreHandle_t worker_slots;

typedef struct {
    int socket_fd;
    ipp_http_request_t *request;
} ipp_connection_t;

static void format_host(const char *address, char *host, size_t host_size)
{
    if (strchr(address, ':')) {
        snprintf(host, host_size, "[%s]", address);
    } else {
        snprintf(host, host_size, "%s", address);
    }
}

static int socket_write(void *context, const uint8_t *data, size_t length)
{
    ipp_connection_t *connection = context;
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
                                       response, response_length, true);
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
    int socket_fd = (int)(intptr_t)context;
    for (unsigned attempt = 0; attempt < RECEIVE_TIMEOUT_RETRIES; ++attempt) {
        ssize_t received = recv(socket_fd, buffer, length, 0);
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

    ipp_proxy_plan_t plan;
    ipp_proxy_plan_request(&request_info, &target,
                           request_body_length, &plan);
    if (plan.action == IPP_PROXY_LOCAL_STATUS) {
        ipp_request_info_t response_info = request_info;
        response_info.major = plan.response_major;
        response_info.minor = plan.response_minor;
        free(prefix);
        return send_ipp_status(connection, &response_info, plan.status_code,
                               plan.status_message);
    }
    bool document_operation = plan.document_operation;

    char upstream_url[ESPRESSO_ADDRESS_MAX + ESPRESSO_PATH_MAX + 32];
    char upstream_printer_uri[sizeof(upstream_url)];
    char upstream_authority[ESPRESSO_ADDRESS_MAX + 32];
    char upstream_host[ESPRESSO_ADDRESS_MAX + 3];
    format_host(target.address, upstream_host, sizeof(upstream_host));
    snprintf(upstream_url, sizeof(upstream_url), "http://%s:%u%s", upstream_host,
             target.port, target.resource_path);
    snprintf(upstream_printer_uri, sizeof(upstream_printer_uri), "ipp://%s:%u%s",
             upstream_host, target.port, target.resource_path);
    snprintf(upstream_authority, sizeof(upstream_authority), "ipp://%s:%u",
             upstream_host, target.port);

    uint8_t *rewritten_prefix = NULL;
    size_t rewritten_length = 0;
    if (request_info.operation_id != IPP_OPERATION_GET_PRINTER_ATTRIBUTES) {
        codec_result = ipp_codec_rewrite_request(
            prefix, prefix_length, upstream_printer_uri, upstream_authority,
            &target, &rewritten_prefix, &rewritten_length,
            &attributes_length);
        if (codec_result != IPP_CODEC_OK) {
            free(prefix);
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
            upstream_printer_uri,
            upstream_major >= 2,
            request_info.document_format[0] ? request_info.document_format : NULL,
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
    bool outgoing_chunked = incoming_chunked && document_operation;
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
        .url = upstream_url,
        .timeout_ms = document_operation ? 120000 : 30000,
        .buffer_size = 4096,
        .buffer_size_tx = 4096,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(rewritten_prefix);
        free(prefix);
        send_http_error(connection, "500 Internal Server Error",
                        "Unable to allocate printer connection");
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/ipp");
    esp_http_client_set_header(client, "Accept", "application/ipp");
    esp_http_client_set_header(client, "User-Agent", "ESPresso/phase1");

    esp_err_t err = esp_http_client_open(
        client, outgoing_chunked ? -1 : (int)outgoing_length);
    if (err == ESP_OK) {
        err = outgoing_chunked
                  ? write_upstream_chunk(client, rewritten_prefix,
                                         rewritten_length)
                  : write_all(client, rewritten_prefix, rewritten_length);
    }
    free(rewritten_prefix);
    rewritten_prefix = NULL;
    if (err == ESP_OK) {
        err = outgoing_chunked
                  ? stream_chunked_request(connection, client)
                  : stream_remaining_request(
                        connection, client, request_body_length - prefix_length);
    }
    free(prefix);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "forwarding request to %s failed: %s", upstream_url,
                 esp_err_to_name(err));
        esp_http_client_cleanup(client);
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
        send_http_error(connection, "502 Bad Gateway",
                        "Legacy printer returned an HTTP error");
        return ESP_FAIL;
    }

    uint8_t *response_body = NULL;
    size_t response_length = 0;
    err = read_upstream_response(client, &response_body, &response_length);
    esp_http_client_cleanup(client);
    if (err != ESP_OK || response_length < 8) {
        free(response_body);
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
    const char *local_printer_uri = "ipp://espresso.local:631/ipp/print";
    const char *local_authority = "ipp://espresso.local:631";
    if (operation_id == IPP_OPERATION_GET_PRINTER_ATTRIBUTES) {
        char local_uuid[ESPRESSO_UUID_MAX];
        printer_identity_uuid(local_uuid, sizeof(local_uuid));
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
        free(client_response);
        send_http_error(connection, "502 Bad Gateway",
                        "Could not translate the printer response");
        return ESP_FAIL;
    }
    client_response[0] = client_major;
    client_response[1] = client_minor;
    err = send_http_response(connection, "200 OK", "application/ipp",
                             client_response, client_response_length, true);
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
    ipp_connection_t connection = {
        .socket_fd = (int)(intptr_t)argument,
        .request = NULL,
    };
    ipp_http_result_t result = ipp_http_request_open(
        receive_socket, (void *)(intptr_t)connection.socket_fd,
        &connection.request);
    if (result != IPP_HTTP_OK) {
        send_http_error(&connection, result == IPP_HTTP_TOO_LARGE
                                         ? "413 Content Too Large"
                                         : "400 Bad Request",
                        "Malformed or unsupported HTTP request");
    } else if (strcasecmp(ipp_http_request_method(connection.request),
                          "POST") != 0) {
        send_http_error(&connection, "405 Method Not Allowed",
                        "The IPP endpoint accepts POST requests");
    } else {
        proxy_handler(&connection);
    }
    ipp_http_request_destroy(connection.request);
    shutdown(connection.socket_fd, SHUT_RDWR);
    close(connection.socket_fd);
    xSemaphoreGive(worker_slots);
    vTaskDelete(NULL);
}

static void ipp_accept_task(void *argument)
{
    int listen_fd = (int)(intptr_t)argument;
    while (true) {
        struct sockaddr_storage address;
        socklen_t address_length = sizeof(address);
        int client_fd = accept(listen_fd, (struct sockaddr *)&address,
                               &address_length);
        if (client_fd < 0) {
            ESP_LOGE(TAG, "IPP accept failed: errno %d", errno);
            continue;
        }
        configure_client_socket(client_fd);
        if (xSemaphoreTake(worker_slots, 0) != pdTRUE) {
            ipp_connection_t busy = {
                .socket_fd = client_fd,
                .request = NULL,
            };
            send_http_error(&busy, "503 Service Unavailable",
                            "ESPresso is processing other print requests");
            shutdown(client_fd, SHUT_RDWR);
            close(client_fd);
            continue;
        }
        if (xTaskCreate(ipp_worker_task, "espresso_ipp_client",
                        IPP_WORKER_TASK_STACK, (void *)(intptr_t)client_fd, 5,
                        NULL) != pdPASS) {
            xSemaphoreGive(worker_slots);
            close(client_fd);
        }
    }
}

esp_err_t ipp_proxy_start(void)
{
#if CONFIG_LWIP_IPV6
    int listen_fd = socket(AF_INET6, SOCK_STREAM, IPPROTO_IP);
#else
    int listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
#endif
    if (listen_fd < 0) {
        ESP_LOGE(TAG, "IPP socket allocation failed: errno %d", errno);
        return ESP_FAIL;
    }
    int reuse = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#if CONFIG_LWIP_IPV6
    const struct in6_addr any_address = IN6ADDR_ANY_INIT;
    struct sockaddr_in6 address = {
        .sin6_family = AF_INET6,
        .sin6_port = htons(IPP_SERVER_PORT),
        .sin6_addr = any_address,
    };
#else
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(IPP_SERVER_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
#endif
    if (bind(listen_fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(listen_fd, IPP_SERVER_BACKLOG) != 0) {
        ESP_LOGE(TAG, "IPP server bind/listen failed: errno %d", errno);
        close(listen_fd);
        return ESP_FAIL;
    }
    worker_slots = xSemaphoreCreateCounting(IPP_SERVER_WORKERS,
                                            IPP_SERVER_WORKERS);
    if (!worker_slots) {
        close(listen_fd);
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(ipp_accept_task, "espresso_ipp", IPP_SERVER_TASK_STACK,
                    (void *)(intptr_t)listen_fd, 5, NULL) != pdPASS) {
        vSemaphoreDelete(worker_slots);
        worker_slots = NULL;
        close(listen_fd);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "IPP compatibility endpoint listening on port %d",
             IPP_SERVER_PORT);
    return ESP_OK;
}
