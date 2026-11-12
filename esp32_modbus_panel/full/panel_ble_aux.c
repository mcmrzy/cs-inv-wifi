#include "panel_ble_aux.h"
#include "panel_ble.h"
#include "panel_preferences.h"
#include "panel_bms_protocol.h"
#include "panel_bus.h"
#include "panel_catalog.h"
#include "panel_ui.h"
#include "direct_link.h"
#include "full_ui_bridge.h"
#include "ble_ct_cmd.h"
#include "ble_ct_telemetry.h"
#include "ble_prov.h"
#include "ota_service.h"
#include "esp_timer.h"
#include "mqtt_handler.h"
#include "cJSON.h"
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

static atomic_bool preferences_changed;
/* The App BLE dispatcher and periodic telemetry run on the same main task. */
static panel_snapshot_t *snapshot;

bool panel_ble_aux_take_preferences_changed(void)
{
    return atomic_exchange(&preferences_changed, false);
}

static bool add_number(cJSON *object, const char *key, double value)
{
    return cJSON_AddNumberToObject(object, key, value) != NULL;
}

static bool add_optional(cJSON *object, const char *key, bool known, double value)
{
    return known ? add_number(object, key, value) : cJSON_AddNullToObject(object, key) != NULL;
}

static bool battery_sample(direct_bms_measurement_t *sample, uint32_t *age)
{
    if (!direct_bms_get_snapshot(sample, age)) return false;
    unsigned scale = sample->percent_divisor ? sample->percent_divisor : 1U;
    return sample->voltage_10mv > 0 && sample->voltage_10mv != 327670U &&
        sample->voltage_10mv != 655350U && sample->current_10ma != 327670 &&
        sample->current_10ma != -327680 && sample->soc_percent <= 100U * scale &&
        sample->soh_percent <= 100U * scale;
}

uint16_t panel_ble_battery_suggestion(const panel_snapshot_t *s, uint16_t address)
{
    if (address != 0x4106 && address != 0x4109 && address != 0x410a) return 0;
    direct_bms_measurement_t b;
    uint32_t age;
    if (panel_bms_protocol_get() != PANEL_BMS_CSERGY || !battery_sample(&b, &age) ||
        age >= 5000 || b.protection) return 0;
    return panel_battery_suggestion(s, address, b.charge_max_deciamp,
                                    b.charge_max_decivolt, b.discharge_min_decivolt);
}

static const char *read_battery(const cJSON *params, cJSON *actual)
{
    uint32_t page;
    if (!panel_ble_read_uint(params, "page", 3, &page)) return "INVALID_ARGS";
    direct_bms_measurement_t b = {0};
    uint32_t age = 0;
    bool known = battery_sample(&b, &age);
    unsigned count = 0;
    if (known) for (unsigned i = 0; i < 32; ++i)
        if (b.cell_mv[i] > 0 && b.cell_mv[i] <= 8191) count = i + 1;
    unsigned scale = b.percent_divisor == 10 ? 10U : 1U;
    if (!add_number(actual, "protocol", panel_bms_protocol_get()) ||
        !cJSON_AddBoolToObject(actual, "online", known && age < 5000) ||
        !add_optional(actual, "age", known, age) ||
        !add_optional(actual, "voltage", known, b.voltage_10mv / 100.0) ||
        !add_optional(actual, "current", known, b.current_10ma / 100.0) ||
        !add_optional(actual, "soc", known, (double)b.soc_percent / scale) ||
        !add_optional(actual, "soh", known, (double)b.soh_percent / scale) ||
        !add_optional(actual, "warning", known, b.warning) ||
        !add_optional(actual, "protection", known, b.protection) ||
        !add_number(actual, "count", count)) return "RESULT_UNAVAILABLE";
    cJSON *cells = cJSON_AddArrayToObject(actual, "cells");
    if (!cells) return "RESULT_UNAVAILABLE";
    unsigned first = page * 8U;
    for (unsigned i = first; i < first + 8U && i < count; ++i) {
        uint16_t mv = b.cell_mv[i];
        cJSON *cell = mv > 0 && mv <= 8191 ? cJSON_CreateNumber(mv) : cJSON_CreateNull();
        if (!cell || !cJSON_AddItemToArray(cells, cell)) {
            cJSON_Delete(cell);
            return "RESULT_UNAVAILABLE";
        }
    }
    return NULL;
}

