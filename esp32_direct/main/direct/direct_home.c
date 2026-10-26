#include "direct_home.h"
#include <stdio.h>
#include <string.h>

static void power(char out[16],int32_t value){
 uint32_t n=value<0?(uint32_t)(-(int64_t)value):(uint32_t)value;
 const char *sign=value<0?"-":"";
 if(n>=1000000U)snprintf(out,16,"%s%luMW",sign,(unsigned long)((n+500000U)/1000000U));
 else if(n>=10000U){
  uint32_t tenth=(n+50U)/100U;
  snprintf(out,16,"%s%lu.%lukW",sign,(unsigned long)(tenth/10U),(unsigned long)(tenth%10U));
 }else snprintf(out,16,"%s%luW",sign,(unsigned long)n);
}
void direct_home_values(const direct_ui_data_t *d,direct_home_values_t *v){
 memset(v,0,sizeof(*v));
 strcpy(v->grid_power,"--W");strcpy(v->grid_voltage,"--V");
 strcpy(v->pv_power,"--W");strcpy(v->pv_voltage,"--V");
 strcpy(v->battery_power,"--W");strcpy(v->battery_voltage,"--V");strcpy(v->battery_soc,"--%");
 strcpy(v->load_power,"--W");strcpy(v->load_voltage,"--V");
 if(!d->dsp_online && !d->dsp_display_held)return;
 const RunParamDef *p=&d->run;
 power(v->grid_power,p->ACInWatt);power(v->pv_power,p->Ppv);power(v->load_power,p->OutputWatt);
 snprintf(v->grid_voltage,16,"%uV",p->GridVolt);snprintf(v->pv_voltage,16,"%uV",p->Vpv1);
 snprintf(v->load_voltage,16,"%uV",p->ACOutputVolt);
 /* Keep the narrow auxiliary column usable for corrupt/out-of-range raw data. */
 if(p->BatVolt>9999)snprintf(v->battery_voltage,16,"%uV",(p->BatVolt+5U)/10U);
 else snprintf(v->battery_voltage,16,"%u.%uV",p->BatVolt/10U,p->BatVolt%10U);
 if(p->BatterySOC<=100)snprintf(v->battery_soc,16,"%u%%",p->BatterySOC);
 if(!(p->BatChgCurr && p->BatDischgCurr)){
  /* Both operands are from this same DSP sample, in 0.1V and 0.1A. */
  uint32_t current=p->BatChgCurr?p->BatChgCurr:p->BatDischgCurr;
  v->battery_watts=((uint32_t)p->BatVolt*current+50U)/100U;
  power(v->battery_power,(int32_t)v->battery_watts);
  v->battery_state=p->BatChgCurr?DIRECT_HOME_CHARGING:p->BatDischgCurr?DIRECT_HOME_DISCHARGING:DIRECT_HOME_IDLE;
 }
}
int direct_home_wifi_level(const direct_ui_data_t *d){
 if(!d->wifi_connected)return -1;
 if(!d->wifi_rssi_valid || d->wifi_rssi_dbm>=0 || d->wifi_rssi_dbm < -127)return -2;
 return d->wifi_rssi_dbm>=-55?3:d->wifi_rssi_dbm>=-70?2:d->wifi_rssi_dbm>=-82?1:0;
}
bool direct_home_animation_active(const direct_ui_state_t *u,const direct_ui_data_t *d){
 /* A display-held real sample keeps its direction and motion. Freshness remains
  * independent in telemetry, alarms and parameter-write authorization. */
 if(u->page!=UI_HOME || u->main_mode || !(d->dsp_online || d->dsp_display_held) || d->ota_busy)return false;
 const RunParamDef *p=&d->run;
 return p->Ppv || p->ACInWatt || p->OutputWatt ||
  (p->BatVolt && ((p->BatChgCurr!=0)!=(p->BatDischgCurr!=0)));
}
uint8_t direct_home_animation_phase(const direct_ui_state_t *u,const direct_ui_data_t *d,uint32_t now_ms){
 return direct_home_animation_active(u,d)?(uint8_t)((now_ms/200U)%16U):0;
}
