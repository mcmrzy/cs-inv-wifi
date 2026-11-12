#pragma once
#include "panel_display_settings.h"

typedef struct {
    uint16_t alarm,buzzer,blank;
    int16_t timezone;
    uint8_t style;
    panel_display_settings_t display;
    panel_feedback_settings_t feedback;
} panel_preferences_t;
enum { PANEL_PREF_ALARM=1U, PANEL_PREF_BUZZER=2U, PANEL_PREF_BLANK=4U,
       PANEL_PREF_LOCAL=7U, PANEL_PREF_DISPLAY=8U, PANEL_PREF_TIME=16U,
       PANEL_PREF_LED_BRIGHTNESS=32U, PANEL_PREF_BEEP_MODE=64U,
       PANEL_PREF_BRIGHTNESS=128U, PANEL_PREF_TIMEOUT=256U,
       PANEL_PREF_THEME=512U, PANEL_PREF_LANGUAGE=1024U,
       PANEL_PREF_ALL=2047U };
/* One committed record. Updates merge only the caller's field group, so a
 * concurrent phone change cannot overwrite screen/time edits and vice versa. */
bool panel_preferences_get(panel_preferences_t *);
bool panel_preferences_update(const panel_preferences_t *,unsigned fields);
bool panel_preferences_reset(void);
