#include "direct_ui.h"
#include "direct_ui_scene.h"
#include "direct_gfx.h"
#include "direct_gfx_assets.h"
#include "direct_ui_locale.h"
#include "full_ui_bridge.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint16_t full[320*240],strip[320*16+2];
static unsigned images;
static bool zh,light;
static uint8_t focus;
static void render(const char *name,direct_ui_state_t *u,direct_ui_data_t *d){
 direct_ui_view_t v;direct_ui_build(u,d,&v);
 direct_gfx_t gfx;direct_gfx_begin(&gfx,full,320,240,0,0);
 full_ui_scene_draw_frame(&gfx,u,d,&v,focus);
 for(int y=0;y<240;y+=16){
  strip[320*16]=0xdead;strip[320*16+1]=0xbeef;
  direct_gfx_begin(&gfx,strip,320,16,y,0);full_ui_scene_draw_frame(&gfx,u,d,&v,focus);
  assert(!memcmp(full+y*320,strip,320*16*sizeof(uint16_t)));
  assert(strip[320*16]==0xdead && strip[320*16+1]==0xbeef);
 }
 char path[160];snprintf(path,sizeof(path),"tmp/ui_graphical/%s_%s_%s.ppm",name,zh?"zh":"en",light?"light":"dark");
 FILE *f=fopen(path,"wb");assert(f);fprintf(f,"P6\n320 240\n255\n");
 for(unsigned i=0;i<320*240;i++){
  uint16_t p=full[i];uint8_t rgb[]={(p>>11)*255/31,((p>>5)&63)*255/63,(p&31)*255/31};
  assert(fwrite(rgb,1,3,f)==3);
 }
 assert(!fclose(f));images++;printf("PASS render: %s %s %s\n",name,zh?"zh":"en",light?"light":"dark");
}
static direct_ui_data_t data(void){
 direct_ui_data_t d={0};d.language_zh=zh;d.theme_light=light;
 d.dsp_online=true;d.status_valid=true;d.wifi_connected=true;d.network_online=true;
 d.wifi_rssi_valid=true;d.wifi_rssi_dbm=-55;d.clock_valid=true;d.hour=16;d.minute=42;
 d.run.GridVolt=230;d.run.ACInWatt=1800;d.run.Ppv=4200;d.run.Vpv1=368;
 d.run.BatVolt=524;d.run.BatterySOC=78;d.run.BatChgCurr=235;d.run.OutputWatt=3200;d.run.ACOutputVolt=230;
 d.model="CS-MB-PANEL";d.hardware="1.0";d.version="dev";d.sn="MB00001122334455";
 d.year=2026;d.month=11;d.day=3;d.timezone_minutes=480;return d;
}
static void check_locales(void){
 for(unsigned i=0;i<direct_ui_locale_count();i++){
  const char *p=direct_ui_locale_zh(i);assert(p && *p);
  while(*p){uint32_t cp=direct_gfx_utf8_next(&p);assert(direct_gfx_has_glyph(cp));}
 }
 char out[128];const char *s=direct_ui_translate("Bluetooth / Communication",true,out,sizeof(out));
 assert(strstr(s,"蓝牙") && strstr(s,"通信"));
}
int main(void){
 for(unsigned i=0;i<13;i++)assert(direct_ui_group_count(i)==0);
 for(unsigned i=0;i<84;i++)assert(!direct_ui_value_allowed(i,0));
 direct_ui_state_t u;direct_ui_init(&u);direct_ui_data_t d=data();
 const direct_ui_page_t retired[]={UI_CONFIG,UI_REVIEW,UI_PARAMETERS,UI_ALARM,UI_BATTERY,UI_BATTERY_REVIEW};
 for(unsigned i=0;i<sizeof(retired)/sizeof(retired[0]);i++){
  u.page=retired[i];assert(direct_ui_key(&u,1,0,&d).action==UI_ACT_NONE && u.page==UI_HOME);
 }
 int good[]={2024,2,29,23,59,59,480};int64_t seconds;
 assert(direct_ui_time_utc(good,&seconds));good[0]=2023;assert(!direct_ui_time_utc(good,&seconds));
 direct_ui_init(&u);u.page=UI_TIME;int end_month[]={2024,1,31,12,0,0,480};memcpy(u.time_fields,end_month,sizeof(end_month));u.selected=1;
 direct_ui_key(&u,6,0,&d);assert(u.time_fields[1]==2 && u.time_fields[2]==29);
 u.selected=0;direct_ui_key(&u,4,0,&d);assert(u.time_fields[0]==2023 && u.time_fields[2]==28);
 u.selected=2;direct_ui_key(&u,6,0,&d);assert(u.time_fields[2]==28 && strstr(u.message,"limit"));
 direct_ui_view_t zone_view;direct_ui_build(&u,&d,&zone_view);assert(strstr(zone_view.rows[6],"UTC+08:00"));
 for(unsigned language=0;language<2;language++)for(unsigned theme=0;theme<2;theme++){
  zh=language!=0;light=theme!=0;d=data();direct_ui_init(&u);
  render("home_online",&u,&d);uint16_t live[320*26];memcpy(live,full,sizeof(live));
  if(zh&&!light){
   const unsigned grid_forward=137*320+72,grid_reverse=137*320+69;
   uint16_t before=0;
   for(unsigned phase=0;phase<16;phase++){
    char name[32];snprintf(name,sizeof(name),"home_flow_%02u",phase);
    d.home_flow_phase=(uint8_t)phase;render(name,&u,&d);
    if(phase==2)before=full[grid_forward];
    if(phase==3)assert(full[grid_forward]!=before);
   }
   d.run.ACInWatt=-1800;d.home_flow_phase=10;render("home_export_10",&u,&d);
   before=full[grid_reverse];
   d.home_flow_phase=11;render("home_export_11",&u,&d);
   assert(full[grid_reverse]!=before);
   d=data();
  }
  d.dsp_online=false;d.dsp_display_held=true;render("home_last_sample",&u,&d);assert(memcmp(live,full,sizeof(live)));
  uint16_t held[320*240];memcpy(held,full,sizeof(held));d.home_flow_phase=9;render("home_held_phase",&u,&d);
  assert(!memcmp(held,full,sizeof(held)));d=data();
  u.page=UI_MENU;
  for(unsigned i=0;i<direct_ui_menu_count();i++){char name[32];u.selected=i;snprintf(name,sizeof(name),"menu_%u",i);render(name,&u,&d);}
  u.selected=1;focus=4;render("menu_focus",&u,&d);focus=0;
  for(unsigned m=4;m<6;m++)for(unsigned i=0;i<direct_ui_submenu_count(m);i++){
   char name[32];u.page=UI_SUBMENU;u.menu_selected=m;u.selected=i;snprintf(name,sizeof(name),"submenu_%u_%u",m,i);render(name,&u,&d);
  }
  u.page=UI_HELP;u.menu_selected=4;u.detail_page=0;render("help",&u,&d);
  u.menu_selected=5;u.detail_page=1;render("lcd_test_a",&u,&d);u.detail_page=2;render("lcd_test_b",&u,&d);
  u.page=UI_APPEARANCE;u.detail_page=0;u.time_fields[0]=100;u.time_fields[1]=2;
  u.time_fields[2]=24;u.time_fields[3]=2;
  for(unsigned i=0;i<7;i++){u.selected=i;render(i?"appearance_rows":"appearance",&u,&d);}
  u.time_fields[0]=10;u.time_fields[1]=0;render("appearance_never",&u,&d);
  u.page=UI_TIME;int fields[]={2026,11,3,16,42,0,480};memcpy(u.time_fields,fields,sizeof(fields));
  for(unsigned i=0;i<7;i++){u.selected=i;render("time",&u,&d);}
  u.page=UI_FACTORY;
  for(unsigned group=0;group<3;group++)for(unsigned confirm=0;confirm<2;confirm++){
   u.group=group;u.confirm=confirm;render(group==2?"lcd_reset":group==1?"restart":confirm?"factory_yes":"factory_no",&u,&d);
  }
  u.page=UI_PRODUCTION;render("production_pending",&u,&d);d.factory_ready=true;d.factory_locked=true;render("production_written",&u,&d);
  u.page=UI_UPGRADE;u.confirm=false;
  for(unsigned state=0;state<=OTA_STATE_ROLLED_BACK;state++){
   d.ota.state=(OtaState)state;d.ota_busy=state>0 && state<=OTA_STATE_CANCELLING;
   d.ota.source=OTA_SOURCE_LOCAL;d.ota.target=OTA_TARGET_ESP;d.ota.progress=63;d.ota.received=900000;d.ota.total=1464000;
   snprintf(d.ota.version,sizeof(d.ota.version),"dev");
   render(state==OTA_STATE_RECEIVING?"upgrade_receiving":"upgrade_states",&u,&d);
   if(state==OTA_STATE_RECEIVING){
    u.confirm=true;render("upgrade_cancel_review",&u,&d);u.confirm=false;
   }
  }
  d.ota.state=OTA_STATE_FAILED;strcpy(d.ota.error_code,"HASH_MISMATCH");render("upgrade_checksum_failed",&u,&d);
 }
 check_locales();
 printf("PASS local UI cleanup: %u rendered reachable scenes, strip/full guards, complete glyphs, locale pairs, no legacy parameter/editor APIs\n",images);
}
