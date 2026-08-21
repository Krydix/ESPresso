#include "web_server.h"

#include <stdlib.h>
#include <string.h>

#include "app_state.h"
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "printer_discovery.h"
#include "wifi_manager.h"

extern const char frontend_index_start[] asm("_binary_index_html_start");
extern const char frontend_index_end[] asm("_binary_index_html_end");

static const char *TAG = "espresso_web";

static esp_err_t send_json(httpd_req_t *request, cJSON *json)
{
    char *serialized = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (!serialized) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "JSON allocation failed");
    }
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(request, serialized);
    free(serialized);
    return err;
}

static char *read_body(httpd_req_t *request, size_t maximum)
{
    if (request->content_len <= 0 || (size_t)request->content_len > maximum) {
        return NULL;
    }
    char *body = malloc((size_t)request->content_len + 1);
    if (!body) {
        return NULL;
    }
    size_t received = 0;
    while (received < (size_t)request->content_len) {
        int result = httpd_req_recv(request, body + received,
                                    (size_t)request->content_len - received);
        if (result <= 0) {
            free(body);
            return NULL;
        }
        received += (size_t)result;
    }
    body[received] = '\0';
    return body;
}

static esp_err_t root_handler(httpd_req_t *request)
{
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, frontend_index_start,
                           frontend_index_end - frontend_index_start);
}

static esp_err_t status_handler(httpd_req_t *request)
{
    bool connected;
    char ssid[33];
    char ip[ESPRESSO_ADDRESS_MAX];
    app_state_get_wifi(&connected, ssid, sizeof(ssid), ip, sizeof(ip));
    cJSON *root = cJSON_CreateObject();
    cJSON *wifi = cJSON_AddObjectToObject(root, "wifi");
    cJSON_AddBoolToObject(wifi, "connected", connected);
    cJSON_AddStringToObject(wifi, "ssid", ssid);
    cJSON_AddStringToObject(wifi, "ip", ip);
    cJSON_AddBoolToObject(wifi, "portalActive", wifi_manager_portal_active());
    cJSON_AddStringToObject(wifi, "setupSsid", wifi_manager_setup_ssid());

    printer_target_t target;
    bool selected = app_state_get_target(&target);
    cJSON *printer = cJSON_AddObjectToObject(root, "printer");
    cJSON_AddBoolToObject(printer, "selected", selected);
    if (selected) {
        cJSON_AddStringToObject(printer, "name", target.label);
        cJSON_AddStringToObject(printer, "address", target.address);
        cJSON_AddStringToObject(printer, "path", target.resource_path);
        cJSON_AddStringToObject(printer, "pdl", target.pdl);
        cJSON_AddStringToObject(printer, "urf", target.urf);
        cJSON_AddStringToObject(printer, "media", target.media);
        cJSON_AddStringToObject(printer, "upstreamIppVersions", target.ipp_versions);
        cJSON_AddBoolToObject(printer, "capabilityQueried", target.capability_queried);
    }
    const esp_app_desc_t *description = esp_app_get_description();
    cJSON_AddStringToObject(root, "version", description->version);
    cJSON_AddBoolToObject(root, "ready", connected && selected);
    return send_json(request, root);
}

static esp_err_t wifi_scan_handler(httpd_req_t *request)
{
    wifi_scan_entry_t entries[ESPRESSO_WIFI_SCAN_MAX];
    size_t count = 0;
    esp_err_t err = wifi_manager_scan(entries, ESPRESSO_WIFI_SCAN_MAX, &count);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi scan failed: %s", esp_err_to_name(err));
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Wi-Fi scan failed");
    }
    cJSON *root = cJSON_CreateObject();
    cJSON *networks = cJSON_AddArrayToObject(root, "networks");
    for (size_t i = 0; i < count; ++i) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "ssid", entries[i].ssid);
        cJSON_AddNumberToObject(item, "rssi", entries[i].rssi);
        cJSON_AddBoolToObject(item, "secure", entries[i].secure);
        cJSON_AddItemToArray(networks, item);
    }
    return send_json(request, root);
}

static esp_err_t wifi_connect_handler(httpd_req_t *request)
{
    char *body = read_body(request, 512);
    if (!body) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid body");
    }
    cJSON *json = cJSON_Parse(body);
    free(body);
    cJSON *ssid = json ? cJSON_GetObjectItemCaseSensitive(json, "ssid") : NULL;
    cJSON *password = json ? cJSON_GetObjectItemCaseSensitive(json, "password") : NULL;
    if (!cJSON_IsString(ssid) || (password && !cJSON_IsString(password))) {
        cJSON_Delete(json);
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                                   "ssid and password must be strings");
    }
    esp_err_t err = wifi_manager_connect(ssid->valuestring,
                                         password ? password->valuestring : "");
    cJSON_Delete(json);
    if (err != ESP_OK) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                                   "Could not connect with those credentials");
    }
    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "connecting", true);
    cJSON_AddStringToObject(response, "next", "http://espresso.local/");
    return send_json(request, response);
}

