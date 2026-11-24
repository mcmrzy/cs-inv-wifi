#pragma once

#include "panel_alarm.h"
#include "panel_sound.h"

typedef struct {
    uint16_t inverter[4];
    uint16_t bms_warning;
    uint16_t bms_protection;
    bool inverter_seen;
    bool bms_seen;
    bool recovery_armed;
} panel_sound_alarm_state_t;

/* Each source retains its last observed bits until a fresh sample replaces them. */
panel_sound_effect_t panel_sound_alarm_update(panel_sound_alarm_state_t *state,
    const panel_alarm_sample_t *inverter, bool bms_fresh,
    uint16_t bms_warning, uint16_t bms_protection, bool audible);
