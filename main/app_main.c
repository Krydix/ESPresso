#include "app_state.h"
#include "improv_serial.h"
#include "ipp_proxy.h"
#include "ota_update.h"
#include "printer_discovery.h"
#include "tls_identity.h"
#include "web_server.h"
#include "wifi_manager.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "nvs_flash.h"

static const char *TAG = "espresso";

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(app_state_init());
    ESP_ERROR_CHECK(tls_identity_init());
    ESP_ERROR_CHECK(ota_update_init());
    ESP_ERROR_CHECK(wifi_manager_init());
    ESP_ERROR_CHECK(web_server_start());
    ESP_ERROR_CHECK(wifi_manager_start());
    ESP_ERROR_CHECK(improv_serial_start());
    ESP_ERROR_CHECK(printer_discovery_init());
    ESP_ERROR_CHECK(ipp_proxy_start());

#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_ERROR_CHECK(esp_ota_mark_app_valid_cancel_rollback());
        ESP_LOGI(TAG, "OTA firmware passed startup checks; rollback cancelled");
    }
#endif

    ESP_LOGI(TAG, "ESPresso is ready; configure it at http://espresso.local/");
}
