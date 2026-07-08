#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"

static const char *TAG = "wifi_scan";

void wifi_scan(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Starting WiFi scan...");

    while (1) {
        wifi_scan_config_t scan_config = {
            .ssid = NULL,
            .bssid = NULL,
            .channel = 0,
            .show_hidden = true,
        };

        ESP_ERROR_CHECK(esp_wifi_scan_start(&scan_config, true));

        uint16_t ap_count = 0;
        esp_wifi_scan_get_ap_num(&ap_count);
        ESP_LOGI(TAG, "Total APs found: %d", ap_count);

        if (ap_count > 0) {
            wifi_ap_record_t *ap_records = malloc(sizeof(wifi_ap_record_t) * ap_count);
            if (ap_records) {
                esp_wifi_scan_get_ap_records(&ap_count, ap_records);

                ESP_LOGI(TAG, "----------------------------------------");
                ESP_LOGI(TAG, " #  | SSID                             | RSSI | CH | BSSID");
                ESP_LOGI(TAG, "----|----------------------------------|------|----|-----------------");

                for (int i = 0; i < ap_count; i++) {
                    ESP_LOGI(TAG, "%3d | %-32s | %4d | %2d | %02x:%02x:%02x:%02x:%02x:%02x",
                             i + 1,
                             ap_records[i].ssid,
                             ap_records[i].rssi,
                             ap_records[i].primary,
                             ap_records[i].bssid[0],
                             ap_records[i].bssid[1],
                             ap_records[i].bssid[2],
                             ap_records[i].bssid[3],
                             ap_records[i].bssid[4],
                             ap_records[i].bssid[5]);
                }
                ESP_LOGI(TAG, "----------------------------------------");

                free(ap_records);
            }
        }

        ESP_LOGI(TAG, "Scan complete. Waiting 5 seconds before next scan...");
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, " ESP32-C2 WiFi Scan Test");
    ESP_LOGI(TAG, "========================================");

    wifi_scan();
}
