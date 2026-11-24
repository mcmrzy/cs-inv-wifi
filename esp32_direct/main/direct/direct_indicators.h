#pragma once

#include "arm_param.h"
#include "direct_protocol.h"
#include "status_led.h"

/* The last status can be retained across a DSP link outage. Only a fresh
 * status and measurement may report active output or battery current. */
void direct_indicators_policy(const direct_dsp_status_t *last_status, bool status_fresh,
                              const RunParamDef *measurement, const panel_led_network_t *network,
                              uint32_t uptime_ms, panel_led_t out[PANEL_LED_COUNT]);
