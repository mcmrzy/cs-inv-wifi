#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t last_attempt_ms, baseline_errors;
    unsigned auto_attempts;
} direct_lcd_recovery_t;

/* Two automatic attempts per boot. A human can retry after exhaustion.
 * Timeouts/pixel inactivity are not proof of a display communication fault. */
static inline bool direct_lcd_recovery_due(const direct_lcd_recovery_t *s,uint32_t now,
                                         uint32_t errors,bool initialized,bool manual,bool ota)
{
    if(ota || (uint32_t)(now-s->last_attempt_ms)<3000U)return false;
    return manual || (s->auto_attempts<2U && (!initialized || errors!=s->baseline_errors));
}
