#pragma once
#include "panel_control.h"
typedef struct {
    uint16_t addresses[PANEL_SETTING_READ_SLOTS],last_reg,last_raw;
    uint8_t count,cursor;
    bool fallback,was_online,flags_read;
    uint32_t next_round,context_generation;
} panel_settings_query_t;
void panel_settings_query_start(panel_settings_query_t *,panel_snapshot_t *,const uint16_t *,unsigned);
/* At most one read, never a write/sleep. Called between normal live-data polls. */
bool panel_settings_query_tick(panel_settings_query_t *,panel_snapshot_t *,const panel_control_io_t *);
