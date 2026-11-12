#pragma once

#include <stdbool.h>
#include "panel_types.h"
struct cJSON;

const char *panel_ble_aux_read(const char *action, const struct cJSON *params,
                               struct cJSON *actual);
int panel_ble_aux_write(const char *id, double value);
uint16_t panel_ble_battery_suggestion(const panel_snapshot_t *s, uint16_t address);
bool panel_ble_aux_take_preferences_changed(void);
void panel_ble_aux_tick(void);
