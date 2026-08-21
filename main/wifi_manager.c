#include "wifi_manager.h"

#include <string.h>
#include <time.h>

#include "app_state.h"
#include "dns_server.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "printer_discovery.h"

static const char *TAG = "espresso_wifi";
static esp_netif_t *s_ap_netif;
static bool s_has_credentials;
static bool s_portal_active;
static unsigned s_retry_count;
static bool s_stop_scheduled;
static bool s_ignore_next_disconnect;
static char s_setup_ssid[33];
static dns_server_handle_t s_dns;

static void printer_network_ready_task(void *context)
{
    (void)context;
    if (time(NULL) < 1704067200) {
        esp_sntp_config_t config =
            ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
        esp_err_t err = esp_netif_sntp_init(&config);
        if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
            for (int attempt = 0;
                 attempt < 5 && time(NULL) < 1704067200; ++attempt) {
                esp_netif_sntp_sync_wait(pdMS_TO_TICKS(2000));
            }
        }
        if (time(NULL) < 1704067200) {
            ESP_LOGW(TAG, "clock is not synchronized; IPPS certificate checks may fail");
        }
    }
    printer_discovery_network_ready();
    vTaskDelete(NULL);
}

static void stop_portal_task(void *context)
{
    (void)context;
    vTaskDelay(pdMS_TO_TICKS(15000));
    bool connected = false;
    app_state_get_wifi(&connected, NULL, 0, NULL, 0);
    if (s_portal_active && connected) {
        dns_server_stop(s_dns);
        s_dns = NULL;
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_mode(WIFI_MODE_STA));
        s_portal_active = false;
        ESP_LOGI(TAG, "setup access point stopped; configuration remains at espresso.local");
    }
    s_stop_scheduled = false;
    vTaskDelete(NULL);
}

static void start_portal(void)
{
    if (s_portal_active) {
        return;
    }
    wifi_config_t ap = {0};
    ap.ap.ssid_len = strlen(s_setup_ssid);
    memcpy(ap.ap.ssid, s_setup_ssid, ap.ap.ssid_len);
    ap.ap.channel = 1;
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_OPEN;
    ap.ap.pmf_cfg.required = false;

    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_config(WIFI_IF_AP, &ap));

    esp_netif_ip_info_t info = {0};
    if (esp_netif_get_ip_info(s_ap_netif, &info) == ESP_OK) {
        static char captive_url[32];
        snprintf(captive_url, sizeof(captive_url), "http://" IPSTR, IP2STR(&info.ip));
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_stop(s_ap_netif));
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_option(
            s_ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI,
            captive_url, strlen(captive_url)));
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_dhcps_start(s_ap_netif));
    }
    s_dns = dns_server_start("WIFI_AP_DEF");
    s_portal_active = true;
    ESP_LOGI(TAG, "setup portal available on Wi-Fi network %s", s_setup_ssid);
}

static void event_handler(void *context, esp_event_base_t base, int32_t event_id,
                          void *event_data)
{
    (void)context;
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        app_state_set_wifi(false, "", "");
        if (s_ignore_next_disconnect) {
            s_ignore_next_disconnect = false;
            return;
        }
        if (s_has_credentials && s_retry_count++ < 5) {
            esp_wifi_connect();
        } else {
            ESP_LOGW(TAG, "saved Wi-Fi unavailable; opening setup portal");
            start_portal();
        }
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = event_data;
        wifi_config_t config = {0};
        esp_wifi_get_config(WIFI_IF_STA, &config);
        char ip[16];
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&event->ip_info.ip));
        app_state_set_wifi(true, (const char *)config.sta.ssid, ip);
        s_retry_count = 0;
        ESP_LOGI(TAG, "connected to %s at %s", config.sta.ssid, ip);
        xTaskCreate(printer_network_ready_task, "printer_resolve", 4096,
                    NULL, 4, NULL);
        if (s_portal_active && !s_stop_scheduled) {
            s_stop_scheduled = true;
            xTaskCreate(stop_portal_task, "portal_stop", 3072, NULL, 4, NULL);
        }
    }
}

