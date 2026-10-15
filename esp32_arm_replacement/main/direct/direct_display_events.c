#include "direct_display_events.h"
#include <string.h>
static void bytes(uint32_t *hash,const void *source,size_t size){
 const uint8_t *p=source;for(size_t i=0;i<size;i++){*hash^=p[i];*hash*=16777619U;}
}
static void text(uint32_t *hash,const char *value){
 if(value)bytes(hash,value,strlen(value));
 uint8_t end=0;bytes(hash,&end,1);
}
uint32_t direct_display_signature(const direct_ui_state_t *u,const direct_ui_data_t *d,const direct_ui_view_t *v){
 uint32_t hash=2166136261U;
#define U(f) bytes(&hash,&u->f,sizeof(u->f))
#define D(f) bytes(&hash,&d->f,sizeof(d->f))
#define R(f) bytes(&hash,&d->run.f,sizeof(d->run.f))
 U(page);U(parent_page);U(group);U(selected);U(detail_page);U(main_mode);
 U(menu_selected);U(submenu_selected);U(editing);U(pending);U(confirm);U(draft);
 text(&hash,u->message);D(theme_light);D(language_zh);
 const char *hints[6];direct_ui_key_hints(u,d,hints);
 for(unsigned i=0;i<6;i++)text(&hash,hints[i]);
 if(u->page==UI_HOME && !u->main_mode){
  D(network_online);D(clock_valid);D(hour);D(minute);D(dsp_online);D(dsp_display_held);
  if(d->dsp_online || d->dsp_display_held){
   R(ACInWatt);R(GridVolt);R(OutputWatt);R(ACOutputVolt);R(Ppv);R(BatterySOC);R(BatVolt);
   bool charge=d->run.BatChgCurr!=0,discharge=d->run.BatDischgCurr!=0;
   bytes(&hash,&charge,sizeof(charge));bytes(&hash,&discharge,sizeof(discharge));
  }
 }else{
  bytes(&hash,v,sizeof(*v));
  if(u->page==UI_PARAMETERS || u->page==UI_ALARM || u->page==UI_HOME){D(dsp_online);D(status_valid);D(dsp_display_held);D(dsp_status_held);}
  if(u->page==UI_CONFIG || u->page==UI_REVIEW){D(config);D(valid);D(writable);}
  if(u->page==UI_BMS){
   D(bms_online);
   if(d->bms_online && u->detail_page){D(bms.soc_percent);D(bms.voltage_10mv);D(bms.cell_mv);}
  }
  if(u->page==UI_UPGRADE){D(ota.state);D(ota.progress);}
 }
#undef U
#undef D
#undef R
 return hash;
}
