#pragma once
#include "panel_types.h"
#include "direct_link.h"
#include "direct_ui.h"
#define FULL_MB_MEASUREMENT_FRESH_MS 2000U
enum { PANEL_HOME_GRID, PANEL_HOME_SOLAR, PANEL_HOME_BATTERY, PANEL_HOME_LOAD, PANEL_HOME_COUNT };
enum { PANEL_HOME_POWER=1U, PANEL_HOME_VOLTAGE=2U, PANEL_HOME_SOC=4U };
typedef struct {
    int32_t watts;
    uint16_t decivolts;
    uint8_t available,fresh;
} panel_home_meter_t;
typedef struct {
    panel_home_meter_t meter[PANEL_HOME_COUNT];
    uint8_t battery_soc;
    int8_t battery_direction; /* -1 charging, +1 discharging, 0 unknown or idle. */
} panel_home_readings_t;
/* LCD readings use independent evidence. After a pause, only newer samples count as live. */
void full_mb_home(const panel_snapshot_t *,const uint32_t *after,panel_home_readings_t *);

/* Internal cloud command selector outside the scalar configuration indices. */
#define FULL_MB_AC_POWER_INDEX 0xff00U

/* Cloud-only selectors after the legacy wire fields in a Modbus snapshot. */
#define FULL_MB_POWER_SAVING 0xff10U
#define FULL_MB_AUTO_FREQUENCY 0xff11U
#define FULL_MB_BATTERY_OPEN_ALARM 0xff12U
#define FULL_MB_INVERTER_MUTE 0xff13U
#define FULL_MB_CLOUD_CONFIG_COUNT (CTRL_PARAM_COUNT + 4U)
bool full_modbus_cloud_config_snapshot(uint16_t *, bool *, uint32_t *);
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

/* The V3 cloud frame can publish available words when the full DSP view is incomplete. */
bool full_mb_partial_telemetry(const panel_snapshot_t *,RunParamDef *,bool words[RUN_PARAM_WORDS]);
bool direct_modbus_get_partial_telemetry(RunParamDef *,bool words[RUN_PARAM_WORDS]);
