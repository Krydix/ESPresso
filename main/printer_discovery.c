#include "printer_discovery.h"

#include <strings.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/inet.h"
#include "mdns.h"
#include "ipp_codec.h"
#include "printer_capabilities.h"
#include "printer_identity.h"

static const char *TAG = "espresso_discovery";
static SemaphoreHandle_t s_lock;
static printer_target_t s_printers[ESPRESSO_PRINTER_MAX];
static size_t s_printer_count;
static bool s_mdns_ready;
static char s_service_instance[64];

static void copy_text(char *destination, size_t destination_size,
                      const char *source, size_t source_length)
{
    if (destination_size == 0) {
        return;
    }
    if (!source) {
        destination[0] = '\0';
        return;
    }
    size_t length = source_length;
    if (length >= destination_size) {
        length = destination_size - 1;
    }
    memcpy(destination, source, length);
    destination[length] = '\0';
}

static bool txt_get(const mdns_result_t *result, const char *key,
                    char *value, size_t value_size)
{
    for (size_t i = 0; i < result->txt_count; ++i) {
        if (result->txt[i].key && strcasecmp(result->txt[i].key, key) == 0) {
            if (!result->txt[i].value) {
                copy_text(value, value_size, "", 0);
                return true;
            }
            size_t length = result->txt_value_len ? result->txt_value_len[i] :
                                                    strlen(result->txt[i].value);
            copy_text(value, value_size, result->txt[i].value, length);
            return true;
        }
    }
    if (value_size) {
        value[0] = '\0';
    }
    return false;
}

static bool text_is_true(const char *value)
{
    return value && (strcasecmp(value, "T") == 0 || strcasecmp(value, "true") == 0 ||
                     strcmp(value, "1") == 0);
}

static void refresh_saved_address(void)
{
    printer_target_t target;
    if (!app_state_get_target(&target) || !target.hostname[0]) {
        return;
    }
    char hostname[ESPRESSO_HOST_MAX];
    snprintf(hostname, sizeof(hostname), "%s", target.hostname);
    char *domain = strchr(hostname, '.');
    if (domain) {
        *domain = '\0';
    }
    esp_ip4_addr_t address;
    if (hostname[0] && mdns_query_a(hostname, 1500, &address) == ESP_OK) {
        char resolved[ESPRESSO_ADDRESS_MAX];
        snprintf(resolved, sizeof(resolved), IPSTR, IP2STR(&address));
        if (strcmp(resolved, target.address) != 0) {
            ESP_LOGI(TAG, "%s moved from %s to %s", target.instance,
                     target.address, resolved);
            snprintf(target.address, sizeof(target.address), "%s", resolved);
            ESP_ERROR_CHECK_WITHOUT_ABORT(app_state_set_target(&target));
        }
    } else if (hostname[0]) {
        esp_ip6_addr_t address6;
        if (mdns_query_aaaa(hostname, 1500, &address6) == ESP_OK) {
            char resolved[ESPRESSO_ADDRESS_MAX];
            snprintf(resolved, sizeof(resolved), IPV6STR, IPV62STR(address6));
            if (strcmp(resolved, target.address) != 0) {
                ESP_LOGI(TAG, "%s moved from %s to %s", target.instance,
                         target.address, resolved);
                snprintf(target.address, sizeof(target.address), "%s", resolved);
                ESP_ERROR_CHECK_WITHOUT_ABORT(app_state_set_target(&target));
            }
        }
    }
}

