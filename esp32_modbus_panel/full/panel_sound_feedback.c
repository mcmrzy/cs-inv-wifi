#include "panel_sound_feedback.h"

panel_sound_effect_t panel_sound_alarm_update(panel_sound_alarm_state_t *state,
    const panel_alarm_sample_t *inverter, bool bms_fresh,
    uint16_t bms_warning, uint16_t bms_protection, bool audible)
{
    bool new_fault = false;
    bool new_warning = false;
    const bool inverter_valid = inverter && inverter->reachable && !inverter->paused;
    if (inverter_valid) {
        for (unsigned i = 0; i < 4; i++) {
            if (!(inverter->fresh & (1U << i))) continue;
            state->inverter_seen = true;
            uint16_t incoming = inverter->words[i];
            if (incoming & ~state->inverter[i]) {
                if (i < 2) new_fault = true;
                else new_warning = true;
            }
            state->inverter[i] = incoming;
        }
    }
    if (bms_fresh) {
        state->bms_seen = true;
        new_fault |= (bms_protection & ~state->bms_protection) != 0;
        new_warning |= (bms_warning & ~state->bms_warning) != 0;
        state->bms_protection = bms_protection;
        state->bms_warning = bms_warning;
    }

    if (!audible) {
        state->recovery_armed = false;
        return PANEL_SOUND_NONE;
    }
    if (new_fault || new_warning) {
        state->recovery_armed = true;
        return new_fault ? PANEL_SOUND_FAULT : PANEL_SOUND_WARNING;
    }
    const bool inverter_confirmed = !state->inverter_seen ||
        (inverter_valid && inverter->fresh == 15);
    if (state->recovery_armed && inverter_confirmed && (!state->bms_seen || bms_fresh) &&
        !state->bms_protection && !state->bms_warning &&
        !state->inverter[0] && !state->inverter[1] && !state->inverter[2] && !state->inverter[3]) {
        state->recovery_armed = false;
        return PANEL_SOUND_RECOVERY;
    }
    return PANEL_SOUND_NONE;
}
