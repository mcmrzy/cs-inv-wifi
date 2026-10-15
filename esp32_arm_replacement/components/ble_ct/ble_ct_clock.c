#include "ble_ct_clock.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "ble_ct_clock";
static uint32_t s_time_base = 0;
static uint32_t s_base_uptime_ms = 0;

static uint32_t uptime_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

uint32_t ble_ct_now(void) {
    if (s_time_base == 0) return 0;
    return s_time_base + (uptime_ms() - s_base_uptime_ms) / 1000;
}

void ble_ct_calibrate(uint32_t unix_ts) {
    s_time_base = unix_ts;
    s_base_uptime_ms = uptime_ms();
    ESP_LOGI(TAG, "Time calibrated: %u", unix_ts);

    nvs_handle_t handle;
    if (nvs_open("ble_ct", NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_u32(handle, "time_base", unix_ts);
        nvs_set_u32(handle, "base_uptime", s_base_uptime_ms);
        nvs_commit(handle);
        nvs_close(handle);
    }
}

void ble_ct_clock_init(void) {
    nvs_handle_t handle;
    if (nvs_open("ble_ct", NVS_READONLY, &handle) == ESP_OK) {
        uint32_t tb = 0, bu = 0;
        if (nvs_get_u32(handle, "time_base", &tb) == ESP_OK &&
            nvs_get_u32(handle, "base_uptime", &bu) == ESP_OK && tb > 0) {
            s_time_base = tb;
            s_base_uptime_ms = bu;
            ESP_LOGI(TAG, "Clock restored: %u", ble_ct_now());
        }
        nvs_close(handle);
    }
}