static void target_from_result(const mdns_result_t *result, printer_target_t *target)
{
    memset(target, 0, sizeof(*target));
    target->profile_schema = ESPRESSO_PROFILE_SCHEMA;
    snprintf(target->instance, sizeof(target->instance), "%s",
             result->instance_name ? result->instance_name : "IPP printer");
    snprintf(target->hostname, sizeof(target->hostname), "%s",
             result->hostname ? result->hostname : "");
    target->port = result->port ? result->port : 631;

    for (mdns_ip_addr_t *address = result->addr; address; address = address->next) {
        if (address->addr.type == ESP_IPADDR_TYPE_V4) {
            snprintf(target->address, sizeof(target->address), IPSTR,
                     IP2STR(&address->addr.u_addr.ip4));
            break;
        }
    }
    if (target->address[0] == '\0') {
        for (mdns_ip_addr_t *address = result->addr; address;
             address = address->next) {
            if (address->addr.type == ESP_IPADDR_TYPE_V6) {
                snprintf(target->address, sizeof(target->address), IPV6STR,
                         IPV62STR(address->addr.u_addr.ip6));
                break;
            }
        }
    }
    char path[ESPRESSO_PATH_MAX] = {0};
    txt_get(result, "rp", path, sizeof(path));
    if (path[0] == '\0') {
        snprintf(target->resource_path, sizeof(target->resource_path), "/ipp/print");
    } else if (path[0] == '/') {
        copy_text(target->resource_path, sizeof(target->resource_path), path, strlen(path));
    } else {
        target->resource_path[0] = '/';
        copy_text(target->resource_path + 1, sizeof(target->resource_path) - 1,
                  path, strlen(path));
    }
    txt_get(result, "pdl", target->pdl, sizeof(target->pdl));
    txt_get(result, "URF", target->urf, sizeof(target->urf));
    if (!txt_get(result, "ty", target->label, sizeof(target->label))) {
        snprintf(target->label, sizeof(target->label), "%s", target->instance);
    }
    txt_get(result, "UUID", target->uuid, sizeof(target->uuid));
    char flag[12];
    txt_get(result, "Color", flag, sizeof(flag));
    target->color = text_is_true(flag);
    txt_get(result, "Duplex", flag, sizeof(flag));
    target->duplex = text_is_true(flag);
    txt_get(result, "Copies", flag, sizeof(flag));
    target->copies = text_is_true(flag);
    txt_get(result, "Collate", flag, sizeof(flag));
    target->collate = text_is_true(flag);
    txt_get(result, "note", target->location, sizeof(target->location));
    txt_get(result, "printer-state", flag, sizeof(flag));
    unsigned long state = strtoul(flag, NULL, 10);
    if (state >= 3 && state <= 5) {
        target->printer_state = (uint8_t)state;
    }
    ipp_codec_finalize_profile(target);
}

esp_err_t printer_discovery_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }
    ESP_RETURN_ON_ERROR(mdns_init(), TAG, "mDNS init failed");
    ESP_RETURN_ON_ERROR(mdns_hostname_set("espresso"), TAG, "mDNS hostname failed");
    ESP_RETURN_ON_ERROR(mdns_instance_name_set("ESPresso print bridge"), TAG,
                        "mDNS instance failed");
    mdns_txt_item_t http_txt[] = {{"path", "/"}};
    ESP_RETURN_ON_ERROR(mdns_service_add("ESPresso setup", "_http", "_tcp", 80,
                                         http_txt, 1),
                        TAG, "HTTP service failed");
    s_mdns_ready = true;
    refresh_saved_address();
    return printer_discovery_advertise_selected();
}

