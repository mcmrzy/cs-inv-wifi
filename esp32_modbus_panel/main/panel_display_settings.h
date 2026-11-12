#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef struct {uint8_t brightness,timeout_index;} panel_display_settings_t;
/* LED stays in its native 0..255 range; beep_mode is an audible level. */
typedef struct {uint8_t led_brightness,beep_mode;} panel_feedback_settings_t;
#define PANEL_DISPLAY_DEFAULT_BRIGHTNESS 100U
#define PANEL_DISPLAY_TIMEOUT_COUNT 6U
#define PANEL_APPEARANCE_ROWS 7U
enum {PANEL_FEEDBACK_LED_EDIT=1U,PANEL_FEEDBACK_BEEP_EDIT=2U};
enum {PANEL_BEEP_OFF,PANEL_BEEP_LOW,PANEL_BEEP_MEDIUM,PANEL_BEEP_HIGH,PANEL_BEEP_COUNT};
/* Retained source aliases. Duration no longer changes with the selected level. */
#define PANEL_BEEP_SHORT PANEL_BEEP_LOW
#define PANEL_BEEP_NORMAL PANEL_BEEP_MEDIUM
#define PANEL_BEEP_LONG PANEL_BEEP_HIGH
panel_feedback_settings_t panel_feedback_preferences(void);
void panel_feedback_set_preferences(panel_feedback_settings_t settings);
unsigned panel_feedback_beep_ms(unsigned mode);
unsigned panel_feedback_beep_percent(unsigned mode);
panel_display_settings_t panel_display_preferences(void);
void panel_display_set_preferences(panel_display_settings_t settings);
bool panel_display_valid(panel_display_settings_t settings);
uint16_t panel_display_pack(panel_display_settings_t settings);
bool panel_display_unpack(uint16_t packed,panel_display_settings_t *out);
uint32_t panel_display_timeout_ms(panel_display_settings_t settings);
bool panel_display_should_sleep(uint32_t now,uint32_t last_input,panel_display_settings_t settings,bool enabled,bool blocked);
uint32_t panel_display_duty(uint8_t brightness,bool asleep,bool active_high);