static esp_err_t wifi_forget_handler(httpd_req_t *request)
{
    httpd_resp_sendstr(request, "{\"restarting\":true}");
    return wifi_manager_forget();
}

static esp_err_t printers_handler(httpd_req_t *request)
{
    char query[64] = {0};
    bool refresh = httpd_req_get_url_query_str(request, query, sizeof(query)) == ESP_OK &&
                   strstr(query, "refresh=1");
    if (refresh) {
        esp_err_t err = printer_discovery_scan();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "printer scan failed: %s", esp_err_to_name(err));
        }
    }
    cJSON *root = cJSON_CreateObject();
    cJSON *printers = cJSON_AddArrayToObject(root, "printers");
    size_t count = printer_discovery_count();
    for (size_t i = 0; i < count; ++i) {
        printer_target_t target;
        if (!printer_discovery_get(i, &target)) {
            continue;
        }
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "index", i);
        cJSON_AddStringToObject(item, "name", target.label);
        cJSON_AddStringToObject(item, "instance", target.instance);
        cJSON_AddStringToObject(item, "address", target.address);
        cJSON_AddNumberToObject(item, "port", target.port);
        cJSON_AddStringToObject(item, "path", target.resource_path);
        cJSON_AddStringToObject(item, "pdl", target.pdl);
        cJSON_AddStringToObject(item, "urf", target.urf);
        cJSON_AddBoolToObject(item, "color", target.color);
        cJSON_AddBoolToObject(item, "duplex", target.duplex);
        cJSON_AddBoolToObject(item, "copies", target.copies);
        cJSON_AddBoolToObject(item, "collate", target.collate);
        cJSON_AddBoolToObject(item, "capabilityQueried", target.capability_queried);
        cJSON_AddStringToObject(item, "ippVersions", target.ipp_versions);
        cJSON_AddStringToObject(item, "media", target.media);
        cJSON_AddItemToArray(printers, item);
    }
    return send_json(request, root);
}

static esp_err_t printer_select_handler(httpd_req_t *request)
{
    char *body = read_body(request, 128);
    if (!body) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid body");
    }
    cJSON *json = cJSON_Parse(body);
    free(body);
    cJSON *index = json ? cJSON_GetObjectItemCaseSensitive(json, "index") : NULL;
    if (!cJSON_IsNumber(index) || index->valuedouble < 0) {
        cJSON_Delete(json);
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                                   "index must identify a discovered printer");
    }
    esp_err_t err = printer_discovery_select((size_t)index->valuedouble);
    cJSON_Delete(json);
    if (err != ESP_OK) {
        return httpd_resp_send_err(request, HTTPD_404_NOT_FOUND,
                                   "Printer is no longer in the discovery list");
    }
    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "selected", true);
    return send_json(request, response);
}

static esp_err_t printer_clear_handler(httpd_req_t *request)
{
    esp_err_t err = printer_discovery_clear_selection();
    if (err != ESP_OK) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Could not clear printer");
    }
    return httpd_resp_sendstr(request, "{\"selected\":false}");
}

static esp_err_t not_found_handler(httpd_req_t *request, httpd_err_code_t error)
{
    (void)error;
    if (wifi_manager_portal_active()) {
        httpd_resp_set_status(request, "302 Found");
        httpd_resp_set_hdr(request, "Location", "/");
        return httpd_resp_sendstr(request, "Open the ESPresso setup portal");
    }
    return httpd_resp_send_err(request, HTTPD_404_NOT_FOUND, "Not found");
}

esp_err_t web_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_open_sockets = 6;
    config.lru_purge_enable = true;
    config.stack_size = 8192;
    httpd_handle_t server = NULL;
    ESP_RETURN_ON_ERROR(httpd_start(&server, &config), TAG, "web server failed");

    const httpd_uri_t handlers[] = {
        {.uri = "/", .method = HTTP_GET, .handler = root_handler},
        {.uri = "/api/status", .method = HTTP_GET, .handler = status_handler},
        {.uri = "/api/wifi/scan", .method = HTTP_GET, .handler = wifi_scan_handler},
        {.uri = "/api/wifi/connect", .method = HTTP_POST, .handler = wifi_connect_handler},
        {.uri = "/api/wifi", .method = HTTP_DELETE, .handler = wifi_forget_handler},
        {.uri = "/api/printers", .method = HTTP_GET, .handler = printers_handler},
        {.uri = "/api/printer", .method = HTTP_POST, .handler = printer_select_handler},
        {.uri = "/api/printer", .method = HTTP_DELETE, .handler = printer_clear_handler},
    };
    for (size_t i = 0; i < sizeof(handlers) / sizeof(handlers[0]); ++i) {
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &handlers[i]), TAG,
                            "route registration failed");
    }
    return httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, not_found_handler);
}
