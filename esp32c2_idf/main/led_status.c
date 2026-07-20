#include "led_status.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "led";

#define LEDC_SPEED_MODE  LEDC_LOW_SPEED_MODE
#define LEDC_TIMER       LEDC_TIMER_0
#define LEDC_DUTY_RES    LEDC_TIMER_8_BIT

// PIN_LED2 < 0 表示硬件上没有 LED2
static bool s_led2_available = (PIN_LED2 >= 0);

static const ledc_channel_config_t led2_channel_cfg = {
    .gpio_num   = PIN_LED2,
    .speed_mode = LEDC_SPEED_MODE,
    .channel    = LEDC_CHANNEL_0,
    .timer_sel  = LEDC_TIMER,
    .duty       = 0,
    .hpoint     = 0,
};

struct LedSlot {
    int        pin;
    LedPattern pattern;
    int64_t    last_toggle_us;
    bool       cur_on;
    int        breath_step;
    int        breath_dir;
    int        flash_count;
    int        flash_total;
};

static struct LedSlot g_led1;
static struct LedSlot g_led2;

static bool    g_data_flash_active = false;
static int64_t g_data_flash_start  = 0;
static LedPattern g_data_flash_prev = LED_OFF;

#define DATA_FLASH_MS  80

#define SLOW_MS   500
#define FAST_MS   166
#define FAULT_MS  100
#define BREATH_T  1500
#define BREATH_S  30
#define BREATH_MS (BREATH_T / BREATH_S)
#define FLASH_ON  150
#define FLASH_OFF 250

static inline int64_t millis_now(void) {
    return esp_timer_get_time() / 1000;
}

static void led_write(struct LedSlot *s, bool on) {
    if (s->pin < 0) return;  // 硬件上不存在此引脚
    gpio_set_level(s->pin, on ? 1 : 0);
    s->cur_on = on;
}

static void led_on(struct LedSlot *s)  { led_write(s, true); }
static void led_off(struct LedSlot *s) { led_write(s, false); }

void led_init(void) {
    g_led1.pin = PIN_LED1;  g_led1.pattern = LED_OFF;  g_led1.last_toggle_us = 0;
    g_led1.cur_on = false;   g_led1.breath_step = 0;    g_led1.breath_dir = 1;
    g_led1.flash_count = 0;  g_led1.flash_total = 0;

    g_led2.pin = PIN_LED2;  g_led2.pattern = LED_OFF;  g_led2.last_toggle_us = 0;
    g_led2.cur_on = false;   g_led2.breath_step = 0;    g_led2.breath_dir = 1;
    g_led2.flash_count = 0;  g_led2.flash_total = 0;

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << PIN_LED1),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level(PIN_LED1, 0);

    // LED2 仅在硬件存在时初始化 LEDC PWM
    if (s_led2_available) {
        ledc_timer_config_t timer_conf = {
            .speed_mode      = LEDC_SPEED_MODE,
            .duty_resolution = LEDC_DUTY_RES,
            .timer_num       = LEDC_TIMER,
            .freq_hz         = 5000,
            .clk_cfg         = LEDC_AUTO_CLK,
        };
        ledc_timer_config(&timer_conf);

        ledc_channel_config(&led2_channel_cfg);
        ledc_set_duty(LEDC_SPEED_MODE, LEDC_CHANNEL_0, 0);
        ledc_update_duty(LEDC_SPEED_MODE, LEDC_CHANNEL_0);
    }
}

static void led_set_pattern(struct LedSlot *s, LedPattern p) {
    if (s->pattern == p) return;

    led_off(s);

    s->pattern       = p;
    s->last_toggle_us = millis_now();
    s->breath_step   = 0;
    s->breath_dir    = 1;
    s->flash_count   = 0;
    s->flash_total   = 0;

    switch (p) {
        case LED_OFF:
            break;
        case LED_SLOW_BLINK:
        case LED_FAST_BLINK:
        case LED_FAULT:
            led_on(s);
            break;
        case LED_SOLID:
            led_on(s);
            break;
        case LED_BREATH:
            break;
        case LED_3FLASH:
            s->flash_total = 3;
            s->flash_count = 1;
            led_on(s);
            s->last_toggle_us = millis_now();
            break;
    }
}

void led1_set(LedPattern state) {
    led_set_pattern(&g_led1, state);
}

void led2_set(LedPattern state) {
    if (!s_led2_available) return;
    g_data_flash_prev = state;
    led_set_pattern(&g_led2, state);
}

void led_signal_data(void) {
    g_data_flash_active = true;
    g_data_flash_start  = millis_now();
}

static void led_tick_slot(struct LedSlot *s, bool is_led2) {
    int64_t now = millis_now();

    switch (s->pattern) {
        case LED_OFF:
        case LED_SOLID:
            break;

        case LED_SLOW_BLINK:
            if (now - s->last_toggle_us >= SLOW_MS) {
                s->last_toggle_us = now;
                led_write(s, !s->cur_on);
            }
            break;

        case LED_FAST_BLINK:
            if (now - s->last_toggle_us >= FAST_MS) {
                s->last_toggle_us = now;
                led_write(s, !s->cur_on);
            }
            break;

        case LED_FAULT:
            if (now - s->last_toggle_us >= FAULT_MS) {
                s->last_toggle_us = now;
                led_write(s, !s->cur_on);
            }
            break;

        case LED_BREATH:
            if (now - s->last_toggle_us >= BREATH_MS) {
                s->last_toggle_us = now;
                s->breath_step += s->breath_dir;
                uint32_t duty = (s->breath_step * 255) / BREATH_S;
                if (is_led2 && s_led2_available) {
                    ledc_set_duty(LEDC_SPEED_MODE, LEDC_CHANNEL_0, duty);
                    ledc_update_duty(LEDC_SPEED_MODE, LEDC_CHANNEL_0);
                } else {
                    gpio_set_level(s->pin, duty > 127 ? 1 : 0);
                }
                if (s->breath_step >= BREATH_S)  { s->breath_dir = -1; }
                if (s->breath_step <= 0)         { s->breath_dir = 1; s->breath_step = 0; }
            }
            break;

        case LED_3FLASH:
            if (s->flash_count < s->flash_total * 2) {
                int64_t interval = (s->flash_count % 2 == 0) ? FLASH_OFF : FLASH_ON;
                if (now - s->last_toggle_us >= interval) {
                    led_write(s, (s->flash_count % 2 == 0));
                    s->last_toggle_us = now;
                    s->flash_count++;
                    if (s->flash_count >= s->flash_total * 2) {
                        s->pattern = LED_OFF;
                        led_off(s);
                    }
                }
            }
            break;
    }
}

void led_tick(void) {
    led_tick_slot(&g_led1, false);

    if (!s_led2_available) return;

    if (g_data_flash_active) {
        if (millis_now() - g_data_flash_start < DATA_FLASH_MS) {
            gpio_set_level(PIN_LED2, 1);
        } else {
            g_data_flash_active = false;
            led_tick_slot(&g_led2, true);
        }
    } else {
        led_tick_slot(&g_led2, true);
    }
}