esp_err_t printer_discovery_scan(void)
{
    if (!s_mdns_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    mdns_result_t *results = NULL;
    esp_err_t err = mdns_query_ptr("_ipp", "_tcp", 4000, ESPRESSO_PRINTER_MAX + 2,
                                   &results);
    if (err != ESP_OK) {
        return err;
    }

    printer_target_t *found = calloc(ESPRESSO_PRINTER_MAX, sizeof(*found));
    if (!found) {
        mdns_query_results_free(results);
        return ESP_ERR_NO_MEM;
    }
    size_t count = 0;
    for (mdns_result_t *result = results; result && count < ESPRESSO_PRINTER_MAX;
         result = result->next) {
        if (result->hostname && strcasecmp(result->hostname, "espresso") == 0) {
            continue;
        }
        printer_target_t target;
        target_from_result(result, &target);
        /* CUPS resolves DNS-SD first, then treats IPP as the capability source. */
        esp_err_t probe_err = printer_capabilities_probe(&target);
        if (probe_err != ESP_OK) {
            ESP_LOGW(TAG, "using DNS-SD-only capabilities for %s", target.instance);
        }
        if (target.pdl[0] == '\0' && target.urf[0] != '\0') {
            snprintf(target.pdl, sizeof(target.pdl), "image/urf");
        }
        if (target.address[0] == '\0' || target.port == 0 ||
            !strstr(target.pdl, "image/urf") || target.urf[0] == '\0') {
            continue;
        }
        bool duplicate = false;
        for (size_t i = 0; i < count; ++i) {
            if (strcmp(found[i].address, target.address) == 0 &&
                found[i].port == target.port &&
                strcmp(found[i].resource_path, target.resource_path) == 0) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            found[count++] = target;
        }
    }
    mdns_query_results_free(results);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(s_printers, found, sizeof(s_printers));
    s_printer_count = count;
    xSemaphoreGive(s_lock);
    free(found);
    ESP_LOGI(TAG, "found %u IPP printer(s)", (unsigned)count);
    return ESP_OK;
}

size_t printer_discovery_count(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t count = s_printer_count;
    xSemaphoreGive(s_lock);
    return count;
}

bool printer_discovery_get(size_t index, printer_target_t *target)
{
    if (!target) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool valid = index < s_printer_count;
    if (valid) {
        *target = s_printers[index];
    }
    xSemaphoreGive(s_lock);
    return valid;
}

esp_err_t printer_discovery_select(size_t index)
{
    printer_target_t target;
    if (!printer_discovery_get(index, &target)) {
        return ESP_ERR_NOT_FOUND;
    }
    ESP_RETURN_ON_ERROR(app_state_set_target(&target), TAG, "printer save failed");
    return printer_discovery_advertise_selected();
}

esp_err_t printer_discovery_clear_selection(void)
{
    ESP_RETURN_ON_ERROR(app_state_clear_target(), TAG, "printer clear failed");
    if (mdns_service_exists("_ipp", "_tcp", NULL)) {
        return mdns_service_remove("_ipp", "_tcp");
    }
    return ESP_OK;
}

esp_err_t printer_discovery_advertise_selected(void)
{
    printer_target_t target;
    if (!app_state_get_target(&target)) {
        return ESP_OK;
    }
    if (mdns_service_exists("_ipp", "_tcp", NULL)) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(mdns_service_remove("_ipp", "_tcp"));
    }

    char uuid[ESPRESSO_UUID_MAX];
    printer_identity_uuid(uuid, sizeof(uuid));
    snprintf(s_service_instance, sizeof(s_service_instance), "ESPresso - %.51s",
             target.label[0] ? target.label : target.instance);
    char admin_url[] = "http://espresso.local/";
    char product[ESPRESSO_LABEL_MAX + 3];
    snprintf(product, sizeof(product), "(%s)",
             target.label[0] ? target.label : "Legacy AirPrint printer");
    char color[] = "F";
    char duplex[] = "F";
    char copies[] = "F";
    char collate[] = "F";
    char printer_state[4];
    snprintf(printer_state, sizeof(printer_state), "%u",
             target.printer_state ? target.printer_state : 3);
    if (target.color) {
        color[0] = 'T';
    }
    if (target.duplex) {
        duplex[0] = 'T';
    }
    if (target.copies) {
        copies[0] = 'T';
    }
    if (target.collate) {
        collate[0] = 'T';
    }
    const char *note = target.location[0] ? target.location :
                                            "Legacy printer bridged by ESPresso";
    char pdl[252];
    snprintf(pdl, sizeof(pdl), "%.251s", target.pdl);
    const char *make_model = target.label[0] ? target.label : target.instance;
    mdns_txt_item_t txt[] = {
        {"txtvers", "1"},
        {"qtotal", "1"},
        {"rp", "ipp/print"},
        {"ty", make_model},
        {"product", product},
        {"pdl", pdl},
        {"URF", target.urf},
        {"Color", color},
        {"Duplex", duplex},
        {"Copies", copies},
        {"Collate", collate},
        {"Transparent", "T"},
        {"Binary", "T"},
        {"printer-state", printer_state},
        {"kind", "document"},
        {"priority", "0"},
        {"adminurl", admin_url},
        {"UUID", uuid},
        {"note", note},
    };
    ESP_RETURN_ON_ERROR(mdns_service_add(s_service_instance, "_ipp", "_tcp", 631,
                                         txt, sizeof(txt) / sizeof(txt[0])),
                        TAG, "IPP advertisement failed");
    ESP_RETURN_ON_ERROR(mdns_service_subtype_add_for_host(
                            s_service_instance, "_ipp", "_tcp", NULL, "_universal"),
                        TAG, "AirPrint subtype failed");
    ESP_LOGI(TAG, "advertising modern AirPrint facade for %s", target.instance);
    return ESP_OK;
}

void printer_discovery_network_ready(void)
{
    if (!s_mdns_ready) {
        return;
    }
    refresh_saved_address();
}