static const char *read_preferences(cJSON *actual)
{
    panel_preferences_t p;
    if (!panel_preferences_get(&p)) return "NOT_READY";
    time_t epoch = time(NULL);
    bool ok = add_number(actual, "brightness", p.display.brightness) &&
        add_number(actual, "timeout", p.blank ? p.display.timeout_index : 0) &&
        add_number(actual, "led", (p.feedback.led_brightness * 100U + 127U) / 255U) &&
        add_number(actual, "beep", p.buzzer ? p.feedback.beep_mode : 0) &&
        add_number(actual, "theme", p.style & 1U) &&
        add_number(actual, "lang", (p.style >> 1) & 1U) &&
        add_number(actual, "alarm", p.alarm) &&
        add_number(actual, "bms", panel_bms_protocol_get()) &&
        add_number(actual, "detect", panel_bms_detection()) &&
        add_number(actual, "tz", p.timezone) &&
        add_number(actual, "epoch", epoch >= 1577836800 ? epoch : 0);
    return ok ? NULL : "RESULT_UNAVAILABLE";
}

static const char *read_diagnostics(cJSON *actual)
{
    uint32_t stamp = panel_stamp(snapshot, 0x42);
    bool ok = cJSON_AddStringToObject(actual, "model", panel_profile_name(snapshot->config.profile_override)) &&
        cJSON_AddStringToObject(actual, "detected", panel_profile_name(snapshot->detected_profile)) &&
        cJSON_AddBoolToObject(actual, "online", panel_online(snapshot)) &&
        add_number(actual, "address", snapshot->config.address) &&
        add_number(actual, "baud", snapshot->config.baud) &&
        add_number(actual, "parity", snapshot->config.parity) &&
        add_number(actual, "profile", snapshot->config.profile_override) &&
        add_number(actual, "context", snapshot->context_generation) &&
        cJSON_AddBoolToObject(actual, "locked", snapshot->profile_locked) &&
        cJSON_AddBoolToObject(actual, "scanning", snapshot->scanning) &&
        add_number(actual, "found", snapshot->scan_found) &&
        add_number(actual, "polls", snapshot->polls) &&
        add_number(actual, "errors", snapshot->errors) &&
        add_optional(actual, "age", stamp != 0, snapshot->now_ms - stamp);
    return ok ? NULL : "RESULT_UNAVAILABLE";
}

static const char *read_alarms(cJSON *actual)
{
    static const char *const keys[] = {"fault_low", "fault_high", "warn_low", "warn_high"};
    unsigned valid = 0, fresh = 0;
    for (unsigned i = 0; i < 4; ++i) {
        uint16_t value = 0, address = (uint16_t)(0x46 + i);
        if (panel_value(snapshot, address, &value)) valid |= 1U << i;
        if (panel_fresh(snapshot, address)) fresh |= 1U << i;
        if (!add_number(actual, keys[i], value)) return "RESULT_UNAVAILABLE";
    }
    bool ok = add_number(actual, "valid", valid) && add_number(actual, "fresh", fresh) &&
        cJSON_AddStringToObject(actual, "model",
            panel_profile_name(panel_is_3ku(snapshot) ? PANEL_PROFILE_3KU : snapshot->config.profile_override));
    return ok ? NULL : "RESULT_UNAVAILABLE";
}

