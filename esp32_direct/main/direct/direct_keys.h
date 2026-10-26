#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "direct_key_codec.h"

/* Call once before display initialization; events remain buffered during draws. */
esp_err_t direct_keys_init(void);
/* Never blocks; returns 0 when the queue is empty. */
uint8_t direct_keys_get_event(void);
/* The timestamp is captured by the scanner, independent of rendering latency. */
bool direct_keys_get_timed_event(uint8_t *key, uint32_t *timestamp_ms);
