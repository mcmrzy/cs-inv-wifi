#include "led1_waveform.h"
#include "driver/gpio.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static rmt_symbol_word_t s_symbols[LED1_SYMBOLS];
static const char *TAG = "led1_test";

void app_main(void)
{
    ESP_LOGI(TAG, "LED1_ONLY v0.1.0-led1-diag GPIO10 GRB first=RED R=64 G=0 B=0; LED2/LED3=OFF");
    ESP_LOGI(TAG, "No LCD, keys, UART, WiFi, BLE, MQTT or NVS initialization");
    ESP_ERROR_CHECK(gpio_set_level(LED1_GPIO, 0));
    const gpio_config_t gpio = {
        .pin_bit_mask = 1ULL << LED1_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&gpio));
    vTaskDelay(pdMS_TO_TICKS(20));

    rmt_channel_handle_t channel = NULL;
    const rmt_tx_channel_config_t config = {
        .gpio_num = LED1_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = LED1_RMT_HZ,
        /* All 72 bits + latch + EOF fit; no mid-frame refill is needed. */
        .mem_block_symbols = 96,
        .trans_queue_depth = 1,
    };
    ESP_ERROR_CHECK(rmt_new_tx_channel(&config, &channel));
    rmt_encoder_handle_t encoder = NULL;
    const rmt_copy_encoder_config_t copy = {0};
    ESP_ERROR_CHECK(rmt_new_copy_encoder(&copy, &encoder));
    ESP_ERROR_CHECK(rmt_enable(channel));

    led1_pulse_t pulses[LED1_SYMBOLS];
    led1_build_waveform(pulses);
    for (unsigned i = 0; i < LED1_SYMBOLS; ++i) {
        s_symbols[i] = (rmt_symbol_word_t){
            .level0 = i < LED1_DATA_BITS,
            .duration0 = pulses[i].high,
            .level1 = 0,
            .duration1 = pulses[i].low,
        };
    }
    const rmt_transmit_config_t tx = {.loop_count = 0, .flags.eot_level = 0};
    unsigned frames = 0;
    for (;;) {
        ESP_ERROR_CHECK(rmt_transmit(channel, encoder, s_symbols, sizeof(s_symbols), &tx));
        ESP_ERROR_CHECK(rmt_tx_wait_all_done(channel, 100));
        if (++frames == 1 || frames % 5 == 0) {
            ESP_LOGI(TAG, "LED1_RED sent=%u GPIO10 first=64/0/0 rest=OFF; transfer OK is not optical confirmation", frames);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
