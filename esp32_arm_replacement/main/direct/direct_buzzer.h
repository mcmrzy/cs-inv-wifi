#pragma once
#include <stdbool.h>
#include "esp_err.h"

/* Application entry configures GPIO off first; initialize after NVS is ready. */
esp_err_t direct_buzzer_init(void);
void direct_buzzer_set_enabled(bool enabled);
/* Called by the scanner after a key is queued, never inside its sampling lock. */
void direct_buzzer_pulse(void);
