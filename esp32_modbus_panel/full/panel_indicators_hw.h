#pragma once
#include "panel_indicators.h"
bool full_indicators_init(int gpio);
void full_indicators_set_brightness(uint8_t brightness);
void full_indicators_tick(const panel_led_network_t *network);