static const char *read_metrics(const cJSON *params, cJSON *actual)
{
    uint32_t page, offset;
    if (!panel_ble_read_uint(params, "page", PANEL_DETAIL_PAGES - 1U, &page) ||
        !panel_ble_read_uint(params, "offset", 4, &offset) || (offset != 0 && offset != 4))
        return "INVALID_ARGS";
    panel_ui_t view = {.page = PANEL_PAGE_DETAILS, .detail = (uint8_t)page,
                       .language_en = panel_ble_english(params)};
    panel_view_t metric_view;
    if (!panel_ui_view(&view, snapshot, &metric_view)) return "NOT_READY";
    cJSON *rows = cJSON_AddArrayToObject(actual, "rows");
    if (!rows || !cJSON_AddStringToObject(actual, "title", metric_view.title) ||
        !add_number(actual, "next", offset == 0 && metric_view.rows[4][0] ? 4 : 0))
        return "RESULT_UNAVAILABLE";
    for (unsigned i = offset; i < offset + 4 && i < 7; ++i) {
        if (!metric_view.rows[i][0]) continue;
        const char *fields[] = {metric_view.rows[i], metric_view.values[i]};
        cJSON *row = cJSON_CreateStringArray(fields, 2);
        if (!row || !cJSON_AddItemToArray(rows, row)) {
            cJSON_Delete(row);
            return "RESULT_UNAVAILABLE";
        }
    }
    return NULL;
}

const char *panel_ble_aux_read(const char *action, const cJSON *params, cJSON *actual)
{
    if (!strcmp(action, "panel_aux")) return read_preferences(actual);
    if (!strcmp(action, "panel_battery")) return read_battery(params, actual);
    bool diag = !strcmp(action, "panel_diag");
    bool alarms = !strcmp(action, "panel_alarms");
    bool metric = !strcmp(action, "panel_metric");
    if (!diag && !alarms && !metric) return "UNSUPPORTED_ACTION";
    snapshot = panel_ble_snapshot();
    if (!snapshot) return "NOT_READY";
    if (diag) return read_diagnostics(actual);
    if (alarms) return read_alarms(actual);
    return read_metrics(params, actual);
}

typedef enum {
    AUX_BRIGHTNESS, AUX_TIMEOUT, AUX_LED, AUX_BEEP, AUX_THEME, AUX_LANGUAGE,
    AUX_ALARM, AUX_BMS, AUX_BMS_DETECT, AUX_TIMEZONE, AUX_CLOCK, AUX_RESET
} aux_field_t;