esp_err_t wifi_manager_init(void)
{
    esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (!s_ap_netif) {
        return ESP_FAIL;
    }

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init), TAG, "Wi-Fi init failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_FLASH), TAG, "Wi-Fi storage failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                    event_handler, NULL),
                        TAG, "Wi-Fi event registration failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                    event_handler, NULL),
                        TAG, "IP event registration failed");

    snprintf(s_setup_ssid, sizeof(s_setup_ssid), "ESPresso-%s",
             ESPRESSO_SETUP_SUFFIX);

    wifi_config_t stored = {0};
    if (esp_wifi_get_config(WIFI_IF_STA, &stored) == ESP_OK && stored.sta.ssid[0]) {
        s_has_credentials = true;
    }
    return ESP_OK;
}

esp_err_t wifi_manager_start(void)
{
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(s_has_credentials ? WIFI_MODE_STA : WIFI_MODE_APSTA),
                        TAG, "Wi-Fi mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "Wi-Fi start failed");
    if (s_has_credentials) {
        return esp_wifi_connect();
    }
    start_portal();
    return ESP_OK;
}

esp_err_t wifi_manager_scan(wifi_scan_entry_t *entries, size_t capacity, size_t *count)
{
    if (!entries || !count || capacity == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    wifi_scan_config_t config = {.show_hidden = false};
    esp_err_t err = esp_wifi_scan_start(&config, true);
    if (err != ESP_OK) {
        return err;
    }
    uint16_t number = capacity > UINT16_MAX ? UINT16_MAX : (uint16_t)capacity;
    wifi_ap_record_t records[ESPRESSO_WIFI_SCAN_MAX];
    if (number > ESPRESSO_WIFI_SCAN_MAX) {
        number = ESPRESSO_WIFI_SCAN_MAX;
    }
    ESP_RETURN_ON_ERROR(esp_wifi_scan_get_ap_records(&number, records), TAG, "scan results failed");

    size_t written = 0;
    for (uint16_t i = 0; i < number && written < capacity; ++i) {
        if (!records[i].ssid[0]) {
            continue;
        }
        bool duplicate = false;
        for (size_t j = 0; j < written; ++j) {
            if (strcmp(entries[j].ssid, (const char *)records[i].ssid) == 0) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            continue;
        }
        snprintf(entries[written].ssid, sizeof(entries[written].ssid), "%s", records[i].ssid);
        entries[written].rssi = records[i].rssi;
        entries[written].secure = records[i].authmode != WIFI_AUTH_OPEN;
        written++;
    }
    *count = written;
    return ESP_OK;
}

esp_err_t wifi_manager_connect(const char *ssid, const char *password)
{
    if (!ssid || ssid[0] == '\0' || strlen(ssid) > 32 ||
        (password && strlen(password) > 63)) {
        return ESP_ERR_INVALID_ARG;
    }
    wifi_config_t config = {0};
    snprintf((char *)config.sta.ssid, sizeof(config.sta.ssid), "%s", ssid);
    snprintf((char *)config.sta.password, sizeof(config.sta.password), "%s", password ? password : "");
    config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    config.sta.pmf_cfg.capable = true;
    config.sta.pmf_cfg.required = false;

    s_has_credentials = true;
    s_retry_count = 0;
    /* Keep the named captive portal available while new credentials are tried. */
    start_portal();
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "APSTA mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &config), TAG, "credentials failed");
    bool connected = false;
    app_state_get_wifi(&connected, NULL, 0, NULL, 0);
    if (connected) {
        s_ignore_next_disconnect = true;
        esp_err_t disconnect_err = esp_wifi_disconnect();
        if (disconnect_err != ESP_OK) {
            s_ignore_next_disconnect = false;
        }
    }
    esp_err_t connect_err = esp_wifi_connect();
    if (connect_err != ESP_OK) {
        s_ignore_next_disconnect = false;
    }
    return connect_err;
}

esp_err_t wifi_manager_forget(void)
{
    ESP_RETURN_ON_ERROR(esp_wifi_restore(), TAG, "Wi-Fi restore failed");
    esp_restart();
    return ESP_OK;
}

bool wifi_manager_portal_active(void)
{
    return s_portal_active;
}

const char *wifi_manager_setup_ssid(void)
{
    return s_setup_ssid;
}
