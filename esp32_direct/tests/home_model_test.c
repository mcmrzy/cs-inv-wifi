#include "direct_home.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int main(void){
 direct_ui_state_t u={.page=UI_HOME};direct_ui_data_t d={0};direct_home_values_t v;
 direct_home_values(&d,&v);assert(!strcmp(v.battery_power,"--W"));assert(!strcmp(v.pv_voltage,"--V"));
 d.dsp_online=true;d.run.BatVolt=524;d.run.BatChgCurr=125;d.run.Vpv1=368;d.run.Vpv2=372;d.run.BatterySOC=78;
 direct_home_values(&d,&v);assert(!strcmp(v.battery_power,"655W"));assert(v.battery_state==DIRECT_HOME_CHARGING);assert(!strcmp(v.pv_voltage,"368V"));
 assert(direct_home_animation_active(&u,&d));assert(direct_home_animation_phase(&u,&d,200)==1);
 d.run.BatChgCurr=0;d.run.BatDischgCurr=125;direct_home_values(&d,&v);assert(!strcmp(v.battery_power,"655W"));assert(v.battery_state==DIRECT_HOME_DISCHARGING);
 d.run.BatChgCurr=125;direct_home_values(&d,&v);assert(!strcmp(v.battery_power,"--W"));assert(v.battery_state==DIRECT_HOME_UNKNOWN);
 d.run.BatChgCurr=0;d.run.BatDischgCurr=0;direct_home_values(&d,&v);assert(!strcmp(v.battery_power,"0W"));assert(v.battery_state==DIRECT_HOME_IDLE);assert(!direct_home_animation_active(&u,&d));
 d.run.Ppv=1;assert(direct_home_animation_active(&u,&d));u.page=UI_MENU;assert(!direct_home_animation_active(&u,&d));u.page=UI_HOME;u.main_mode=1;assert(!direct_home_animation_active(&u,&d));u.main_mode=0;
 d.ota_busy=true;assert(!direct_home_animation_active(&u,&d));d.ota_busy=false;
 d.dsp_online=false;d.dsp_display_held=true;direct_home_values(&d,&v);assert(!strcmp(v.pv_voltage,"368V"));assert(v.battery_state==DIRECT_HOME_IDLE);assert(direct_home_animation_active(&u,&d));
 assert(direct_home_animation_phase(&u,&d,600)==3);
 d.ota_busy=true;assert(!direct_home_animation_active(&u,&d));d.ota_busy=false;
 d.run.Ppv=0;assert(!direct_home_animation_active(&u,&d));d.run.Ppv=1;
 d.dsp_display_held=false;direct_home_values(&d,&v);assert(!strcmp(v.pv_power,"--W"));
 d.dsp_online=true;d.run.BatVolt=UINT16_MAX;d.run.BatChgCurr=UINT16_MAX;direct_home_values(&d,&v);assert(v.battery_watts==42948362U);assert(!strcmp(v.battery_power,"43MW"));d.run.BatterySOC=101;direct_home_values(&d,&v);assert(!strcmp(v.battery_soc,"--%"));
 assert(direct_home_wifi_level(&d)==-1);d.wifi_connected=true;assert(direct_home_wifi_level(&d)==-2);d.wifi_rssi_valid=true;d.wifi_rssi_dbm=-48;assert(direct_home_wifi_level(&d)==3);d.wifi_rssi_dbm=-64;assert(direct_home_wifi_level(&d)==2);d.wifi_rssi_dbm=-77;assert(direct_home_wifi_level(&d)==1);d.wifi_rssi_dbm=-88;assert(direct_home_wifi_level(&d)==0);
 puts("PASS home model: real DSP watts/units, PV1 only, zero/missing/stale, direction conflict, arithmetic, gated motion and Wi-Fi levels");
}
