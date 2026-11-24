#include "direct_indicators.h"

/* L10 DSP CMD03 fields from ARM App(10) SystemManage.h. The native mode
 * numbers and current sign differ from those of the Modbus panel. */
enum {
    DSP_GRID_VALID = 1U << 0,
    DSP_AC_RELAY = 1U << 7,
    DSP_LOAD_RELAY = 1U << 9,
    DSP_PRECHARGE = 1U << 12,
    DSP_MODE_AC_TO_LOAD = 16,
    DSP_BATTERY_VOLTAGE_STATE = 3,
    DSP_BATTERY_VOLTAGE_WARNING = 3,
    /* Cell voltage, pack voltage, temperature and shutdown warnings. */
    DSP_BMS_BATTERY_LIMIT = 0x1f9e,
    DSP_BMS_BATTERY_DISCONNECTED = 1U << 14,
};

static panel_led_t run_led(const direct_dsp_status_t *status)
{
    const uint16_t state = status->inverter_status[0];
    if (!(state & DSP_LOAD_RELAY)) {
        return (panel_led_t){LED_YELLOW, (state & DSP_PRECHARGE) ? LED_STEADY : LED_BREATHE};
    }
    const bool grid = (state & DSP_GRID_VALID) &&
        ((state & DSP_AC_RELAY) || status->mode == DSP_MODE_AC_TO_LOAD);
    return (panel_led_t){grid ? LED_GREEN : LED_BLUE, LED_STEADY};
}

static panel_led_t battery_led(const direct_dsp_status_t *status, const RunParamDef *data)
{
    const uint16_t bms_warning = status->warning[2];
    if (!(status->inverter_status[1] & DSP_BATTERY_VOLTAGE_STATE) ||
        (bms_warning & DSP_BMS_BATTERY_DISCONNECTED)) {
        return (panel_led_t){LED_OFF, LED_STEADY};
    }
    if ((status->warning[1] & DSP_BATTERY_VOLTAGE_WARNING) ||
        (bms_warning & DSP_BMS_BATTERY_LIMIT)) {
        return (panel_led_t){LED_YELLOW, LED_BREATHE};
    }
    if (data->BatChgCurr) return (panel_led_t){LED_GREEN, LED_BREATHE};
    if (data->BatDischgCurr) return (panel_led_t){LED_BLUE, LED_BREATHE};
    return (panel_led_t){data->BatterySOC == 100 ? LED_GREEN : LED_BLUE, LED_STEADY};
}

void direct_indicators_policy(const direct_dsp_status_t *last_status, bool status_fresh,
                              const RunParamDef *measurement, const panel_led_network_t *network,
                              uint32_t uptime_ms, panel_led_t out[PANEL_LED_COUNT])
{
    for (unsigned i = 0; i < PANEL_LED_COUNT; ++i)
        out[i] = (panel_led_t){LED_OFF, LED_STEADY};

    const bool fault = last_status && last_status->fault;
    const bool warning = last_status &&
        (last_status->warning[0] || last_status->warning[1] || last_status->warning[2]);
    if (fault) out[LED_ALM] = (panel_led_t){LED_RED, LED_STEADY};
    else if (warning || (uptime_ms >= 5000U && (!status_fresh || !measurement)))
        out[LED_ALM] = (panel_led_t){LED_YELLOW, LED_STEADY};

    if (last_status && status_fresh) {
        if (!fault) out[LED_RUN] = run_led(last_status);
        if (measurement) out[LED_CHG] = battery_led(last_status, measurement);
    }
    panel_indicators_apply_network(network, out);
}
