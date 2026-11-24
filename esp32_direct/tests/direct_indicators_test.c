#include "direct_indicators.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void expect(panel_led_t led, panel_led_color_t color, panel_led_pattern_t pattern)
{
    assert(led.color == color && led.pattern == pattern);
}

int main(void)
{
    panel_led_t leds[PANEL_LED_COUNT];
    panel_led_network_t net = {.provisioning = true};
    direct_indicators_policy(NULL, false, NULL, &net, 4999, leds);
    expect(leds[LED_RUN], LED_OFF, LED_STEADY);
    expect(leds[LED_ALM], LED_OFF, LED_STEADY);
    expect(leds[LED_NET], LED_BLUE, LED_BREATHE);
    direct_indicators_policy(NULL, false, NULL, &net, 5000, leds);
    expect(leds[LED_ALM], LED_YELLOW, LED_STEADY);

    direct_dsp_status_t status = {.inverter_status = {1U << 12, 1}};
    RunParamDef measurement = {.BatterySOC = 80, .BatChgCurr = 1};
    net = (panel_led_network_t){.enabled = true, .wifi = true, .cloud = true};
    direct_indicators_policy(&status, true, &measurement, &net, 6000, leds);
    expect(leds[LED_RUN], LED_YELLOW, LED_STEADY);
    expect(leds[LED_CHG], LED_GREEN, LED_BREATHE);
    expect(leds[LED_NET], LED_GREEN, LED_STEADY);
    expect(leds[LED_ALM], LED_OFF, LED_STEADY);

    status.inverter_status[0] = (1U << 9) | (1U << 7) | 1U;
    direct_indicators_policy(&status, true, &measurement, &net, 6000, leds);
    expect(leds[LED_RUN], LED_GREEN, LED_STEADY);

    status.inverter_status[0] = 1U << 9;
    measurement.BatChgCurr = 0;
    measurement.BatDischgCurr = 1;
    status.warning[0] = 1;
    direct_indicators_policy(&status, true, &measurement, &net, 6000, leds);
    expect(leds[LED_RUN], LED_BLUE, LED_STEADY);
    expect(leds[LED_CHG], LED_BLUE, LED_BREATHE);
    expect(leds[LED_ALM], LED_YELLOW, LED_STEADY);
    uint8_t pixels[PANEL_LED_BYTES];
    const uint8_t expected[] = {0, 0, 24, 0, 0, 24, 24, 0, 0, 12, 24, 0};
    panel_indicators_grb(leds, 1000, 24, pixels);
    assert(memcmp(pixels, expected, sizeof(pixels)) == 0);

    status.warning[1] = 1;
    direct_indicators_policy(&status, true, &measurement, &net, 6000, leds);
    expect(leds[LED_CHG], LED_YELLOW, LED_BREATHE);
    status.warning[2] = 1U << 14;
    direct_indicators_policy(&status, true, &measurement, &net, 6000, leds);
    expect(leds[LED_CHG], LED_OFF, LED_STEADY);

    status.fault = 1;
    net.ble = true;
    direct_indicators_policy(&status, false, NULL, &net, 12000, leds);
    expect(leds[LED_RUN], LED_OFF, LED_STEADY);
    expect(leds[LED_CHG], LED_OFF, LED_STEADY);
    expect(leds[LED_NET], LED_BLUE, LED_STEADY);
    expect(leds[LED_ALM], LED_RED, LED_STEADY);
    status = (direct_dsp_status_t){.inverter_status = {0, 1}};
    net = (panel_led_network_t){.enabled = true, .reconnecting = true};
    direct_indicators_policy(&status, true, NULL, &net, 12000, leds);
    expect(leds[LED_RUN], LED_YELLOW, LED_BREATHE);
    expect(leds[LED_NET], LED_RED, LED_BREATHE);
    expect(leds[LED_CHG], LED_OFF, LED_STEADY);
    expect(leds[LED_ALM], LED_YELLOW, LED_STEADY);

    measurement.BatDischgCurr = 0;
    measurement.BatterySOC = 100;
    direct_indicators_policy(&status, true, &measurement, NULL, 12000, leds);
    expect(leds[LED_CHG], LED_GREEN, LED_STEADY);
    expect(leds[LED_ALM], LED_OFF, LED_STEADY);
    puts("PASS: L10 RUN/CHG/NET/ALM states, DSP current direction, stale fault and GRB order");
    return 0;
}