int panel_ble_aux_write(const char *id, double value)
{
    static const struct { const char *id; int low, high; } fields[] = {
        [AUX_BRIGHTNESS] = {"panel_brightness", 10, 100},
        [AUX_TIMEOUT] = {"panel_timeout", 0, PANEL_DISPLAY_TIMEOUT_COUNT - 1},
        [AUX_LED] = {"panel_led", 0, 100},
        [AUX_BEEP] = {"panel_beep", 0, PANEL_BEEP_COUNT - 1},
        [AUX_THEME] = {"panel_theme", 0, 1},
        [AUX_LANGUAGE] = {"panel_lang", 0, 1},
        [AUX_ALARM] = {"panel_alarm", 0, 1},
        [AUX_BMS] = {"panel_bms", PANEL_BMS_BBJ, PANEL_BMS_CSERGY},
        [AUX_BMS_DETECT] = {"panel_bms_detect", 1, 1},
        [AUX_TIMEZONE] = {"panel_tz", -720, 840},
        [AUX_CLOCK] = {"panel_epoch", 1577836800, 2147483647},
        [AUX_RESET] = {"panel_reset_prefs", 1, 1},
    };
    unsigned field = 0;
    while (field < sizeof(fields) / sizeof(fields[0]) && strcmp(id, fields[field].id)) field++;
    if (field == sizeof(fields) / sizeof(fields[0])) return -100;
    if (ota_service_is_busy()) return BLE_CMD_UPGRADE_BUSY;
    if (!isfinite(value) || value < fields[field].low || value > fields[field].high ||
        value != floor(value)) return BLE_CMD_OUT_OF_RANGE;
    int raw = (int)value;
    panel_preferences_t update = {0};
    unsigned mask;
    switch (field) {
    case AUX_BRIGHTNESS: update.display.brightness = raw; mask = PANEL_PREF_BRIGHTNESS; break;
    case AUX_TIMEOUT: update.display.timeout_index = raw; mask = PANEL_PREF_TIMEOUT; break;
    case AUX_LED: update.feedback.led_brightness = (raw * 255 + 50) / 100; mask = PANEL_PREF_LED_BRIGHTNESS; break;
    case AUX_BEEP:
        update.buzzer = raw != 0;
        update.feedback.beep_mode = raw ? raw : PANEL_BEEP_HIGH;
        mask = PANEL_PREF_BEEP_MODE;
        break;
    case AUX_THEME: update.style = raw; mask = PANEL_PREF_THEME; break;
    case AUX_LANGUAGE: update.style = raw << 1; mask = PANEL_PREF_LANGUAGE; break;
    case AUX_ALARM: update.alarm = raw; mask = PANEL_PREF_ALARM; break;
    case AUX_TIMEZONE: update.timezone = raw; mask = PANEL_PREF_TIME; break;
    case AUX_BMS: return panel_bms_protocol_set((panel_bms_protocol_t)raw) ? BLE_CMD_OK : BLE_CMD_BUSY;
    case AUX_BMS_DETECT: return panel_bms_detect_start() ? BLE_CMD_OK : BLE_CMD_BUSY;
    case AUX_CLOCK: {
        const struct timeval clock = {.tv_sec = (time_t)raw};
        return settimeofday(&clock, NULL) == 0 ? BLE_CMD_OK : BLE_WRITE_SAVE_FAILED;
    }
    case AUX_RESET:
        if (!panel_preferences_reset()) return BLE_WRITE_SAVE_FAILED;
        full_ui_reload_local_preferences();
        atomic_store(&preferences_changed, true);
        return BLE_CMD_OK;
    default: return BLE_CMD_UNKNOWN;
    }
    if (!panel_preferences_update(&update, mask)) return BLE_WRITE_SAVE_FAILED;
    full_ui_reload_local_preferences();
    atomic_store(&preferences_changed, true);
    return BLE_CMD_OK;
}

static bool add_register(cJSON *object, const char *key, uint16_t address,
                         unsigned scale, bool is_signed)
{
    uint16_t raw = 0;
    bool valid = panel_fresh(snapshot, address) && panel_value(snapshot, address, &raw) &&
        raw != 0x7fff && raw != 0x8000 && (is_signed || raw != 0xffff);
    int value = is_signed ? (int16_t)raw : (int)raw;
    return add_optional(object, key, valid, (double)value / scale);
}

static bool add_live_battery(cJSON *object)
{
    direct_bms_measurement_t b = {0};
    uint32_t age;
    bool direct = battery_sample(&b, &age) && age < 5000;
    uint16_t raw_current = 0;
    bool known = panel_fresh(snapshot, 0x81) && panel_value(snapshot, 0x81, &raw_current) &&
                 raw_current != 0x7fff && raw_current != 0x8000;
    int current = direct ? b.current_10ma : -(int)(int16_t)raw_current * 10;
    const char *direction = current > 0 ? "charging" : current < 0 ? "discharging" : "standby";
    bool ok = cJSON_AddStringToObject(object, "charge_state", direct || known ? direction : "unknown") &&
        add_optional(object, "current", direct || known, current / 100.0);
    if (direct) {
        int64_t microwatts = (int64_t)b.voltage_10mv * b.current_10ma;
        return ok && add_number(object, "voltage", b.voltage_10mv / 100.0) &&
            add_number(object, "power", (double)(microwatts / 10000)) &&
            add_number(object, "soc", (double)b.soc_percent / (b.percent_divisor == 10 ? 10 : 1));
    }
    return ok && add_register(object, "voltage", 0x80, 10, false) &&
        add_register(object, "power", 0x85, 1, true) &&
        add_register(object, "soc", 0x84, 10, false);
}

static bool add_live_pv(cJSON *object)
{
    uint32_t watts = 0;
    bool fresh = false;
    bool known = panel_pv_watts(snapshot, &watts, &fresh) && fresh;
    return add_register(object, "pv_voltage", 0x96, 10, false) &&
        add_register(object, "pv_current", 0x97, 100, false) &&
        add_optional(object, "pv_power", known, watts);
}

