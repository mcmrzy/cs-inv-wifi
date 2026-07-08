#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "TEST";
#define LED_PIN 18

void app_main(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, " GPIO18 Blink Test - ESP32-C2");
    ESP_LOGI(TAG, "========================================");

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << LED_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    ESP_LOGI(TAG, "GPIO%d configured as output", LED_PIN);

    int state = 0;
    while (1) {
        state = !state;
        gpio_set_level(LED_PIN, state);
        ESP_LOGI(TAG, "LED %s (GPIO%d=%d)", state ? "ON" : "OFF", LED_PIN, state);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
