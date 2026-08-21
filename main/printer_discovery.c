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
#include "printer_advertisement.h"
#include "printer_capabilities.h"
#include "printer_identity.h"

static const char *TAG = "espresso_discovery";
static SemaphoreHandle_t s_lock;
static printer_target_t s_printers[ESPRESSO_PRINTER_MAX];
static size_t s_printer_count;
static bool s_mdns_ready;
static bool s_ipp_advertised;
static bool s_ipps_advertised;
static bool s_everywhere_advertised;
static char s_service_instance[64];
static printer_advertisement_t s_advertisement;
/* Schema-4 profiles and their DNS-SD projection do not fit together on the
 * 3.5 KiB ESP main-task stack. Access is serialized by s_lock. */
static printer_target_t s_advertisement_target;
static printer_advertisement_t s_pending_advertisement;
static char s_advertisement_uuid[ESPRESSO_UUID_MAX];
static printer_txt_item_t s_generic_txt[ESPRESSO_DNSSD_TXT_MAX];
static mdns_txt_item_t s_mdns_txt[ESPRESSO_DNSSD_TXT_MAX];

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

static bool safe_web_url(const char *value)
{
    if (!value || (strncmp(value, "http://", 7) != 0 &&
                   strncmp(value, "https://", 8) != 0)) {
        return false;
    }
    for (const unsigned char *cursor = (const unsigned char *)value;
         *cursor; ++cursor) {
        if (*cursor < 0x20 || *cursor == 0x7f) {
            return false;
        }
    }
    return true;
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

static void target_from_result(const mdns_result_t *result, bool secure,
                               printer_target_t *target)
{
    memset(target, 0, sizeof(*target));
    target->profile_schema = ESPRESSO_PROFILE_SCHEMA;
    target->secure_transport = secure;
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
    char admin_url[ESPRESSO_ADMIN_URL_MAX];
    if (txt_get(result, "adminurl", admin_url, sizeof(admin_url)) &&
        safe_web_url(admin_url)) {
        snprintf(target->admin_url, sizeof(target->admin_url), "%s", admin_url);
    }
    txt_get(result, "printer-state", flag, sizeof(flag));
    unsigned long state = strtoul(flag, NULL, 10);
    if (state >= 3 && state <= 5) {
        target->printer_state = (uint8_t)state;
    }
    ipp_codec_finalize_profile(target);
}

static void collect_results(mdns_result_t *results, bool secure,
                            printer_target_t *found, size_t *count)
{
    for (mdns_result_t *result = results;
         result && *count < ESPRESSO_PRINTER_MAX; result = result->next) {
        if (result->hostname && strcasecmp(result->hostname, "espresso") == 0) {
            continue;
        }
        printer_target_t target;
        target_from_result(result, secure, &target);
        esp_err_t probe_err = printer_capabilities_probe(&target);
        if (probe_err != ESP_OK) {
            ESP_LOGW(TAG, "using DNS-SD-only capabilities for %s",
                     target.instance);
        }
        if (target.pdl[0] == '\0' && target.urf[0] != '\0') {
            snprintf(target.pdl, sizeof(target.pdl), "image/urf");
        }
        if (target.address[0] == '\0' || target.port == 0 ||
            !strstr(target.pdl, "image/urf") || target.urf[0] == '\0') {
            continue;
        }
        bool duplicate = false;
        for (size_t i = 0; i < *count; ++i) {
            bool same_service =
                target.hostname[0] && found[i].hostname[0] &&
                strcasecmp(found[i].hostname, target.hostname) == 0 &&
                strcmp(found[i].resource_path, target.resource_path) == 0;
            bool same_endpoint =
                strcmp(found[i].address, target.address) == 0 &&
                found[i].port == target.port &&
                strcmp(found[i].resource_path, target.resource_path) == 0;
            if (same_service || same_endpoint) {
                if (secure && !found[i].secure_transport) {
                    found[i] = target;
                }
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            found[(*count)++] = target;
        }
    }
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
    refresh_saved_address();
    esp_err_t err = printer_discovery_advertise_selected();
    if (err == ESP_OK) {
        s_mdns_ready = true;
    }
    return err;
}

esp_err_t printer_discovery_scan(void)
{
    if (!s_mdns_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    mdns_result_t *ipp_results = NULL;
    mdns_result_t *ipps_results = NULL;
    esp_err_t ipp_err = mdns_query_ptr("_ipp", "_tcp", 4000,
                                       ESPRESSO_PRINTER_MAX + 2,
                                       &ipp_results);
    esp_err_t ipps_err = mdns_query_ptr("_ipps", "_tcp", 4000,
                                        ESPRESSO_PRINTER_MAX + 2,
                                        &ipps_results);
    if (ipp_err != ESP_OK && ipps_err != ESP_OK) {
        return ipp_err;
    }

    printer_target_t *found = calloc(ESPRESSO_PRINTER_MAX, sizeof(*found));
    if (!found) {
        mdns_query_results_free(ipp_results);
        mdns_query_results_free(ipps_results);
        return ESP_ERR_NO_MEM;
    }
    size_t count = 0;
    collect_results(ipp_results, false, found, &count);
    collect_results(ipps_results, true, found, &count);
    mdns_query_results_free(ipp_results);
    mdns_query_results_free(ipps_results);

    /* A scan also refreshes the persisted profile for the selected endpoint.
     * This keeps the selection stable while picking up newly advertised
     * metadata such as adminurl, location, and changing capabilities. */
    bool selected_changed = false;
    printer_target_t selected;
    if (app_state_get_target(&selected)) {
        for (size_t i = 0; i < count; ++i) {
            if (strcmp(found[i].address, selected.address) == 0 &&
                found[i].port == selected.port &&
                strcmp(found[i].resource_path, selected.resource_path) == 0) {
                if (memcmp(&found[i], &selected, sizeof(selected)) != 0) {
                    esp_err_t save_err = app_state_set_target(&found[i]);
                    if (save_err == ESP_OK) {
                        selected_changed = true;
                    } else {
                        ESP_LOGW(TAG, "could not refresh selected profile: %s",
                                 esp_err_to_name(save_err));
                    }
                }
                break;
            }
        }
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(s_printers, found, sizeof(s_printers));
    s_printer_count = count;
    xSemaphoreGive(s_lock);
    free(found);
    if (selected_changed) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(printer_discovery_advertise_selected());
    }
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
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    if (s_ipp_advertised) {
        err = mdns_service_remove(ESPRESSO_DNSSD_SERVICE,
                                  ESPRESSO_DNSSD_PROTOCOL);
        if (err == ESP_OK || err == ESP_ERR_NOT_FOUND) {
            s_ipp_advertised = false;
            s_service_instance[0] = '\0';
            memset(&s_advertisement, 0, sizeof(s_advertisement));
            err = ESP_OK;
        }
    }
    if (s_ipps_advertised) {
        esp_err_t secure_err = mdns_service_remove(
            ESPRESSO_DNSSD_SECURE_SERVICE, ESPRESSO_DNSSD_PROTOCOL);
        if (secure_err == ESP_OK || secure_err == ESP_ERR_NOT_FOUND) {
            s_ipps_advertised = false;
            s_everywhere_advertised = false;
        } else if (err == ESP_OK) {
            err = secure_err;
        }
    }
    xSemaphoreGive(s_lock);
    return err;
}

static esp_err_t advertise_selected_locked(void)
{
    if (!app_state_get_target(&s_advertisement_target)) {
        return ESP_OK;
    }

    printer_identity_target_uuid(&s_advertisement_target,
                                 s_advertisement_uuid,
                                 sizeof(s_advertisement_uuid));
    char custom_name[ESPRESSO_PRINTER_NAME_MAX];
    app_state_get_printer_name(custom_name, sizeof(custom_name));
    printer_advertisement_build(&s_advertisement_target,
                                s_advertisement_uuid,
                                custom_name,
                                &s_pending_advertisement);
    size_t txt_count = printer_advertisement_txt(
        &s_pending_advertisement, s_generic_txt, ESPRESSO_DNSSD_TXT_MAX);
    for (size_t i = 0; i < txt_count; ++i) {
        s_mdns_txt[i].key = s_generic_txt[i].key;
        s_mdns_txt[i].value = s_generic_txt[i].value;
    }

    /* printer_discovery_init() installs the service before Wi-Fi has an IP,
     * then printer_discovery_network_ready() refreshes it. Avoid removing and
     * immediately re-adding an unchanged service: doing that while mDNS has
     * scheduled answers can corrupt the component's service-answer queue. */
    if (s_ipp_advertised && s_ipps_advertised &&
        memcmp(&s_advertisement, &s_pending_advertisement,
               sizeof(s_pending_advertisement)) == 0) {
        ESP_LOGI(TAG, "AirPrint facade already advertised for %s",
                 s_advertisement_target.instance);
        return ESP_OK;
    }

    if (s_ipp_advertised) {
        if (strcmp(s_service_instance, s_pending_advertisement.instance) != 0) {
            ESP_RETURN_ON_ERROR(mdns_service_instance_name_set(
                                    ESPRESSO_DNSSD_SERVICE,
                                    ESPRESSO_DNSSD_PROTOCOL,
                                s_pending_advertisement.instance),
                                TAG, "IPP instance update failed");
            ESP_RETURN_ON_ERROR(mdns_service_instance_name_set(
                                    ESPRESSO_DNSSD_SECURE_SERVICE,
                                    ESPRESSO_DNSSD_PROTOCOL,
                                    s_pending_advertisement.instance),
                                TAG, "IPPS instance update failed");
        }
        ESP_RETURN_ON_ERROR(mdns_service_txt_set(ESPRESSO_DNSSD_SERVICE,
                                                 ESPRESSO_DNSSD_PROTOCOL,
                                                 s_mdns_txt, txt_count),
                            TAG, "IPP TXT update failed");
        size_t secure_count = printer_advertisement_ipps_txt(
            &s_pending_advertisement, s_generic_txt, ESPRESSO_DNSSD_TXT_MAX);
        for (size_t i = 0; i < secure_count; ++i) {
            s_mdns_txt[i].key = s_generic_txt[i].key;
            s_mdns_txt[i].value = s_generic_txt[i].value;
        }
        ESP_RETURN_ON_ERROR(mdns_service_txt_set(
                                ESPRESSO_DNSSD_SECURE_SERVICE,
                                ESPRESSO_DNSSD_PROTOCOL, s_mdns_txt,
                                secure_count),
                            TAG, "IPPS TXT update failed");
        if (s_everywhere_advertised !=
            s_pending_advertisement.ipp_everywhere) {
            esp_err_t (*change_subtype)(const char *, const char *,
                                        const char *, const char *,
                                        const char *) =
                s_pending_advertisement.ipp_everywhere ?
                    mdns_service_subtype_add_for_host :
                    mdns_service_subtype_remove_for_host;
            ESP_RETURN_ON_ERROR(change_subtype(
                                    s_pending_advertisement.instance,
                                    ESPRESSO_DNSSD_SERVICE,
                                    ESPRESSO_DNSSD_PROTOCOL, NULL,
                                    ESPRESSO_DNSSD_PRINT_SUBTYPE),
                                TAG, "IPP Everywhere subtype update failed");
            ESP_RETURN_ON_ERROR(change_subtype(
                                    s_pending_advertisement.instance,
                                    ESPRESSO_DNSSD_SECURE_SERVICE,
                                    ESPRESSO_DNSSD_PROTOCOL, NULL,
                                    ESPRESSO_DNSSD_PRINT_SUBTYPE),
                                TAG, "secure subtype update failed");
            s_everywhere_advertised =
                s_pending_advertisement.ipp_everywhere;
        }
        s_advertisement = s_pending_advertisement;
        snprintf(s_service_instance, sizeof(s_service_instance), "%s",
                 s_pending_advertisement.instance);
        ESP_LOGI(TAG, "updated modern AirPrint facade for %s",
                 s_advertisement_target.instance);
        return ESP_OK;
    }

    snprintf(s_service_instance, sizeof(s_service_instance), "%s",
             s_pending_advertisement.instance);
    ESP_RETURN_ON_ERROR(mdns_service_add(s_service_instance,
                                         ESPRESSO_DNSSD_SERVICE,
                                         ESPRESSO_DNSSD_PROTOCOL, 631,
                                         s_mdns_txt, txt_count),
                        TAG, "IPP advertisement failed");
    ESP_RETURN_ON_ERROR(mdns_service_subtype_add_for_host(
                            s_service_instance, ESPRESSO_DNSSD_SERVICE,
                            ESPRESSO_DNSSD_PROTOCOL, NULL,
                            ESPRESSO_DNSSD_SUBTYPE),
                        TAG, "AirPrint subtype failed");
    if (s_pending_advertisement.ipp_everywhere) {
        ESP_RETURN_ON_ERROR(mdns_service_subtype_add_for_host(
                                s_service_instance, ESPRESSO_DNSSD_SERVICE,
                                ESPRESSO_DNSSD_PROTOCOL, NULL,
                                ESPRESSO_DNSSD_PRINT_SUBTYPE),
                            TAG, "IPP Everywhere subtype failed");
    }
    size_t secure_count = printer_advertisement_ipps_txt(
        &s_pending_advertisement, s_generic_txt, ESPRESSO_DNSSD_TXT_MAX);
    for (size_t i = 0; i < secure_count; ++i) {
        s_mdns_txt[i].key = s_generic_txt[i].key;
        s_mdns_txt[i].value = s_generic_txt[i].value;
    }
    ESP_RETURN_ON_ERROR(mdns_service_add(
                            s_service_instance,
                            ESPRESSO_DNSSD_SECURE_SERVICE,
                            ESPRESSO_DNSSD_PROTOCOL, 8631, s_mdns_txt,
                            secure_count),
                        TAG, "IPPS advertisement failed");
    if (s_pending_advertisement.ipp_everywhere) {
        ESP_RETURN_ON_ERROR(mdns_service_subtype_add_for_host(
                                s_service_instance,
                                ESPRESSO_DNSSD_SECURE_SERVICE,
                                ESPRESSO_DNSSD_PROTOCOL, NULL,
                                ESPRESSO_DNSSD_PRINT_SUBTYPE),
                            TAG, "secure IPP Everywhere subtype failed");
    }
    s_advertisement = s_pending_advertisement;
    s_ipp_advertised = true;
    s_ipps_advertised = true;
    s_everywhere_advertised = s_pending_advertisement.ipp_everywhere;
    ESP_LOGI(TAG, "advertising modern AirPrint facade for %s",
             s_advertisement_target.instance);
    return ESP_OK;
}

esp_err_t printer_discovery_advertise_selected(void)
{
    if (!s_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = advertise_selected_locked();
    xSemaphoreGive(s_lock);
    return err;
}

void printer_discovery_advertised_name(char *name, size_t name_size)
{
    if (!name || name_size == 0) {
        return;
    }
    name[0] = '\0';
    if (!s_lock) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(name, name_size, "%s", s_service_instance);
    xSemaphoreGive(s_lock);
}

void printer_discovery_network_ready(void)
{
    if (!s_mdns_ready) {
        return;
    }
    refresh_saved_address();
    ESP_ERROR_CHECK_WITHOUT_ABORT(printer_discovery_advertise_selected());
}
