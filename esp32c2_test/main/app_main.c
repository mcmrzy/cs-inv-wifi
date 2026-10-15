#include <string.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "wifi_scan";

static const char *auth_mode_str(wifi_auth_mode_t mode)
{
    switch (mode) {
    case WIFI_AUTH_OPEN:            return "OPEN";
    case WIFI_AUTH_WEP:             return "WEP";
    case WIFI_AUTH_WPA_PSK:         return "WPA_PSK";
    case WIFI_AUTH_WPA2_PSK:        return "WPA2_PSK";
    case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA_WPA2_PSK";
    case WIFI_AUTH_WPA3_PSK:        return "WPA3_PSK";
    case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2_WPA3_PSK";
    default:                        return "UNKNOWN";
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== ESP32-C2 WiFi Scan ===");

    /* NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    /* Network */
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    /* WiFi init */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();

    ESP_LOGI(TAG, "WiFi started, begin scanning...");

    /* Scan loop */
    while (1) {
        wifi_scan_config_t scan_cfg = {
            .show_hidden = true,
        };

        ret = esp_wifi_scan_start(&scan_cfg, true);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Scan failed: %s", esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        uint16_t ap_count = 0;
        esp_wifi_scan_get_ap_num(&ap_count);
        ESP_LOGI(TAG, "--- Found %u APs ---", ap_count);

        if (ap_count > 0) {
            wifi_ap_record_t *records = malloc(sizeof(wifi_ap_record_t) * ap_count);
            if (records) {
                esp_wifi_scan_get_ap_records(&ap_count, records);
                for (int i = 0; i < ap_count; i++) {
                    ESP_LOGI(TAG, "[%2d] SSID:%-32s  RSSI:%d  CH:%2d  Auth:%s",
                             i + 1,
                             records[i].ssid,
                             records[i].rssi,
                             records[i].primary,
                             auth_mode_str(records[i].authmode));
                }
                free(records);
            }
        }

        ESP_LOGI(TAG, "heap=%lu", (unsigned long)esp_get_free_heap_size());
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
