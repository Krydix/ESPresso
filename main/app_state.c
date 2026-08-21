#include "app_state.h"

#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "ipp_codec.h"
#include "nvs.h"

static const char *TAG = "espresso_state";
static SemaphoreHandle_t s_lock;
static bool s_wifi_connected;
static char s_wifi_ssid[33];
static char s_wifi_ip[ESPRESSO_ADDRESS_MAX];
static bool s_has_target;
static printer_target_t s_target;
static char s_printer_name[ESPRESSO_PRINTER_NAME_MAX];

static void copy_string(char *destination, size_t size, const char *source)
{
    if (size == 0) {
        return;
    }
    snprintf(destination, size, "%s", source ? source : "");
}

static bool ascii_whitespace(char value)
{
    return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

static esp_err_t normalize_printer_name(const char *name, char *normalized,
                                        size_t normalized_size)
{
    if (!name || !normalized || normalized_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    while (ascii_whitespace(*name)) {
        ++name;
    }
    size_t length = strlen(name);
    while (length > 0 && ascii_whitespace(name[length - 1])) {
        --length;
    }
    if (length >= normalized_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    for (size_t i = 0; i < length; ++i) {
        unsigned char value = (unsigned char)name[i];
        if (value < 0x20 || value == 0x7f) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    memcpy(normalized, name, length);
    normalized[length] = '\0';
    return ESP_OK;
}

esp_err_t app_state_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open("espresso", NVS_READWRITE, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "no persisted printer selection");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not open persisted state: %s",
                 esp_err_to_name(err));
        return err;
    }

    memset(&s_target, 0, sizeof(s_target));
    size_t size = sizeof(s_target);
    esp_err_t target_err = nvs_get_blob(nvs, "printer", &s_target, &size);
    size_t name_size = sizeof(s_printer_name);
    esp_err_t name_err = nvs_get_str(nvs, "printer_name", s_printer_name,
                                     &name_size);

    /* Schema 5 only appends admin_url. Preserve schema-4 selections across
     * OTA and leave the new field empty until the next capability refresh. */
    const size_t alignment = _Alignof(printer_target_t);
    const size_t legacy_size =
        (offsetof(printer_target_t, admin_url) + alignment - 1) & ~(alignment - 1);
    bool migrated_target = target_err == ESP_OK && size == legacy_size &&
                           s_target.profile_schema == 4;
    if (migrated_target) {
        memset(s_target.admin_url, 0, sizeof(s_target.admin_url));
        s_target.profile_schema = ESPRESSO_PROFILE_SCHEMA;
        target_err = nvs_set_blob(nvs, "printer", &s_target, sizeof(s_target));
        if (target_err == ESP_OK) {
            target_err = nvs_commit(nvs);
        }
        if (target_err == ESP_OK) {
            size = sizeof(s_target);
            ESP_LOGI(TAG, "migrated persisted printer profile from schema 4");
        }
    }
    nvs_close(nvs);

    if (name_err == ESP_OK) {
        char normalized[ESPRESSO_PRINTER_NAME_MAX];
        if (normalize_printer_name(s_printer_name, normalized,
                                   sizeof(normalized)) == ESP_OK &&
            normalized[0] != '\0') {
            copy_string(s_printer_name, sizeof(s_printer_name), normalized);
            ESP_LOGI(TAG, "restored AirPrint name '%s'", s_printer_name);
        } else {
            ESP_LOGW(TAG, "discarding invalid persisted AirPrint name");
            s_printer_name[0] = '\0';
        }
    } else if (name_err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "could not restore AirPrint name: %s",
                 esp_err_to_name(name_err));
        s_printer_name[0] = '\0';
    }

    if (target_err == ESP_OK && size == sizeof(s_target) &&
        s_target.profile_schema == ESPRESSO_PROFILE_SCHEMA && s_target.port != 0 &&
        s_target.address[0] != '\0') {
        ipp_codec_finalize_profile(&s_target);
        s_has_target = true;
        ESP_LOGI(TAG, "restored printer selection '%s' at %s:%u%s",
                 s_target.instance, s_target.address, s_target.port,
                 s_target.resource_path);
        return ESP_OK;
    }
    uint32_t stored_schema = s_target.profile_schema;
    memset(&s_target, 0, sizeof(s_target));
    if (target_err == ESP_ERR_NVS_NOT_FOUND ||
        target_err == ESP_ERR_NVS_INVALID_LENGTH ||
        (target_err == ESP_OK && size != sizeof(s_target))) {
        ESP_LOGW(TAG,
                 "discarding incompatible printer selection: read=%s size=%u expected=%u",
                 esp_err_to_name(target_err), (unsigned)size,
                 (unsigned)sizeof(s_target));
        return ESP_OK;
    }
    if (target_err == ESP_OK) {
        ESP_LOGW(TAG,
                 "discarding invalid printer selection: schema=%" PRIu32
                 " expected=%u",
                 stored_schema, (unsigned)ESPRESSO_PROFILE_SCHEMA);
        return ESP_OK;
    }
    ESP_LOGE(TAG, "could not read persisted printer selection: %s",
             esp_err_to_name(target_err));
    return target_err;
}

