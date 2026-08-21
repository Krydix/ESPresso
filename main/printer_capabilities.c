#include "printer_capabilities.h"

#include <stdio.h>
#include <stdlib.h>

#include "esp_http_client.h"
#include "esp_log.h"
#include "ipp_codec.h"

#define CAPABILITY_RESPONSE_MAX (128 * 1024)
#define IPP_STATUS_ERROR_BAD_REQUEST 0x0400
#define IPP_STATUS_ERROR_VERSION_NOT_SUPPORTED 0x0503

static const char *TAG = "espresso_caps";
static uint32_t s_request_id = 1;

static esp_err_t write_request(esp_http_client_handle_t client,
                               const uint8_t *request, size_t request_length)
{
    size_t written = 0;
    while (written < request_length) {
        int result = esp_http_client_write(client, (const char *)request + written,
                                           request_length - written);
        if (result <= 0) {
            return ESP_FAIL;
        }
        written += (size_t)result;
    }
    return ESP_OK;
}

static esp_err_t read_response(esp_http_client_handle_t client, uint8_t **body,
                               size_t *body_length)
{
    size_t capacity = 4096;
    size_t length = 0;
    uint8_t *data = malloc(capacity);
    if (!data) {
        return ESP_ERR_NO_MEM;
    }
    while (true) {
        if (length == capacity) {
            if (capacity >= CAPABILITY_RESPONSE_MAX) {
                free(data);
                return ESP_ERR_INVALID_SIZE;
            }
            size_t next = capacity * 2;
            if (next > CAPABILITY_RESPONSE_MAX) {
                next = CAPABILITY_RESPONSE_MAX;
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
        if (received < 0) {
            free(data);
            return ESP_FAIL;
        }
        if (received == 0) {
            break;
        }
        length += (size_t)received;
    }
    *body = data;
    *body_length = length;
    return ESP_OK;
}

static esp_err_t query(const printer_target_t *target, uint8_t major, uint8_t minor,
                       bool include_media_col_database, uint8_t **response,
                       size_t *response_length)
{
    char http_url[ESPRESSO_ADDRESS_MAX + ESPRESSO_PATH_MAX + 32];
    char printer_uri[sizeof(http_url)];
    snprintf(http_url, sizeof(http_url), "http://%s:%u%s", target->address,
             target->port, target->resource_path);
    snprintf(printer_uri, sizeof(printer_uri), "ipp://%s:%u%s", target->address,
             target->port, target->resource_path);

    uint8_t *request = NULL;
    size_t request_length = 0;
    ipp_codec_result_t codec = ipp_codec_build_get_printer_attributes(
        major, minor, s_request_id++, printer_uri, include_media_col_database,
        &request, &request_length);
    if (codec != IPP_CODEC_OK) {
        return codec == IPP_CODEC_NO_MEMORY ? ESP_ERR_NO_MEM : ESP_FAIL;
    }

    esp_http_client_config_t config = {
        .url = http_url,
        .timeout_ms = 4000,
        .buffer_size = 4096,
        .buffer_size_tx = 1024,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(request);
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/ipp");
    esp_http_client_set_header(client, "Accept", "application/ipp");
    esp_err_t err = esp_http_client_open(client, (int)request_length);
    if (err == ESP_OK) {
        err = write_request(client, request, request_length);
    }
    free(request);
    if (err == ESP_OK) {
        int64_t headers = esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        if ((headers < 0 && !esp_http_client_is_chunked_response(client)) ||
            status < 200 || status >= 300) {
            err = ESP_FAIL;
        }
    }
    if (err == ESP_OK) {
        err = read_response(client, response, response_length);
    }
    esp_http_client_cleanup(client);
    return err;
}

esp_err_t printer_capabilities_probe(printer_target_t *target)
{
    if (!target || !target->address[0] || !target->resource_path[0] ||
        target->port == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t *response = NULL;
    size_t response_length = 0;
    esp_err_t err = query(target, 2, 0, true, &response, &response_length);
    uint16_t status = ipp_codec_message_code(response, response_length);
    if (err != ESP_OK || status == IPP_STATUS_ERROR_BAD_REQUEST ||
        status == IPP_STATUS_ERROR_VERSION_NOT_SUPPORTED || status >= 0x0400) {
        free(response);
        response = NULL;
        response_length = 0;
        ESP_LOGI(TAG, "%s did not accept IPP/2.0 capability query; trying IPP/1.1",
                 target->instance);
        err = query(target, 1, 1, false, &response, &response_length);
        status = ipp_codec_message_code(response, response_length);
    }
    if (err != ESP_OK || status >= 0x0400) {
        free(response);
        ESP_LOGW(TAG, "capability query failed for %s (transport=%s, IPP=0x%04x)",
                 target->instance, esp_err_to_name(err), status);
        return err != ESP_OK ? err : ESP_FAIL;
    }

    ipp_codec_result_t codec = ipp_codec_apply_printer_attributes(
        response, response_length, target);
    free(response);
    if (codec != IPP_CODEC_OK) {
        ESP_LOGW(TAG, "malformed capability response from %s", target->instance);
        return codec == IPP_CODEC_NO_MEMORY ? ESP_ERR_NO_MEM : ESP_FAIL;
    }
    target->capability_queried = true;
    ESP_LOGI(TAG, "%s: IPP %u.%u, formats=%s, URF=%s", target->instance,
             target->upstream_ipp_major, target->upstream_ipp_minor,
             target->pdl, target->urf);
    return ESP_OK;
}