static bool add_live_status(cJSON *object)
{
    static const char *const states[] = {
        "boot", "initializing", "standby", "grid", "solar", "battery",
        "generator", "fault", "off", "test", "upgrading"
    };
    uint16_t state = UINT16_MAX, low = 0, high = 0;
    bool known = panel_fresh(snapshot, 0x42) && panel_value(snapshot, 0x42, &state);
    bool have_fault = panel_fresh(snapshot, 0x46) && panel_value(snapshot, 0x46, &low) &&
        panel_fresh(snapshot, 0x47) && panel_value(snapshot, 0x47, &high);
    return cJSON_AddStringToObject(object, "state", known && state < sizeof(states) / sizeof(states[0]) ?
               states[state] : "unknown") &&
        add_optional(object, "fault_code", have_fault, ((uint32_t)high << 16) | low) &&
        add_register(object, "temp_inv", 0x331, panel_is_3ku(snapshot) ? 1 : 10, true);
}

static bool add_timestamp(cJSON *root)
{
    time_t now = time(NULL);
    struct tm utc;
    char timestamp[24];
    if (now < 1577836800 || !gmtime_r(&now, &utc)) return true;
    if (utc.tm_year < 100 || utc.tm_year > 199) return true;
    int length = snprintf(timestamp, sizeof(timestamp), "%04u-%02u-%02uT%02u:%02u:%02uZ",
        (unsigned)utc.tm_year + 1900U, (unsigned)utc.tm_mon + 1U, (unsigned)utc.tm_mday,
        (unsigned)utc.tm_hour, (unsigned)utc.tm_min, (unsigned)utc.tm_sec);
    if (length != 20) return true;
    return cJSON_AddStringToObject(root, "updated_at", timestamp) != NULL;
}

void panel_ble_aux_tick(void)
{
    static uint32_t next_ms;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (!ble_prov_is_connected() || ota_service_is_busy()) {
        next_ms = now;
        return;
    }
    if ((int32_t)(now - next_ms) < 0) return;
    next_ms = now + 1000U;
    snapshot = panel_ble_snapshot();
    if (!snapshot) return;
    cJSON *root = cJSON_CreateObject();
    if (!root) return;
    uint32_t stamp = panel_stamp(snapshot, 0x42);
    bool ok = cJSON_AddStringToObject(root, "device_sn", g_device_sn) && add_timestamp(root) &&
        cJSON_AddBoolToObject(root, "online", panel_online(snapshot)) &&
        add_optional(root, "age_ms", stamp != 0, snapshot->now_ms - stamp) &&
        add_register(root, "load_power", 0x5b, 1, true);
    cJSON *ac = ok ? cJSON_AddObjectToObject(root, "ac") : NULL;
    ok = ac && add_register(ac, "voltage", 0x58, 10, false) &&
        add_register(ac, "current", 0x59, 100, false) &&
        add_register(ac, "power", 0x5b, 1, true) &&
        add_register(ac, "frequency", 0x5a, 100, false) &&
        add_register(ac, "load_percent", 0x5d, 10, false);
    cJSON *battery = ok ? cJSON_AddObjectToObject(root, "battery") : NULL;
    cJSON *pv = battery && add_live_battery(battery) ? cJSON_AddObjectToObject(root, "pv") : NULL;
    cJSON *status = pv && add_live_pv(pv) ? cJSON_AddObjectToObject(root, "sys_status") : NULL;
    cJSON *energy = status && add_live_status(status) ? cJSON_AddObjectToObject(root, "energy") : NULL;
    ok = energy && add_register(energy, "daily_pv", 0x9c, 10, false) &&
        add_register(energy, "total_pv", 0x9d, 10, false);
    char json[600];
    if (ok && cJSON_PrintPreallocated(root, json, sizeof(json), false))
        ble_ct_telemetry_notify(json, (uint16_t)strlen(json));
    cJSON_Delete(root);
}
