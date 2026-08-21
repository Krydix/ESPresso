#include "ipp_proxy.h"

#include <stdlib.h>
#include <strings.h>
#include <string.h>

#include "app_state.h"
#include "esp_check.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "ipp_codec.h"
#include "printer_discovery.h"
#include "printer_identity.h"

#define REQUEST_PREFIX_MAX (64 * 1024)
#define RESPONSE_MAX (128 * 1024)
#define STREAM_CHUNK 4096
#define RECEIVE_TIMEOUT_RETRIES 3

static const char *TAG = "espresso_proxy";

static void format_host(const char *address, char *host, size_t host_size)
{
    if (strchr(address, ':')) {
        snprintf(host, host_size, "[%s]", address);
    } else {
        snprintf(host, host_size, "%s", address);
    }
}

static esp_err_t send_http_error(httpd_req_t *request, const char *status,
                                 const char *message)
{
    httpd_resp_set_status(request, status);
    httpd_resp_set_type(request, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(request, message);
}

static esp_err_t send_ipp_status(httpd_req_t *request,
                                 const ipp_request_info_t *info,
                                 uint16_t status, const char *message)
{
    uint8_t *response = NULL;
    size_t response_length = 0;
    ipp_codec_result_t result = ipp_codec_build_status_response(
        info->major, info->minor, status, info->request_id, message,
        &response, &response_length);
    if (result != IPP_CODEC_OK) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Could not build IPP response");
    }
    httpd_resp_set_type(request, "application/ipp");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(request, (const char *)response,
                                    response_length);
    free(response);
    return err;
}

static esp_err_t send_continue_if_requested(httpd_req_t *request)
{
    char expect[32];
    if (httpd_req_get_hdr_value_str(request, "Expect", expect,
                                    sizeof(expect)) != ESP_OK ||
        strcasecmp(expect, "100-continue") != 0) {
        return ESP_OK;
    }
    static const char response[] = "HTTP/1.1 100 Continue\r\n\r\n";
    int socket_fd = httpd_req_to_sockfd(request);
    int sent = httpd_socket_send(request->handle, socket_fd, response,
                                 sizeof(response) - 1, 0);
    return sent == (int)(sizeof(response) - 1) ? ESP_OK : ESP_FAIL;
}

static int receive_request(httpd_req_t *request, void *buffer, size_t length)
{
    for (unsigned attempt = 0; attempt < RECEIVE_TIMEOUT_RETRIES; ++attempt) {
        int received = httpd_req_recv(request, buffer, length);
        if (received != HTTPD_SOCK_ERR_TIMEOUT) {
            return received;
        }
    }
    return HTTPD_SOCK_ERR_TIMEOUT;
}

