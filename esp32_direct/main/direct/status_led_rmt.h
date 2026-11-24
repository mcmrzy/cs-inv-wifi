#pragma once

#include "status_led.h"

/* Initialize and send from one owner at a time. Initialization clears the chain. */
bool status_led_init(int gpio);
bool status_led_is_ready(void);
void status_led_send(const uint8_t pixels[PANEL_LED_BYTES]);