void app_state_set_wifi(bool connected, const char *ssid, const char *ip_address)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_wifi_connected = connected;
    copy_string(s_wifi_ssid, sizeof(s_wifi_ssid), ssid);
    copy_string(s_wifi_ip, sizeof(s_wifi_ip), ip_address);
    xSemaphoreGive(s_lock);
}

void app_state_get_wifi(bool *connected, char *ssid, size_t ssid_size,
                        char *ip_address, size_t ip_size)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (connected) {
        *connected = s_wifi_connected;
    }
    copy_string(ssid, ssid_size, s_wifi_ssid);
    copy_string(ip_address, ip_size, s_wifi_ip);
    xSemaphoreGive(s_lock);
}

esp_err_t app_state_set_target(const printer_target_t *target)
{
    if (!target || target->port == 0 || target->address[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open("espresso", NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(nvs, "printer", target, sizeof(*target));
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (err == ESP_OK) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_target = *target;
        s_has_target = true;
        xSemaphoreGive(s_lock);
        ESP_LOGI(TAG, "persisted printer selection '%s' at %s:%u%s",
                 target->instance, target->address, target->port,
                 target->resource_path);
    } else {
        ESP_LOGE(TAG, "could not persist printer selection: %s",
                 esp_err_to_name(err));
    }
    return err;
}

esp_err_t app_state_update_target(const printer_target_t *target)
{
    if (!target || target->port == 0 || target->address[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_target = *target;
    s_has_target = true;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "updated in-memory printer profile '%s'", target->instance);
    return ESP_OK;
}

bool app_state_get_target(printer_target_t *target)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool present = s_has_target;
    if (present && target) {
        *target = s_target;
    }
    xSemaphoreGive(s_lock);
    return present;
}

esp_err_t app_state_clear_target(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("espresso", NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_key(nvs, "printer");
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (err == ESP_OK) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        memset(&s_target, 0, sizeof(s_target));
        s_has_target = false;
        xSemaphoreGive(s_lock);
        ESP_LOGW(TAG, "printer selection explicitly cleared");
    }
    return err;
}

esp_err_t app_state_set_printer_name(const char *name)
{
    char normalized[ESPRESSO_PRINTER_NAME_MAX];
    esp_err_t err = normalize_printer_name(name, normalized,
                                           sizeof(normalized));
    if (err != ESP_OK) {
        return err;
    }

    nvs_handle_t nvs;
    err = nvs_open("espresso", NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    if (normalized[0]) {
        err = nvs_set_str(nvs, "printer_name", normalized);
    } else {
        err = nvs_erase_key(nvs, "printer_name");
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (err == ESP_OK) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        copy_string(s_printer_name, sizeof(s_printer_name), normalized);
        xSemaphoreGive(s_lock);
        if (normalized[0]) {
            ESP_LOGI(TAG, "saved AirPrint name '%s'", normalized);
        } else {
            ESP_LOGI(TAG, "restored generated AirPrint name");
        }
    }
    return err;
}

void app_state_get_printer_name(char *name, size_t name_size)
{
    if (!name || name_size == 0) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    copy_string(name, name_size, s_printer_name);
    xSemaphoreGive(s_lock);
}
