#pragma once
#include "panel_types.h"

typedef struct {
    uint16_t address, read_address; /* 0xFFFF means no verified setting readback */
    const char *label, *unit;
    uint16_t divisor;
} panel_setting_t;
size_t panel_setting_count(void);
#define PANEL_SETTING_GROUPS 9U
#define PANEL_SETTING_GROUP_BMS 5U
const char *panel_setting_group_name(unsigned group);
unsigned panel_setting_group_for(uint16_t address);
unsigned panel_setting_group_count(unsigned group);
unsigned panel_setting_group_index(unsigned group,unsigned position);
unsigned panel_setting_group_position(unsigned group,unsigned index);
typedef enum {
    PANEL_ACCESS_EDITABLE, PANEL_ACCESS_OFFLINE, PANEL_ACCESS_STATE,
    PANEL_ACCESS_MODEL, PANEL_ACCESS_BATTERY, PANEL_ACCESS_STANDBY,
    PANEL_ACCESS_FIXED, PANEL_ACCESS_LEAD_ONLY, PANEL_ACCESS_DEFINITION,
    PANEL_ACCESS_UNSUPPORTED, PANEL_ACCESS_PROFILE_CONFLICT, PANEL_ACCESS_READ_ONLY,
    PANEL_ACCESS_MODE, PANEL_ACCESS_MODE_FIXED
} panel_setting_access_t;
panel_setting_access_t panel_setting_access(const panel_snapshot_t *,uint16_t address);
const char *panel_setting_access_reason(panel_setting_access_t access);
const char *panel_setting_restriction(const panel_snapshot_t *,uint16_t address);
const panel_setting_t *panel_setting(size_t index);
const panel_setting_t *panel_setting_find(uint16_t address);
/* Model-aware address for reading the current setting, or 0xFFFF if unavailable. */
uint16_t panel_setting_current_address(const panel_snapshot_t *,const panel_setting_t *);
/* SOC percentage readback also needs its independently readable enable bit. */
uint16_t panel_setting_current_flags(uint16_t address);
/* Convert a documented read register into the corresponding write encoding. */
bool panel_setting_decode_readback(uint16_t address,uint16_t raw,uint16_t *value);
bool panel_setting_decode_current(const panel_snapshot_t *,uint16_t address,uint16_t raw,uint16_t *value);
const char *panel_profile_name(panel_profile_t profile);
/* Diagnostic candidate only. Conflicting or unrecognised encodings are unknown. */
panel_profile_t panel_detect_profile(uint16_t id38, uint16_t id39);
bool panel_setting_allowed(const panel_snapshot_t *s, uint16_t address, uint16_t value);
uint16_t panel_battery_suggestion(const panel_snapshot_t *s, uint16_t address,
                                 uint16_t charge_deciamp, uint16_t charge_decivolt,
                                 uint16_t discharge_decivolt);
/* Finds the next permitted raw value, without wrapping. An unknown current value
 * starts at the lowest/highest allowed value according to direction. */
bool panel_setting_step(const panel_snapshot_t *s, uint16_t address, uint16_t current,
                        int direction, uint16_t *next);
void panel_setting_format(const panel_snapshot_t *s, uint16_t address,
                          uint16_t raw, char *out, size_t size);
void panel_setting_format_profile(panel_profile_t,uint16_t address,
                                  uint16_t raw,char *out,size_t size);
