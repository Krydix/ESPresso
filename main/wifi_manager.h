#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define ESPRESSO_WIFI_SCAN_MAX 20

typedef struct {
    char ssid[33];
    int8_t rssi;
    bool secure;
} wifi_scan_entry_t;

esp_err_t wifi_manager_init(void);
esp_err_t wifi_manager_start(void);
esp_err_t wifi_manager_scan(wifi_scan_entry_t *entries, size_t capacity, size_t *count);
esp_err_t wifi_manager_connect(const char *ssid, const char *password);
esp_err_t wifi_manager_forget(void);
bool wifi_manager_portal_active(void);
const char *wifi_manager_setup_ssid(void);
