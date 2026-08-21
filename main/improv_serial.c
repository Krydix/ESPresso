#include "improv_serial.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "app_state.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "improv_serial_codec.h"
#include "sdkconfig.h"
#include "wifi_manager.h"

#define IMPROV_TYPE_CURRENT_STATE 0x01
#define IMPROV_TYPE_ERROR_STATE 0x02
#define IMPROV_TYPE_RPC_COMMAND 0x03
#define IMPROV_TYPE_RPC_RESULT 0x04

#define IMPROV_STATE_READY 0x02
#define IMPROV_STATE_PROVISIONING 0x03
#define IMPROV_STATE_PROVISIONED 0x04

#define IMPROV_ERROR_NONE 0x00
#define IMPROV_ERROR_INVALID_RPC 0x01
#define IMPROV_ERROR_UNKNOWN_RPC 0x02
#define IMPROV_ERROR_UNABLE_TO_CONNECT 0x03

#define IMPROV_RPC_WIFI_SETTINGS 0x01
#define IMPROV_RPC_CURRENT_STATE 0x02
#define IMPROV_RPC_DEVICE_INFO 0x03
#define IMPROV_RPC_WIFI_NETWORKS 0x04
#define IMPROV_RPC_NETWORK_STATE 0x07

#define IMPROV_CONNECT_TIMEOUT_MS 30000
#define IMPROV_RX_TIMEOUT_MS 1000

static const char *TAG = "espresso_improv";
static const uart_port_t s_uart = (uart_port_t)CONFIG_ESP_CONSOLE_UART_NUM;

static esp_err_t write_packet(uint8_t type, const uint8_t *data, size_t length)
{
    uint8_t packet[IMPROV_SERIAL_MAX_PACKET_SIZE];
    size_t packet_length = improv_serial_encode_packet(type, data, length,
                                                       packet, sizeof(packet));
    if (packet_length == 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (uart_write_bytes(s_uart, packet, packet_length) < 0 ||
        uart_write_bytes(s_uart, "\n", 1) < 0) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void send_state(uint8_t state)
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(write_packet(IMPROV_TYPE_CURRENT_STATE, &state, 1));
}

static void send_error(uint8_t error)
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(write_packet(IMPROV_TYPE_ERROR_STATE, &error, 1));
}

static esp_err_t send_rpc_result(uint8_t command, const char *const *strings,
                                 size_t string_count)
{
    uint8_t data[IMPROV_SERIAL_MAX_DATA];
    size_t position = 2;
    data[0] = command;

    for (size_t i = 0; i < string_count; ++i) {
        size_t length = strings[i] ? strlen(strings[i]) : 0;
        if (length > UINT8_MAX || position + 1 + length > sizeof(data)) {
            return ESP_ERR_INVALID_SIZE;
        }
        data[position++] = (uint8_t)length;
        if (length > 0) {
            memcpy(&data[position], strings[i], length);
            position += length;
        }
    }
    data[1] = (uint8_t)(position - 2);
    return write_packet(IMPROV_TYPE_RPC_RESULT, data, position);
}

static bool current_wifi(char *ssid, size_t ssid_size, char *ip, size_t ip_size)
{
    bool connected = false;
    app_state_get_wifi(&connected, ssid, ssid_size, ip, ip_size);
    return connected;
}

static void build_local_url(char *url, size_t capacity, const char *ip)
{
    snprintf(url, capacity, "http://%s/", ip);
}

static void handle_current_state(uint8_t command)
{
    char ip[16];
    if (!current_wifi(NULL, 0, ip, sizeof(ip))) {
        send_state(IMPROV_STATE_READY);
        return;
    }

    send_state(IMPROV_STATE_PROVISIONED);
    char url[32];
    build_local_url(url, sizeof(url), ip);
    const char *urls[] = {url};
    ESP_ERROR_CHECK_WITHOUT_ABORT(send_rpc_result(command, urls, 1));
}

