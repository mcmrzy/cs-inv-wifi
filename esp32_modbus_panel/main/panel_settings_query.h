#pragma once
#include "panel_control.h"
typedef struct {
    uint16_t addresses[PANEL_SETTING_READ_SLOTS],last_reg,last_raw;
    uint8_t count,cursor,last_slot;
    bool fallback,flags_read;
    uint32_t context_generation;
} panel_settings_query_t;
void panel_settings_query_start(panel_settings_query_t *,panel_snapshot_t *,const uint16_t *,unsigned);
/* Each requested value is read once. A tick sends at most one read. */
bool panel_settings_query_tick(panel_settings_query_t *,panel_snapshot_t *,const panel_control_io_t *);
