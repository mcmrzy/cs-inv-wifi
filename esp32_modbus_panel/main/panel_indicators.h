#pragma once
#include "panel_types.h"
#include "status_led.h"

void panel_indicators_policy(const panel_snapshot_t *,const panel_led_network_t *,uint32_t now_ms,panel_led_t out[4]);
