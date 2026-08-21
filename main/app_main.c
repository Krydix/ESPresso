#include "app_state.h"
#include "ipp_proxy.h"
#include "printer_discovery.h"
#include "web_server.h"
#include "wifi_manager.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
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
    ESP_ERROR_CHECK(wifi_manager_init());
    ESP_ERROR_CHECK(web_server_start());
    ESP_ERROR_CHECK(wifi_manager_start());
    ESP_ERROR_CHECK(printer_discovery_init());
    ESP_ERROR_CHECK(ipp_proxy_start());

    ESP_LOGI(TAG, "ESPresso is ready; configure it at http://espresso.local/");
}
