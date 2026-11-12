#pragma once
#include "panel_types.h"
#include "direct_link.h"
#include "direct_ui.h"
#define FULL_MB_MEASUREMENT_FRESH_MS 2000U
bool full_mb_measurement(const panel_snapshot_t *,RunParamDef *,uint32_t *received_ms);
typedef enum {FULL_MB_DATA_READY,FULL_MB_DATA_MISSING,FULL_MB_DATA_STALE,FULL_MB_DATA_INVALID} full_mb_data_state_t;
full_mb_data_state_t full_mb_measurement_check(const panel_snapshot_t *,uint16_t *problem_register);
bool full_mb_status(const panel_snapshot_t *,direct_dsp_status_t *,uint32_t *received_ms);
void full_mb_battery(const panel_snapshot_t *,direct_inverter_battery_t *);
bool full_mb_read_config(const panel_snapshot_t *,uint16_t index,bool wire,uint16_t *value);
bool full_mb_make_write(const panel_snapshot_t *,uint16_t index,uint16_t value,bool wire,panel_command_t *);
bool full_mb_config_writable(const panel_snapshot_t *,uint16_t index);
const char *full_mb_registry_model(const panel_snapshot_t *);
const char *full_modbus_registry_model(void);
