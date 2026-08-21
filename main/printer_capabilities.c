#include "printer_capabilities.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_state.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "ipp_codec.h"

#define CAPABILITY_RESPONSE_MAX (128 * 1024)
#define IPP_STATUS_ERROR_BAD_REQUEST 0x0400
#define IPP_STATUS_ERROR_VERSION_NOT_SUPPORTED 0x0503
#define FORMAT_QUERY_MAX 6
#define REFRESH_INTERVAL_MS 60000

static const char *TAG = "espresso_caps";
static uint32_t s_request_id = 1;
static SemaphoreHandle_t s_refresh_lock;
static TickType_t s_last_refresh;
static char s_last_endpoint[ESPRESSO_ADDRESS_MAX + ESPRESSO_PATH_MAX + 16];

static void format_host(const char *address, char *host, size_t host_size)
{
    if (strchr(address, ':')) {
        snprintf(host, host_size, "[%s]", address);
    } else {
        snprintf(host, host_size, "%s", address);
    }
}

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
    unsigned timeout_retries = 0;
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
        if (received == -ESP_ERR_HTTP_EAGAIN) {
            if (++timeout_retries >= 3) {
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

static esp_err_t query(const printer_target_t *target, uint8_t major, uint8_t minor,
                       bool include_media_col_database, const char *document_format,
                       uint8_t **response, size_t *response_length)
{
    char http_url[ESPRESSO_ADDRESS_MAX + ESPRESSO_PATH_MAX + 32];
    char printer_uri[sizeof(http_url)];
    char host[ESPRESSO_ADDRESS_MAX + 3];
    format_host(target->address, host, sizeof(host));
    snprintf(http_url, sizeof(http_url), "http://%s:%u%s", host,
             target->port, target->resource_path);
    snprintf(printer_uri, sizeof(printer_uri), "ipp://%s:%u%s", host,
             target->port, target->resource_path);

    uint8_t *request = NULL;
    size_t request_length = 0;
    ipp_codec_result_t codec = ipp_codec_build_get_printer_attributes_for_format(
        major, minor, s_request_id++, printer_uri, include_media_col_database,
        document_format, &request, &request_length);
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
    target->operations_supported = 0;
    target->printer_state = 0;
    target->accepting_jobs = false;
    target->accepting_jobs_known = false;
    target->state_reasons[0] = '\0';

    esp_err_t err = query(target, 2, 0, true, NULL, &response, &response_length);
    uint16_t status = ipp_codec_message_code(response, response_length);
    if (err != ESP_OK || status == IPP_STATUS_ERROR_BAD_REQUEST ||
        status == IPP_STATUS_ERROR_VERSION_NOT_SUPPORTED || status >= 0x0400) {
        free(response);
        response = NULL;
        response_length = 0;
        ESP_LOGI(TAG, "%s did not accept IPP/2.0 capability query; trying IPP/1.1",
                 target->instance);
        err = query(target, 1, 1, false, NULL, &response, &response_length);
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

    /* IPP capabilities can be conditional on document-format. Merge a bounded
     * number of exact-format responses, prioritizing URF for the AirPrint facade. */
    char formats[ESPRESSO_PDL_MAX];
    snprintf(formats, sizeof(formats), "%s", target->pdl);
    size_t queried = 0;
    bool has_priority_urf = strstr(formats, "image/urf") != NULL;
    for (size_t pass = 0; pass < 2 && queried < FORMAT_QUERY_MAX; ++pass) {
        char *cursor = formats;
        while (*cursor && queried < FORMAT_QUERY_MAX) {
            while (*cursor == ',' || *cursor == ' ') {
                ++cursor;
            }
            char *start = cursor;
            while (*cursor && *cursor != ',') {
                ++cursor;
            }
            char saved = *cursor;
            *cursor = '\0';
            char *end = cursor;
            while (end > start && end[-1] == ' ') {
                --end;
            }
            char saved_end = *end;
            *end = '\0';
            bool selected = *start && ((pass == 0 && has_priority_urf &&
                                        strcmp(start, "image/urf") == 0) ||
                                       (pass == 1 &&
                                        (!has_priority_urf ||
                                         strcmp(start, "image/urf") != 0)));
            if (selected) {
                uint8_t *format_response = NULL;
                size_t format_length = 0;
                esp_err_t format_err = query(
                    target, target->upstream_ipp_major, target->upstream_ipp_minor,
                    target->upstream_ipp_major >= 2, start,
                    &format_response, &format_length);
                uint16_t format_status = ipp_codec_message_code(format_response,
                                                                 format_length);
                if (format_err == ESP_OK && format_status < 0x0400) {
                    ipp_codec_result_t format_codec =
                        ipp_codec_apply_printer_attributes(format_response,
                                                           format_length, target);
                    if (format_codec != IPP_CODEC_OK) {
                        ESP_LOGW(TAG, "%s returned malformed %s capabilities",
                                 target->instance, start);
                    }
                } else {
                    ESP_LOGD(TAG, "%s rejected format-specific query for %s",
                             target->instance, start);
                }
                free(format_response);
                ++queried;
            }
            *end = saved_end;
            *cursor = saved;
            if (*cursor) {
                ++cursor;
            }
        }
    }
    ipp_codec_finalize_profile(target);
    ESP_LOGI(TAG, "%s: IPP %u.%u, formats=%s, URF=%s", target->instance,
             target->upstream_ipp_major, target->upstream_ipp_minor,
             target->pdl, target->urf);
    return ESP_OK;
}

esp_err_t printer_capabilities_refresh_selected(bool force,
                                                printer_target_t *refreshed,
                                                bool *changed)
{
    if (changed) {
        *changed = false;
    }
    if (!s_refresh_lock) {
        s_refresh_lock = xSemaphoreCreateMutex();
        if (!s_refresh_lock) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (xSemaphoreTake(s_refresh_lock, pdMS_TO_TICKS(5000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    printer_target_t current;
    if (!app_state_get_target(&current)) {
        xSemaphoreGive(s_refresh_lock);
        return ESP_ERR_NOT_FOUND;
    }
    char endpoint[sizeof(s_last_endpoint)];
    snprintf(endpoint, sizeof(endpoint), "%s:%u%s", current.address,
             current.port, current.resource_path);
    TickType_t now = xTaskGetTickCount();
    bool same_endpoint = strcmp(endpoint, s_last_endpoint) == 0;
    bool fresh = same_endpoint && s_last_refresh != 0 &&
                 (now - s_last_refresh) < pdMS_TO_TICKS(REFRESH_INTERVAL_MS);
    if (!force && fresh) {
        if (refreshed) {
            *refreshed = current;
        }
        xSemaphoreGive(s_refresh_lock);
        return ESP_OK;
    }

    printer_target_t updated = current;
    esp_err_t err = printer_capabilities_probe(&updated);
    s_last_refresh = now;
    snprintf(s_last_endpoint, sizeof(s_last_endpoint), "%s", endpoint);
    if (err == ESP_OK) {
        bool different = memcmp(&current, &updated, sizeof(current)) != 0;
        if (different) {
            app_state_update_target(&updated);
        }
        if (changed) {
            *changed = different;
        }
        current = updated;
    }
    if (refreshed) {
        *refreshed = current;
    }
    xSemaphoreGive(s_refresh_lock);
    return err;
}
