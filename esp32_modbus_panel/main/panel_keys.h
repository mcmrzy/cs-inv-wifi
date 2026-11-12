#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
esp_err_t panel_keys_init(void);
bool panel_keys_next(uint8_t *key);
