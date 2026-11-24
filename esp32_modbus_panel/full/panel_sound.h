#pragma once
#include <stdint.h>

typedef enum {
    PANEL_SOUND_NONE=0, PANEL_SOUND_BOOT, PANEL_SOUND_KEY,
    PANEL_SOUND_CONFIRM, PANEL_SOUND_BACK, PANEL_SOUND_SUCCESS,
    PANEL_SOUND_ERROR, PANEL_SOUND_WARNING, PANEL_SOUND_FAULT,
    PANEL_SOUND_RECOVERY, PANEL_SOUND_COUNT
} panel_sound_effect_t;

/* Requests never wait for playback. Higher-priority sounds replace lower ones.
 * All-clear waits for the active alert, unless a new alert or mute cancels it. */
void panel_sound_play(panel_sound_effect_t effect);
/* Physical press edges only: BACK=1, ENTER=2, directions=3..6. */
void panel_sound_key(uint8_t key);
