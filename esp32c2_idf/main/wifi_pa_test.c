#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "driver/gpio.h"

static const char *TAG = "WIFI_TEST";

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *data)
{
    if (event_id == WIFI_EVENT_AP_START) {
        ESP_LOGI(TAG, ">>> AP STARTED <<<");
    } else if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        ESP_LOGI(TAG, "Station connected!");
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, " WiFi AP + GPIO12 HIGH Test");
    ESP_LOGI(TAG, "========================================");

    // Force GPIO12 HIGH to enable RF PA
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << 12),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level(12, 1);
    ESP_LOGI(TAG, "GPIO12 set HIGH (RF_PA_EN)");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL);

    esp_wifi_set_mode(WIFI_MODE_AP);

    wifi_config_t ap_cfg = {
        .ap = {
            .ssid = "ESP32C2_PA",
            .ssid_len = strlen("ESP32C2_PA"),
            .channel = 1,
            .max_connection = 4,
            .authmode = WIFI_AUTH_OPEN,
        },
    };
    esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);

    esp_wifi_start();
    ESP_LOGI(TAG, "AP SSID: ESP32C2_PA, Channel: 1");
    ESP_LOGI(TAG, "Please search for 'ESP32C2_PA' on your phone");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        ESP_LOGI(TAG, "Running...");
    }
}