static bool decode_wifi_settings(const uint8_t *data, size_t length,
                                 char *ssid, size_t ssid_capacity,
                                 char *password, size_t password_capacity)
{
    if (!data || length < 2) {
        return false;
    }
    size_t position = 0;
    size_t ssid_length = data[position++];
    if (ssid_length == 0 || ssid_length >= ssid_capacity ||
        position + ssid_length + 1 > length) {
        return false;
    }
    memcpy(ssid, &data[position], ssid_length);
    ssid[ssid_length] = '\0';
    position += ssid_length;

    size_t password_length = data[position++];
    if (password_length >= password_capacity || position + password_length != length) {
        return false;
    }
    memcpy(password, &data[position], password_length);
    password[password_length] = '\0';
    return true;
}

static void handle_wifi_settings(const uint8_t *data, size_t length)
{
    char requested_ssid[33];
    char password[64];
    if (!decode_wifi_settings(data, length, requested_ssid, sizeof(requested_ssid),
                              password, sizeof(password))) {
        send_error(IMPROV_ERROR_INVALID_RPC);
        return;
    }

    send_state(IMPROV_STATE_PROVISIONING);
    esp_err_t err = wifi_manager_connect(requested_ssid, password);
    memset(password, 0, sizeof(password));
    if (err != ESP_OK) {
        send_error(IMPROV_ERROR_UNABLE_TO_CONNECT);
        send_state(IMPROV_STATE_READY);
        return;
    }

    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(IMPROV_CONNECT_TIMEOUT_MS);
    char connected_ssid[33];
    char ip[16];
    while ((int32_t)(deadline - xTaskGetTickCount()) > 0) {
        if (current_wifi(connected_ssid, sizeof(connected_ssid), ip, sizeof(ip)) &&
            strcmp(connected_ssid, requested_ssid) == 0) {
            send_state(IMPROV_STATE_PROVISIONED);
            char url[32];
            build_local_url(url, sizeof(url), ip);
            const char *urls[] = {url};
            ESP_ERROR_CHECK_WITHOUT_ABORT(
                send_rpc_result(IMPROV_RPC_WIFI_SETTINGS, urls, 1));
            ESP_LOGI(TAG, "Wi-Fi provisioned over serial for %s", requested_ssid);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    send_error(IMPROV_ERROR_UNABLE_TO_CONNECT);
    send_state(IMPROV_STATE_READY);
    ESP_LOGW(TAG, "serial Wi-Fi provisioning timed out for %s", requested_ssid);
}

static void handle_device_info(void)
{
    const esp_app_desc_t *description = esp_app_get_description();
    const char *strings[] = {
        "ESPresso",
        description->version,
        CONFIG_IDF_TARGET "/" CONFIG_IDF_TARGET,
        wifi_manager_setup_ssid(),
    };
    ESP_ERROR_CHECK_WITHOUT_ABORT(
        send_rpc_result(IMPROV_RPC_DEVICE_INFO, strings, 4));
}

static void handle_wifi_networks(void)
{
    wifi_scan_entry_t entries[ESPRESSO_WIFI_SCAN_MAX];
    size_t count = 0;
    esp_err_t err = wifi_manager_scan(entries, ESPRESSO_WIFI_SCAN_MAX, &count);
    if (err != ESP_OK) {
        send_error(IMPROV_ERROR_UNABLE_TO_CONNECT);
        return;
    }

    for (size_t i = 0; i < count; ++i) {
        char rssi[6];
        snprintf(rssi, sizeof(rssi), "%d", entries[i].rssi);
        const char *strings[] = {
            entries[i].ssid,
            rssi,
            entries[i].secure ? "YES" : "NO",
        };
        ESP_ERROR_CHECK_WITHOUT_ABORT(
            send_rpc_result(IMPROV_RPC_WIFI_NETWORKS, strings, 3));
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(
        send_rpc_result(IMPROV_RPC_WIFI_NETWORKS, NULL, 0));
}

static void handle_network_state(void)
{
    char ip[16];
    bool connected = current_wifi(NULL, 0, ip, sizeof(ip));
    const char *flags = connected ? "3" : "2";
    if (!connected) {
        const char *strings[] = {flags};
        ESP_ERROR_CHECK_WITHOUT_ABORT(
            send_rpc_result(IMPROV_RPC_NETWORK_STATE, strings, 1));
        return;
    }

    char url[32];
    build_local_url(url, sizeof(url), ip);
    const char *strings[] = {flags, url};
    ESP_ERROR_CHECK_WITHOUT_ABORT(
        send_rpc_result(IMPROV_RPC_NETWORK_STATE, strings, 2));
}

static void handle_rpc(const improv_serial_packet_t *packet)
{
    if (packet->type != IMPROV_TYPE_RPC_COMMAND || packet->length < 2) {
        send_error(IMPROV_ERROR_INVALID_RPC);
        return;
    }
    uint8_t command = packet->data[0];
    size_t data_length = packet->data[1];
    if (data_length != (size_t)packet->length - 2) {
        send_error(IMPROV_ERROR_INVALID_RPC);
        return;
    }

    send_error(IMPROV_ERROR_NONE);
    const uint8_t *data = &packet->data[2];
    switch (command) {
        case IMPROV_RPC_WIFI_SETTINGS:
            handle_wifi_settings(data, data_length);
            break;
        case IMPROV_RPC_CURRENT_STATE:
            if (data_length == 0) {
                handle_current_state(command);
            } else {
                send_error(IMPROV_ERROR_INVALID_RPC);
            }
            break;
        case IMPROV_RPC_DEVICE_INFO:
            if (data_length == 0) {
                handle_device_info();
            } else {
                send_error(IMPROV_ERROR_INVALID_RPC);
            }
            break;
        case IMPROV_RPC_WIFI_NETWORKS:
            if (data_length == 0) {
                handle_wifi_networks();
            } else {
                send_error(IMPROV_ERROR_INVALID_RPC);
            }
            break;
        case IMPROV_RPC_NETWORK_STATE:
            if (data_length == 0) {
                handle_network_state();
            } else {
                send_error(IMPROV_ERROR_INVALID_RPC);
            }
            break;
        default:
            send_error(IMPROV_ERROR_UNKNOWN_RPC);
            break;
    }
}

static void improv_task(void *context)
{
    (void)context;
    improv_serial_parser_t parser = {0};
    improv_serial_packet_t packet;
    TickType_t last_byte = 0;
    uint8_t input[64];

    while (true) {
        int received = uart_read_bytes(s_uart, input, sizeof(input), pdMS_TO_TICKS(20));
        if (received <= 0) {
            if (parser.length > 0 &&
                xTaskGetTickCount() - last_byte > pdMS_TO_TICKS(IMPROV_RX_TIMEOUT_MS)) {
                improv_serial_parser_reset(&parser);
            }
            continue;
        }
        last_byte = xTaskGetTickCount();
        for (int i = 0; i < received; ++i) {
            improv_serial_parse_result_t result =
                improv_serial_parser_feed(&parser, input[i], &packet);
            if (result == IMPROV_SERIAL_PARSE_PACKET) {
                handle_rpc(&packet);
            } else if (result == IMPROV_SERIAL_PARSE_INVALID) {
                send_error(IMPROV_ERROR_INVALID_RPC);
            }
        }
    }
}

esp_err_t improv_serial_start(void)
{
    if (!uart_is_driver_installed(s_uart)) {
        ESP_RETURN_ON_ERROR(uart_driver_install(s_uart, 512, 0, 0, NULL, 0),
                            TAG, "UART driver install failed");
    }
    uart_vfs_dev_use_driver(s_uart);
    if (xTaskCreate(improv_task, "improv_serial", 6144, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Improv Serial ready on UART %d at %d baud",
             CONFIG_ESP_CONSOLE_UART_NUM, CONFIG_ESP_CONSOLE_UART_BAUDRATE);
    return ESP_OK;
}
