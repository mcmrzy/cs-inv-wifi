#ifndef DIRECT_HOME_H
#define DIRECT_HOME_H
#include "direct_ui.h"
typedef enum {
 DIRECT_HOME_UNKNOWN, DIRECT_HOME_IDLE, DIRECT_HOME_CHARGING,
 DIRECT_HOME_DISCHARGING
} direct_home_battery_state_t;
typedef struct {
 char grid_power[16],grid_voltage[16],pv_power[16],pv_voltage[16];
 char battery_power[16],battery_voltage[16],battery_soc[16];
 char load_power[16],load_voltage[16];
 uint32_t battery_watts;
 direct_home_battery_state_t battery_state;
} direct_home_values_t;
void direct_home_values(const direct_ui_data_t *,direct_home_values_t *);
/* -1 disconnected, -2 AP connected with unknown RSSI, 0..3 signal strength. */
int direct_home_wifi_level(const direct_ui_data_t *);
bool direct_home_animation_active(const direct_ui_state_t *,const direct_ui_data_t *);
uint8_t direct_home_animation_phase(const direct_ui_state_t *,const direct_ui_data_t *,uint32_t now_ms);
#endif
