#include "direct_display_events.h"
#include "direct_display_sample.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint32_t signature(direct_ui_state_t *u,direct_ui_data_t *d){
 direct_ui_view_t v;direct_ui_build(u,d,&v);return direct_display_signature(u,d,&v);
}
int main(void){
 direct_ui_state_t u;direct_ui_init(&u);
 direct_ui_data_t d={.dsp_online=true,.network_online=true};d.run.BatVolt=497;
 uint32_t original=signature(&u,&d);unsigned renders=1;
 for(unsigned i=1;i<6000;i++){
  d.measurement_age_ms=i%2000;d.second=i%60;d.polls=i;d.errors=i;
  /* Hidden values and new reception times with identical visible data. */
  d.run.Ppv1=i;d.run.Ibus=i;uint32_t current=signature(&u,&d);
  if(current!=original || direct_display_should_render(i*20,100,false))renders++;
 }
 assert(renders==1); /* Two minutes without a screen event: zero redraws. */
 d.run.BatVolt=498;assert(signature(&u,&d)!=original);d.run.BatVolt=497;
 d.run.OutputWatt=0;d.run.BatChgCurr=10;uint32_t charging=signature(&u,&d);
 d.run.BatChgCurr=20;assert(signature(&u,&d)==charging);
 d.run.BatChgCurr=0;assert(signature(&u,&d)==original);
 d.hour=1;assert(signature(&u,&d)!=original);d.hour=0;
 d.network_online=false;assert(signature(&u,&d)!=original);d.network_online=true;
 d.dsp_display_held=true;uint32_t held=signature(&u,&d);
 d.measurement_age_ms=4999;assert(signature(&u,&d)==held);
 d.dsp_online=false;assert(signature(&u,&d)!=held);d.dsp_online=true;d.dsp_display_held=false;
 d.dsp_online=false;d.dsp_display_held=true;
 uint32_t stale=signature(&u,&d);
 for(unsigned i=5000;i<3600000;i+=1000){d.measurement_age_ms=i;assert(signature(&u,&d)==stale);}
 d.run.Warning=1;d.status.fault=1;d.dsp_status_held=true;
 assert(!direct_ui_has_alarm(&d)); /* Cached data cannot become live alarms. */
 d.dsp_online=true;d.dsp_display_held=false;d.dsp_status_held=false;
 u.dirty=true;assert(direct_display_should_render(1,100,u.dirty));
 u.page=UI_PARAMETERS;u.detail_page=0;uint32_t params=signature(&u,&d);
 d.run.Buck1Curr++;assert(signature(&u,&d)!=params);
 u.page=UI_BMS;u.detail_page=1;d.bms_online=true;uint32_t bms=signature(&u,&d);
 d.bms.cell_mv[5]=3300;assert(signature(&u,&d)!=bms);
 u.page=UI_UPGRADE;d.ota.progress=1;uint32_t ota=signature(&u,&d);
 d.ota.progress=2;assert(signature(&u,&d)!=ota);
 puts("PASS display events: two minutes unchanged means zero repaint; visible data/network/minute/page/keys/BMS/OTA events repaint; timestamps/counters/age alone do not");
}
