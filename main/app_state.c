#include "app_state.h"

#include <inttypes.h>
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

static void copy_string(char *destination, size_t size, const char *source)
{
    if (size == 0) {
        return;
    }
    snprintf(destination, size, "%s", source ? source : "");
}

esp_err_t app_state_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open("espresso", NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "no persisted printer selection");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not open persisted state: %s",
                 esp_err_to_name(err));
        return err;
    }

    size_t size = sizeof(s_target);
    err = nvs_get_blob(nvs, "printer", &s_target, &size);
    nvs_close(nvs);
    if (err == ESP_OK && size == sizeof(s_target) &&
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
    if (err == ESP_ERR_NVS_NOT_FOUND || err == ESP_ERR_NVS_INVALID_LENGTH ||
        (err == ESP_OK && size != sizeof(s_target))) {
        ESP_LOGW(TAG,
                 "discarding incompatible printer selection: read=%s size=%u expected=%u",
                 esp_err_to_name(err), (unsigned)size,
                 (unsigned)sizeof(s_target));
        return ESP_OK;
    }
    if (err == ESP_OK) {
        ESP_LOGW(TAG,
                 "discarding invalid printer selection: schema=%" PRIu32
                 " expected=%u",
                 stored_schema, (unsigned)ESPRESSO_PROFILE_SCHEMA);
        return ESP_OK;
    }
    ESP_LOGE(TAG, "could not read persisted printer selection: %s",
             esp_err_to_name(err));
    return err;
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
