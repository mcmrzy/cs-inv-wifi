#pragma once
#include "panel_types.h"
#include "panel_alarm.h"
#include "esp_err.h"
esp_err_t panel_bus_start(void);
bool panel_bus_snapshot(panel_snapshot_t *out);
bool panel_bus_submit(const panel_command_t *command);
bool panel_bus_submit_sequence(const panel_command_t *command,uint32_t *sequence);
/* Admission checks the requested inverter context while holding the bus lock. */
bool panel_bus_submit_in_context(const panel_command_t *command,uint32_t context,uint32_t *sequence);
void panel_bus_set_quiet(bool quiet);
bool panel_bus_software_healthy(void);
void panel_bus_request_ota_diagnostics(void);
void panel_bus_request_settings(const uint16_t *addresses,unsigned count);
void panel_bus_request_ble_setting(uint16_t address);
uint32_t panel_bus_context_generation(void);
bool panel_bus_alarm_sample(panel_alarm_sample_t *out);
