#include <stdint.h>
#include "driver/gpio.h"
#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

enum {
    LED_COUNT = 4,
    DATA_BITS = LED_COUNT * 24,
    COLOR_CYCLE = 6 * 256,
    PEAK_BRIGHTNESS = 96,
};

static rmt_symbol_word_t symbols[DATA_BITS + 1];

static void rainbow_grb(unsigned hue, uint8_t grb[3])
{
    const uint8_t rise = (hue % 256U) * PEAK_BRIGHTNESS / 255U;
    const uint8_t fall = PEAK_BRIGHTNESS - rise;
    const uint8_t wheel[][3] = {
        {rise, PEAK_BRIGHTNESS, 0}, {PEAK_BRIGHTNESS, fall, 0},
        {PEAK_BRIGHTNESS, 0, rise}, {fall, 0, PEAK_BRIGHTNESS},
        {0, rise, PEAK_BRIGHTNESS}, {0, PEAK_BRIGHTNESS, fall},
    };
    for (unsigned i = 0; i < 3; ++i) grb[i] = wheel[hue / 256U][i];
}

static void make_frame(unsigned phase)
{
    for (unsigned led = 0; led < LED_COUNT; ++led) {
        uint8_t grb[3];
        rainbow_grb((phase + led * (COLOR_CYCLE / LED_COUNT)) % COLOR_CYCLE, grb);
        for (unsigned bit = 0; bit < 24; ++bit) {
            const bool one = (grb[bit / 8] & (0x80U >> (bit % 8))) != 0;
            symbols[led * 24 + bit] = (rmt_symbol_word_t){
                .level0 = 1, .duration0 = one ? 14 : 7,
                .level1 = 0, .duration1 = one ? 11 : 18,
            };
        }
    }
    symbols[DATA_BITS] = (rmt_symbol_word_t){.duration0 = 3000, .duration1 = 3000};
}

void app_main(void)
{
    const gpio_config_t gpio = {.pin_bit_mask = 1ULL << GPIO_NUM_9, .mode = GPIO_MODE_OUTPUT};
    ESP_ERROR_CHECK(gpio_set_level(GPIO_NUM_9, 0));
    ESP_ERROR_CHECK(gpio_config(&gpio));
    vTaskDelay(pdMS_TO_TICKS(20));

    const rmt_tx_channel_config_t channel_config = {
        .gpio_num = GPIO_NUM_9, .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 20000000, .mem_block_symbols = 144, .trans_queue_depth = 1,
    };
    rmt_channel_handle_t channel = NULL;
    rmt_encoder_handle_t encoder = NULL;
    const rmt_copy_encoder_config_t copy_config = {};
    const rmt_transmit_config_t tx = {.flags.eot_level = 0};
    ESP_ERROR_CHECK(rmt_new_tx_channel(&channel_config, &channel));
    ESP_ERROR_CHECK(rmt_new_copy_encoder(&copy_config, &encoder));
    ESP_ERROR_CHECK(rmt_enable(channel));

    TickType_t next_frame = xTaskGetTickCount();
    for (unsigned phase = 0;; phase = (phase + 6) % COLOR_CYCLE) {
        make_frame(phase);
        ESP_ERROR_CHECK(rmt_transmit(channel, encoder, symbols, sizeof(symbols), &tx));
        ESP_ERROR_CHECK(rmt_tx_wait_all_done(channel, 100));
        vTaskDelayUntil(&next_frame, pdMS_TO_TICKS(20));
    }
}
