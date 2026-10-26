#include "direct_buzzer.h"
#include "sdkconfig.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static esp_timer_handle_t s_timer;
static bool s_enabled = true;
static int64_t s_off_at;

static void output(bool on)
{
#if CONFIG_DIRECT_BUZZ_PIN >= 0
    const int active = CONFIG_DIRECT_BUZZ_ACTIVE_LEVEL ? 1 : 0;
    gpio_set_level(CONFIG_DIRECT_BUZZ_PIN, on ? active : !active);
#else
    (void)on;
#endif
}

static void timer_tick(void *arg)
{
    (void)arg;
    const int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_mux);
    if (s_off_at && now >= s_off_at) {
        output(false);
        s_off_at = 0;
    }
    portEXIT_CRITICAL(&s_mux);
}

esp_err_t direct_buzzer_init(void)
{
    if (s_timer) return ESP_OK;
    uint16_t enabled = 1;
    nvs_handle_t h;
    if (nvs_open("direct_ui", NVS_READONLY, &h) == ESP_OK) {
        (void)nvs_get_u16(h, "buzzer", &enabled);
        nvs_close(h);
    }
    direct_buzzer_set_enabled(enabled != 0);
    const esp_timer_create_args_t args = {.callback = timer_tick, .name = "key_beep"};
    esp_timer_handle_t timer;
    esp_err_t err = esp_timer_create(&args, &timer);
    if (err != ESP_OK) return err;
    /* Independent deadline check: overlapping key/alarm pulses extend the beep
       safely. No callback can switch off a newer pulse using an old deadline.
       Nominal duration 70ms, timer resolution adds at most 5ms when scheduled. */
    err = esp_timer_start_periodic(timer, 5000);
    if (err != ESP_OK) { esp_timer_delete(timer); return err; }
    s_timer = timer;
    return ESP_OK;
}

void direct_buzzer_set_enabled(bool enabled)
{
    portENTER_CRITICAL(&s_mux);
    s_enabled = enabled;
    if (!enabled) { s_off_at = 0; output(false); }
    portEXIT_CRITICAL(&s_mux);
}

void direct_buzzer_pulse(void)
{
    const int64_t off_at = esp_timer_get_time() + 70000;
    portENTER_CRITICAL(&s_mux);
    if (s_enabled && s_timer) { s_off_at = off_at; output(true); }
    portEXIT_CRITICAL(&s_mux);
}
