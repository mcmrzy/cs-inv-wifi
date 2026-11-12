#pragma once
#include "panel_types.h"

/* Initialize the full-panel handlers and poll the async bus receipt from the
 * same task that owns ble_ct_cmd_tick(). */
void panel_ble_init(void);
void panel_ble_tick(void);
/* Main-task-only scratch, overwritten by the next native BLE operation. */
panel_snapshot_t *panel_ble_snapshot(void);
struct cJSON;
bool panel_ble_read_uint(const struct cJSON *params, const char *key,
                         uint32_t max, uint32_t *out);
bool panel_ble_english(const struct cJSON *params);