static esp_err_t receive_more(httpd_req_t *request, uint8_t **buffer, size_t *length,
                              size_t *capacity)
{
    if (*length >= REQUEST_PREFIX_MAX) {
        return ESP_ERR_NO_MEM;
    }
    size_t wanted = *capacity ? *capacity * 2 : STREAM_CHUNK;
    if (wanted > REQUEST_PREFIX_MAX) {
        wanted = REQUEST_PREFIX_MAX;
    }
    if (wanted > (size_t)request->content_len) {
        wanted = request->content_len;
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

    int received = receive_request(request, (char *)*buffer + *length,
                                   wanted - *length);
    if (received <= 0) {
        return ESP_FAIL;
    }
    *length += (size_t)received;
    return ESP_OK;
}

static esp_err_t write_all(esp_http_client_handle_t client, const uint8_t *data,
                           size_t length)
{
    size_t sent = 0;
    while (sent < length) {
        int result = esp_http_client_write(client, (const char *)data + sent,
                                           length - sent);
        if (result <= 0) {
            return ESP_FAIL;
        }
        sent += (size_t)result;
    }
    return ESP_OK;
}

static esp_err_t stream_remaining_request(httpd_req_t *request,
                                          esp_http_client_handle_t client,
                                          size_t remaining)
{
    uint8_t *chunk = malloc(STREAM_CHUNK);
    if (!chunk) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = ESP_OK;
    while (remaining > 0) {
        size_t wanted = remaining < STREAM_CHUNK ? remaining : STREAM_CHUNK;
        int received = receive_request(request, (char *)chunk, wanted);
        if (received <= 0) {
            err = ESP_FAIL;
            break;
        }
        err = write_all(client, chunk, (size_t)received);
        if (err != ESP_OK) {
            break;
        }
        remaining -= (size_t)received;
    }
    free(chunk);
    return err;
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

static esp_err_t proxy_handler(httpd_req_t *request)
{
    printer_target_t target;
    if (!app_state_get_target(&target)) {
        send_http_error(request, "503 Service Unavailable",
                        "Select a printer at http://espresso.local first");
        return ESP_FAIL;
    }
    if (request->content_len < 8) {
        httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid IPP request");
        return ESP_FAIL;
    }
    char content_type[64];
    if (httpd_req_get_hdr_value_str(request, "Content-Type", content_type,
                                    sizeof(content_type)) != ESP_OK ||
        strncasecmp(content_type, "application/ipp", 15) != 0 ||
        (content_type[15] != '\0' && content_type[15] != ';' &&
         content_type[15] != ' ' && content_type[15] != '\t')) {
        httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                            "Content-Type must be application/ipp");
        return ESP_FAIL;
    }
    if (send_continue_if_requested(request) != ESP_OK) {
        return ESP_FAIL;
    }

    uint8_t *prefix = NULL;
    size_t prefix_length = 0;
    size_t prefix_capacity = 0;
    size_t attributes_length = 0;
    ipp_codec_result_t codec_result = IPP_CODEC_INCOMPLETE;
    ipp_request_info_t request_info;
    while (codec_result == IPP_CODEC_INCOMPLETE) {
        esp_err_t receive_err = receive_more(request, &prefix, &prefix_length,
                                             &prefix_capacity);
        if (receive_err != ESP_OK) {
            free(prefix);
            httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                                "IPP attributes are incomplete or too large");
            return receive_err;
        }
        codec_result = ipp_codec_inspect_request(prefix, prefix_length,
                                                 &request_info);
    }
    if (codec_result != IPP_CODEC_OK) {
        free(prefix);
        httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Malformed IPP request");
        return ESP_FAIL;
    }
    attributes_length = request_info.attributes_length;

    if (!((request_info.major == 1 && request_info.minor == 1) ||
          (request_info.major == 2 && request_info.minor == 0))) {
        ipp_request_info_t response_info = request_info;
        response_info.major = 2;
        response_info.minor = 0;
        free(prefix);
        return send_ipp_status(
            request, &response_info,
            IPP_STATUS_SERVER_ERROR_VERSION_NOT_SUPPORTED,
            "ESPresso supports IPP/1.1 and IPP/2.0");
    }
    if (request_info.request_id == 0 ||
        !request_info.operation_attributes_valid ||
        !request_info.has_attributes_charset ||
        !request_info.has_natural_language || !request_info.has_target_uri) {
        free(prefix);
        return send_ipp_status(request, &request_info,
                               IPP_STATUS_CLIENT_ERROR_BAD_REQUEST,
                               "Missing required IPP operation attribute");
    }
    if (strcasecmp(request_info.attributes_charset, "utf-8") != 0) {
        free(prefix);
        return send_ipp_status(request, &request_info,
                               IPP_STATUS_CLIENT_ERROR_CHARSET_NOT_SUPPORTED,
                               "Only utf-8 IPP attributes are supported");
    }

    uint64_t relayed_operations = ipp_codec_relay_operations(
        target.operations_supported);
    if (request_info.operation_id >= 64 ||
        (relayed_operations & (1ULL << request_info.operation_id)) == 0) {
        free(prefix);
        return send_ipp_status(request, &request_info,
                               IPP_STATUS_SERVER_ERROR_OPERATION_NOT_SUPPORTED,
                               "Operation is not supported by the selected printer");
    }
    bool document_operation =
        request_info.operation_id == IPP_OPERATION_PRINT_JOB ||
        request_info.operation_id == IPP_OPERATION_SEND_DOCUMENT;
    if (request_info.operation_id == IPP_OPERATION_PRINT_JOB &&
        (size_t)request->content_len == attributes_length) {
        free(prefix);
        return send_ipp_status(request, &request_info,
                               IPP_STATUS_CLIENT_ERROR_BAD_REQUEST,
                               "Print-Job requires document data");
    }
    if (!document_operation && (size_t)request->content_len != attributes_length) {
        free(prefix);
        return send_ipp_status(request, &request_info,
                               IPP_STATUS_CLIENT_ERROR_BAD_REQUEST,
                               "This operation cannot contain document data");
    }
    if (request_info.document_format[0] &&
        !ipp_codec_format_supported(&target, request_info.document_format)) {
        free(prefix);
        return send_ipp_status(
            request, &request_info,
            IPP_STATUS_CLIENT_ERROR_DOCUMENT_FORMAT_NOT_SUPPORTED,
            "Document format is not accepted unchanged by the selected printer");
    }

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
        codec_result = ipp_codec_rewrite(
            prefix, prefix_length, upstream_printer_uri, upstream_authority,
            &rewritten_prefix, &rewritten_length, &attributes_length);
        if (codec_result != IPP_CODEC_OK) {
            free(prefix);
            return send_ipp_status(request, &request_info,
                                   IPP_STATUS_CLIENT_ERROR_BAD_REQUEST,
                                   "Could not translate the IPP request");
        }
    }

    uint8_t client_major = request_info.major;
    uint8_t client_minor = request_info.minor;
    uint16_t operation_id = request_info.operation_id;
    uint8_t upstream_major = target.capability_queried &&
                             target.upstream_ipp_major >= 2 ?
                             target.upstream_ipp_major : 1;
    uint8_t upstream_minor = upstream_major >= 2 ? target.upstream_ipp_minor : 1;
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
            httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                "Could not build capability request");
            return ESP_FAIL;
        }
        rewritten_prefix = capability_request;
        rewritten_length = capability_request_length;
        prefix_length = (size_t)request->content_len;
    } else {
        rewritten_prefix[0] = upstream_major;
        rewritten_prefix[1] = upstream_minor;
    }
    int64_t outgoing_length = (int64_t)request->content_len +
                              (int64_t)rewritten_length - (int64_t)prefix_length;
    if (outgoing_length < 0 || outgoing_length > INT32_MAX) {
        free(rewritten_prefix);
        free(prefix);
        httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid IPP length");
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
        httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Unable to allocate printer connection");
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/ipp");
    esp_http_client_set_header(client, "Accept", "application/ipp");
    esp_http_client_set_header(client, "User-Agent", "ESPresso/phase1");

    esp_err_t err = esp_http_client_open(client, (int)outgoing_length);
    if (err == ESP_OK) {
        err = write_all(client, rewritten_prefix, rewritten_length);
    }
    free(rewritten_prefix);
    rewritten_prefix = NULL;
    if (err == ESP_OK) {
        err = stream_remaining_request(request, client,
                                       (size_t)request->content_len - prefix_length);
    }
    free(prefix);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "forwarding request to %s failed: %s", upstream_url,
                 esp_err_to_name(err));
        esp_http_client_cleanup(client);
        send_http_error(request, "502 Bad Gateway",
                        "Legacy printer did not accept the job");
        return err;
    }

    int64_t response_headers = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if ((response_headers < 0 && !esp_http_client_is_chunked_response(client)) ||
        status < 200 || status >= 300) {
        ESP_LOGE(TAG, "legacy printer returned HTTP %d", status);
        esp_http_client_cleanup(client);
        send_http_error(request, "502 Bad Gateway",
                        "Legacy printer returned an HTTP error");
        return ESP_FAIL;
    }

    uint8_t *response_body = NULL;
    size_t response_length = 0;
    err = read_upstream_response(client, &response_body, &response_length);
    esp_http_client_cleanup(client);
    if (err != ESP_OK || response_length < 8) {
        free(response_body);
        send_http_error(request, "502 Bad Gateway",
                        "Legacy printer returned an invalid IPP response");
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
        if (codec_result == IPP_CODEC_OK &&
            request_info.requested_attributes[0]) {
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
    }
    free(response_body);
    if (codec_result != IPP_CODEC_OK) {
        free(client_response);
        send_http_error(request, "502 Bad Gateway",
                        "Could not translate the printer response");
        return ESP_FAIL;
    }
    client_response[0] = client_major;
    client_response[1] = client_minor;
    httpd_resp_set_type(request, "application/ipp");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    err = httpd_resp_send(request, (const char *)client_response,
                          client_response_length);
    free(client_response);
    return err;
}

esp_err_t ipp_proxy_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 631;
    config.ctrl_port = 32769;
    config.max_open_sockets = 4;
    config.lru_purge_enable = true;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.stack_size = 8192;
    config.recv_wait_timeout = 120;
    config.send_wait_timeout = 30;

    httpd_handle_t server = NULL;
    ESP_RETURN_ON_ERROR(httpd_start(&server, &config), TAG, "IPP server failed");
    const httpd_uri_t handler = {
        .uri = "/*",
        .method = HTTP_POST,
        .handler = proxy_handler,
    };
    return httpd_register_uri_handler(server, &handler);
}
