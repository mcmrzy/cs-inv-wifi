"""Exercise the real embedded Modbus LCD loop with physical-key/SPI stubs."""
from pathlib import Path
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
root=Path(__file__).resolve().parents[1]
source=(root/'full/full_panel_runtime.inc').read_text(encoding='utf-8')
tick=source[source.index('static void full_modbus_panel_tick('):source.index('static void panel_task(')]
inversion=source[source.index('static bool panel_set_view_inversion('):source.index('static void panel_render(')]
ota_fn=source[source.index('static bool panel_ota_active('):source.index('static void panel_snapshot(')]
actions=source[source.index('static void panel_save_appearance('):source.index('static void full_modbus_panel_tick(')]
config=(root/'sdkconfig.full').read_text(encoding='utf-8')
assert 'CONFIG_DIRECT_LCD_SPI_CLOCK_HZ=40000000' in config
assert 's_modbus_active && (panel_ota_active()' in source
assert 'panel_reset_for_key_page' not in source
assert 's_ui_data.bms_received=direct_bms_get_snapshot(' in source
assert 's_ui_data.bms_age_ms<5000U' in source
test=r'''
#include "panel_ui.h"
#include "panel_catalog.h"
#include "panel_lvgl.h"
#include "direct_gfx_assets.h"
#include "lvgl.h"
#include "direct_key_codec.h"
#include "ota_service.h"
#include "direct_ui.h"
#include "full_ui_bridge.h"
#include "direct_boot_logo.h"
#include "panel_wifi.h"
#include "panel_key_labels.h"
#include "panel_preferences.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
void panel_lvgl_host_assert(const char *file,int line){
 lv_mem_monitor_t m;lv_mem_monitor(&m);
 fprintf(stderr,"LVGL assertion at %s:%d free=%u largest=%u peak=%u\n",file,line,
 (unsigned)m.free_size,(unsigned)m.free_biggest_size,(unsigned)m.max_used);abort();
}
#define LCD_W 320
#define LCD_H 240
static const char *TAG="test";
static void log_line(const char *tag,const char *fmt,...){(void)tag;(void)fmt;}
#define ESP_LOGW log_line
#define ESP_LOGI log_line
static atomic_uint s_panel_key_events,s_lcd_tx_errors;
static atomic_uint s_panel_page,s_panel_frames;
static atomic_bool s_first_frame_done;
enum {PANEL_RENDER};
static void panel_stage(int stage){assert(stage==PANEL_RENDER);}
static panel_ui_t s_modbus_ui;
static panel_snapshot_t s_modbus_snapshot,live;
static panel_view_t s_modbus_view,s_modbus_previous;
static panel_page_t s_modbus_root=PANEL_PAGE_POWER;
static bool s_modbus_active=true,s_modbus_drawn,s_blank,s_have_previous;
static bool s_lcd_test_inverted=true;
static direct_ui_state_t s_ui;
static direct_ui_data_t s_ui_data;
static unsigned weather_refreshes;
void panel_weather_refresh(void){weather_refreshes++;}
bool panel_weather_search(const char *s){(void)s;return false;}
bool panel_weather_set_city(int i){(void)i;return false;}
bool panel_weather_city(unsigned i,char *out,size_t n){(void)i;(void)out;(void)n;return false;}
static char s_wifi_password[65];
static void wifi_edit_reset(void){s_ui.editing=false;s_wifi_password[0]=0;}
static bool keyboard_key(uint8_t k,unsigned limit){(void)k;(void)limit;return false;}
static void keyboard_labels(uint8_t *keys,uint8_t submit){keys[KEY_ENTER]=submit;}
#include "panel_weather_ui.inc"
static uint8_t s_saved_style,s_preview_style;
static uint16_t s_local_blank=1;
static panel_display_settings_t s_saved_display={100,2},s_preview_display={100,2};
static panel_feedback_settings_t saved_feedback={24,PANEL_BEEP_NORMAL};
static bool fail_save,s_manual_recovery_pending;
static uint8_t backlight=100;
static void full_backlight_set(uint8_t b,bool off){assert(!off);backlight=b;}
static void panel_discard_preview(void){
 s_preview_style=s_saved_style;s_preview_display=s_saved_display;
 full_backlight_set(s_preview_display.brightness,s_blank);
}
bool panel_preferences_update(const panel_preferences_t *p,unsigned fields){
 assert(p && (fields&PANEL_PREF_DISPLAY));
 if(fail_save)return false;
 if(fields&PANEL_PREF_LED_BRIGHTNESS)saved_feedback.led_brightness=p->feedback.led_brightness;
 if(fields&PANEL_PREF_BEEP_MODE)saved_feedback.beep_mode=p->buzzer?p->feedback.beep_mode:PANEL_BEEP_OFF;
 return true;
}
bool panel_preferences_reset(void){return false;}
bool panel_bms_detect_start(void){s_ui_data.bms_detection=PANEL_BMS_DETECT_RUNNING;return true;}
bool panel_bms_protocol_set(panel_bms_protocol_t protocol){
 if(fail_save)return false;
 s_ui_data.bms_protocol=protocol;
 return true;
}
void full_ui_reload_local_preferences(void){}
static void panel_load_local(void){panel_feedback_set_preferences(saved_feedback);}
static bool panel_set_time(bool change_clock){(void)change_clock;return true;}
static void arm_poll_request_config(void){}
static void arm_poll_request_info(void){}
static void esp_restart(void){assert(false);}
static OtaState ota_state;
bool ota_service_get_snapshot(OtaSnapshot *out){memset(out,0,sizeof(*out));out->state=ota_state;return true;}
bool ota_service_is_busy(void){return false;}
OtaResult ota_service_cancel(const char *id){(void)id;return OTA_RESULT_OK;}
static uint16_t s_strip_pixels[320*16];
static uint16_t framebuffer[320*240];
static uint16_t incremental_frame[320*240];
static unsigned bands,flushed_pixels,submits,notes,queries;
static bool fail_band,bus_unavailable;
static uint8_t next_key,following_key;
static uint32_t now_ms(void){return live.now_ms;}
static unsigned inversion_commands;
static uint8_t last_inversion_command;
static void command(uint8_t cmd){assert(cmd==0x20 || cmd==0x21);last_inversion_command=cmd;inversion_commands++;}
static bool panel_key_current(uint32_t timestamp){return live.now_ms-timestamp<=1000U;}
static bool panel_bus_snapshot(panel_snapshot_t *out){if(bus_unavailable)return false;*out=live;return true;}
static void panel_bus_request_ota_diagnostics(void){queries++;}
static unsigned setting_queries;
static void panel_bus_request_settings(const uint16_t *addresses,unsigned count){
 assert(count<=5);for(unsigned i=0;i<count;i++)assert(panel_setting_find(addresses[i]));setting_queries+=count;
}
static bool direct_keys_get_timed_event(uint8_t *key,uint32_t *stamp){
 if(!next_key)return false;*key=next_key;*stamp=live.now_ms;next_key=following_key;following_key=0;return true;
}
static void factory_note_key(uint8_t key){(void)key;notes++;}
static bool panel_wake_key(void){return false;}
static uint16_t expected_address=0x4123;
static bool panel_bus_submit_sequence(const panel_command_t *c,uint32_t *seq){
 assert(c->type==PANEL_CMD_WRITE && c->address==expected_address);
 *seq=40+(++submits);live.command_sequence=*seq;return true;
}
static void write_pixels(int x,int y,int w,int h,uint16_t *pixels){
 assert(!(x&1) && !(w&1)); /* RGB565 DMA spans are 32-bit aligned. */
 assert(x>=0&&y>=0&&w>0&&h>0&&x+w<=320&&y+h<=240&&w*h<=320*16);
 bands++;flushed_pixels+=w*h;
 for(int row=0;row<h;row++)memcpy(framebuffer+(y+row)*320+x,pixels+row*w,w*sizeof(*pixels));
 if(fail_band){atomic_fetch_add(&s_lcd_tx_errors,1);fail_band=false;}
}
''' +ota_fn+inversion+actions+tick+r'''
static void press(uint8_t key){next_key=key;full_modbus_panel_tick();}
static void user_key(uint8_t key){
 if(s_modbus_active)press(key);
 else {
  panel_action(direct_ui_key(&s_ui,key,live.now_ms,&s_ui_data));
  if(s_modbus_active)full_modbus_panel_tick();
 }
}
static void save_frame(const char *name){
 char path[80];snprintf(path,sizeof(path),"tmp/%s.ppm",name);
 FILE *f=fopen(path,"wb");assert(f);fputs("P6\n320 240\n255\n",f);
 for(unsigned i=0;i<320*240;i++){
  uint16_t p=framebuffer[i];
  unsigned char c[]={((p>>11)&31)*255/31,((p>>5)&63)*255/63,(p&31)*255/31};
  assert(fwrite(c,1,3,f)==3);
 }
 assert(!fclose(f));
}
static void assert_clean_frame(uint32_t now){
 memcpy(incremental_frame,framebuffer,sizeof(framebuffer));
 panel_lvgl_refresh(now,true);
 assert(!memcmp(incremental_frame,framebuffer,sizeof(framebuffer)));
}
static lv_obj_t *find_label(lv_obj_t *obj,const char *text,int minimum_y){
 if(lv_obj_has_flag(obj,LV_OBJ_FLAG_HIDDEN))return NULL;
 if(lv_obj_check_type(obj,&lv_label_class) && !strcmp(lv_label_get_text(obj),text)){
  lv_area_t a;lv_obj_get_coords(obj,&a);if(a.y1>=minimum_y)return obj;
 }
 for(unsigned i=0;i<lv_obj_get_child_cnt(obj);i++){
  lv_obj_t *found=find_label(lv_obj_get_child(obj,(int)i),text,minimum_y);if(found)return found;
 }
 return NULL;
}
static lv_area_t label_bounds(const char *text,int minimum_y){
 lv_obj_t *obj=find_label(lv_scr_act(),text,minimum_y);assert(obj);
 lv_area_t a;lv_obj_get_coords(obj,&a);return a;
}
static unsigned sprite_colors(unsigned x,unsigned y,unsigned width,unsigned height){
 uint16_t colors[64];unsigned count=0;
 for(unsigned row=y;row<y+height;row++)for(unsigned col=x;col<x+width;col++){
  uint16_t pixel=framebuffer[row*320+col];unsigned i=0;
  while(i<count && colors[i]!=pixel)i++;
  if(i==count){colors[count++]=pixel;if(count==64)return count;}
 }
 return count;
}
static void user_journey(void){
 panel_load_local();
 direct_ui_init(&s_ui);
 s_preview_style=s_saved_style=2;
 s_ui_data.language_zh=true;s_ui_data.valid[15]=true;s_ui_data.config[15]=1;
 bus_unavailable=false;live.now_ms=10000;panel_invalidate(&live,0x42);
 user_key(DIRECT_KEY_UP);full_modbus_panel_tick();
 assert(s_modbus_active&&s_modbus_ui.page==PANEL_PAGE_DETAILS&&strstr(s_modbus_view.subtitle,"等待"));
 panel_store(&live,0x42,3,live.now_ms);full_modbus_panel_tick();
 assert(strstr(s_modbus_view.subtitle,"在线"));
 user_key(DIRECT_KEY_DOWN);assert(s_modbus_ui.detail==0);
 user_key(DIRECT_KEY_ENTER);assert(s_modbus_ui.page==PANEL_PAGE_DETAIL_GROUPS);
 user_key(DIRECT_KEY_DOWN);user_key(DIRECT_KEY_DOWN);user_key(DIRECT_KEY_DOWN);user_key(DIRECT_KEY_ENTER);
 assert(!s_modbus_active && s_ui.page==UI_BMS);
 user_key(DIRECT_KEY_BACK);assert(s_modbus_ui.page==PANEL_PAGE_DETAIL_GROUPS&&s_modbus_ui.selected==3);
 user_key(DIRECT_KEY_BACK);assert(s_modbus_ui.page==PANEL_PAGE_DETAILS&&s_modbus_ui.detail==0);
 user_key(DIRECT_KEY_BACK);assert(!s_modbus_active&&s_ui.page==UI_HOME);
 user_key(DIRECT_KEY_LEFT);full_modbus_panel_tick();
 assert(s_modbus_ui.page==PANEL_PAGE_ALARMS&&strstr(s_modbus_view.rows[0],"尚未收到"));
 panel_store(&live,0x46,1U<<4,live.now_ms);
 panel_store(&live,0x47,0,live.now_ms);
 panel_store(&live,0x48,1U<<6,live.now_ms);
 panel_store(&live,0x49,1U<<3,0);
 full_modbus_panel_tick();
 assert(s_modbus_view.alarm_list && s_modbus_view.alert && (s_modbus_view.fault_rows&(1U<<2)));
 assert(!strcmp(s_modbus_view.rows[0],"当前 2  /  上次 1"));
 assert((s_modbus_view.stale_rows&(1U<<4)) && strstr(s_modbus_view.rows[4],"历史"));
 save_frame("lvgl_alarms");
 for(unsigned reg=0x46;reg<=0x49;reg++)panel_invalidate(&live,reg);
 user_key(DIRECT_KEY_BACK);assert(s_ui.page==UI_HOME);
 user_key(DIRECT_KEY_ENTER);user_key(DIRECT_KEY_ENTER);
 assert(s_modbus_ui.page==PANEL_PAGE_DETAIL_GROUPS&&s_modbus_root==PANEL_PAGE_DETAIL_GROUPS);
 save_frame("lvgl_detail_groups");
 const uint16_t grid_regs[]={0x50,0x51,0x52,0x53,0x58,0x5b,0x5e};
 const uint16_t grid_example[]={2310,610,5001,1385,2301,1080,1150};
 for(unsigned i=0;i<7;i++)panel_store(&live,grid_regs[i],grid_example[i],live.now_ms);
 user_key(DIRECT_KEY_ENTER);assert(s_modbus_view.detail_dashboard);
 save_frame("lvgl_detail_dashboard");
 user_key(DIRECT_KEY_BACK);
 user_key(DIRECT_KEY_RIGHT);user_key(DIRECT_KEY_ENTER);assert(s_modbus_ui.detail==4);
 user_key(DIRECT_KEY_BACK);user_key(DIRECT_KEY_BACK);assert(s_ui.page==UI_MENU&&s_ui.selected==0);
 user_key(DIRECT_KEY_RIGHT);user_key(DIRECT_KEY_DOWN);
 user_key(DIRECT_KEY_ENTER);
 assert(s_ui.page==UI_SUBMENU&&s_ui.menu_selected==4);
 user_key(DIRECT_KEY_ENTER);assert(s_ui.page==UI_APPEARANCE);
 user_key(DIRECT_KEY_DOWN);user_key(DIRECT_KEY_DOWN);user_key(DIRECT_KEY_LEFT);
 assert(backlight==90&&s_saved_display.brightness==100);
 fail_save=true;user_key(DIRECT_KEY_ENTER);
 assert(backlight==100&&s_ui.time_fields[0]==90&&!strcmp(s_ui.message,"Save failed / retry"));
 direct_ui_view_t appearance_view;direct_ui_build(&s_ui,&s_ui_data,&appearance_view);
 panel_lvgl_show_local(&s_ui,&s_ui_data,&appearance_view,NULL,NULL,false,false);
 panel_lvgl_refresh(live.now_ms,false);save_frame("lvgl_appearance_retry");
 fail_save=false;user_key(DIRECT_KEY_ENTER);
 assert(backlight==90&&s_saved_display.brightness==90&&!strcmp(s_ui.message,"Appearance saved"));
 user_key(DIRECT_KEY_DOWN);user_key(DIRECT_KEY_DOWN);user_key(DIRECT_KEY_RIGHT);
 assert(s_ui.time_fields[2]==36&&saved_feedback.led_brightness==24);
 user_key(DIRECT_KEY_DOWN);user_key(DIRECT_KEY_RIGHT);
 assert(s_ui.time_fields[3]==PANEL_BEEP_LONG&&saved_feedback.beep_mode==PANEL_BEEP_NORMAL);
 user_key(DIRECT_KEY_ENTER);
 assert(saved_feedback.led_brightness==36&&saved_feedback.beep_mode==PANEL_BEEP_LONG);
 user_key(DIRECT_KEY_RIGHT);assert(s_ui.time_fields[3]==PANEL_BEEP_OFF);
 user_key(DIRECT_KEY_BACK);assert(s_ui.page==UI_SUBMENU&&s_ui.selected==0);
 assert(saved_feedback.beep_mode==PANEL_BEEP_LONG);
 user_key(DIRECT_KEY_BACK);assert(s_ui.page==UI_MENU&&s_ui.selected==4);
 user_key(DIRECT_KEY_LEFT);user_key(DIRECT_KEY_LEFT);
 user_key(DIRECT_KEY_ENTER);
 assert(s_modbus_active&&s_modbus_ui.page==PANEL_PAGE_SETTING_GROUPS&&s_ui.selected==1);
 save_frame("lvgl_settings_groups");
 user_key(DIRECT_KEY_RIGHT);user_key(DIRECT_KEY_ENTER);user_key(DIRECT_KEY_DOWN);
 assert(s_modbus_ui.page==PANEL_PAGE_SETTINGS && panel_setting(s_modbus_ui.selected)->address==0x4101);
 panel_store(&live,0x2a1,5000,live.now_ms);
 panel_store(&live,0x142,1,live.now_ms);
 panel_store(&live,0x140,1,live.now_ms);
 panel_store(&live,0x143,2,live.now_ms);
 full_modbus_panel_tick();save_frame("lvgl_settings");
 lv_obj_t *setting_label=find_label(lv_scr_act(),"输出频率",0);
 lv_obj_t *setting_value=find_label(lv_scr_act(),"50 Hz",0);
 assert(setting_label);
 assert(setting_value);
 assert(s_modbus_view.key_labels[KEY_ENTER]==KEY_EDIT);
 user_key(DIRECT_KEY_ENTER);user_key(DIRECT_KEY_RIGHT);
 assert(s_modbus_ui.editing&&!s_modbus_ui.review);
 assert(!strcmp(s_modbus_view.values[1],"50 Hz")&&!strcmp(s_modbus_view.values[2],"60 Hz"));
 assert(s_modbus_view.setting_editor&&strstr(s_modbus_view.footer,"确认保存"));
 assert(s_modbus_view.key_labels[KEY_LEFT]==KEY_MINUS && s_modbus_view.key_labels[KEY_DOWN]==KEY_MINUS);
 assert(s_modbus_view.key_labels[KEY_RIGHT]==KEY_PLUS && s_modbus_view.key_labels[KEY_UP]==KEY_PLUS);
 save_frame("lvgl_editor");
 unsigned before=submits;
 live.now_ms+=PANEL_FRESH_MS+1;full_modbus_panel_tick();
 assert(!s_modbus_ui.review&&!s_modbus_ui.editing&&strstr(s_modbus_view.footer,"已取消"));
 user_key(DIRECT_KEY_ENTER);assert(s_modbus_ui.blocked&&!s_modbus_ui.pending);
 panel_store(&live,0x42,3,live.now_ms);panel_store(&live,0x2a1,5000,live.now_ms);
 full_modbus_panel_tick();assert(!s_modbus_ui.blocked&&strstr(s_modbus_view.footer,"已恢复")&&submits==before);
 expected_address=0x4101;
 user_key(DIRECT_KEY_ENTER);user_key(DIRECT_KEY_RIGHT);user_key(DIRECT_KEY_ENTER);
 assert(submits==before+1&&s_modbus_ui.pending);
 live.now_ms+=PANEL_FRESH_MS+1;
 panel_result_record(&live,live.command_sequence,PANEL_WRITE_UNCERTAIN,0);full_modbus_panel_tick();
 assert(s_modbus_ui.have_result&&strstr(s_modbus_view.rows[2],"设置应答超时")&&(s_modbus_view.stale_rows&2));
 panel_store(&live,0x42,3,live.now_ms);panel_store(&live,0x2a1,5000,live.now_ms);
 full_modbus_panel_tick();
 assert(s_modbus_ui.have_result&&strstr(s_modbus_view.rows[2],"目标不一致")&&submits==before+1);
 user_key(DIRECT_KEY_ENTER);
 user_key(DIRECT_KEY_ENTER);user_key(DIRECT_KEY_RIGHT);user_key(DIRECT_KEY_ENTER);
 assert(submits==before+2&&s_modbus_ui.pending);
 panel_result_record(&live,live.command_sequence,PANEL_WRITE_REJECTED,3);full_modbus_panel_tick();
 assert(s_modbus_ui.have_result&&strstr(s_modbus_view.rows[3],"拒绝"));
 user_key(DIRECT_KEY_ENTER);assert(!s_modbus_ui.have_result&&submits==before+2);
 user_key(DIRECT_KEY_ENTER);user_key(DIRECT_KEY_RIGHT);user_key(DIRECT_KEY_ENTER);
 panel_store(&live,0x2a1,6000,live.now_ms);
 panel_result_record(&live,live.command_sequence,PANEL_WRITE_CONFIRMED,0);full_modbus_panel_tick();
 assert(submits==before+3&&s_modbus_ui.have_result&&strstr(s_modbus_view.rows[2],"设置已生效"));
 user_key(DIRECT_KEY_ENTER);user_key(DIRECT_KEY_BACK);
 assert(s_modbus_active&&s_modbus_ui.page==PANEL_PAGE_SETTING_GROUPS&&s_modbus_ui.selected==1);
 user_key(DIRECT_KEY_RIGHT);user_key(DIRECT_KEY_ENTER);
 assert(s_modbus_ui.page==PANEL_PAGE_SETTINGS&&strstr(s_modbus_view.title,"充电与电池"));
 assert(!panel_ui_can_exit(&s_modbus_ui,&s_modbus_snapshot,s_modbus_root));
 user_key(DIRECT_KEY_BACK);
 assert(s_modbus_active&&s_modbus_ui.page==PANEL_PAGE_SETTING_GROUPS&&s_modbus_ui.selected==2);
 user_key(DIRECT_KEY_DOWN);user_key(DIRECT_KEY_RIGHT);user_key(DIRECT_KEY_ENTER);
 assert(s_modbus_ui.page==PANEL_PAGE_SETTINGS&&strstr(s_modbus_view.title,"系统功能"));
 assert(panel_setting(s_modbus_ui.selected)->address==0x4123);
 user_key(DIRECT_KEY_BACK);user_key(DIRECT_KEY_BACK);
 assert(submits==before+3);
 assert(!s_modbus_active&&s_ui.page==UI_MENU&&s_ui.selected==1);
 user_key(DIRECT_KEY_BACK);assert(s_ui.page==UI_HOME);
 puts("PASS user journey: home, live data, alarms, display save failure/retry, disconnect/recovery, parameter rejection/readback and back to home");
}
int main(void){
 for(int state=OTA_STATE_IDLE;state<=OTA_STATE_ROLLED_BACK;state++){
  ota_state=(OtaState)state;
  bool active=(state>=OTA_STATE_ACCEPTED && state<=OTA_STATE_CANCELLING)||state==OTA_STATE_REBOOTING;
  assert(panel_ota_active()==active);
 }
 ota_state=OTA_STATE_IDLE;assert(!panel_ota_active()); /* BLE admin lock leaves IDLE snapshot. */
 live.now_ms=1000;s_preview_style=2;live.config=(panel_config_t){1,9600,0,PANEL_PROFILE_6KU};
 panel_store(&live,0x42,3,1000);panel_ui_init(&s_modbus_ui);s_modbus_ui.page=PANEL_PAGE_POWER;
 assert(panel_lvgl_init(s_strip_pixels,write_pixels,live.now_ms));
 /* Check the LVGL bitmap adapter, including odd-width row padding. */
 for(uint32_t cp=32;cp<=0xffff;cp++)if(direct_gfx_has_glyph(cp)){
  const direct_gfx_glyph_t *g=direct_gfx_glyph(cp,16);unsigned w=g->width,h=g->height,stride=g->stride;
  uint8_t original[512];assert(stride*h<=sizeof(original));memcpy(original,g->data,stride*h);
  const uint8_t *bitmap=lv_font_get_glyph_bitmap(&panel_lvgl_body_font,cp);
  for(unsigned y=0;y<h;y++)for(unsigned x=0;x<w;x++){
   unsigned expected=(original[y*stride+x/2]>>(x&1?0:4))&15U,p=y*w+x;
   assert(expected==((bitmap[p/2]>>(p&1?0:4))&15U));
  }
 }
 puts("PASS LVGL glyph packing: all body glyphs, common baseline and odd-width rows retain exact pixels");
 full_modbus_panel_tick();assert(flushed_pixels==320*240 && s_modbus_drawn);
 assert(atomic_load(&s_panel_page)==0x100U+PANEL_PAGE_POWER && atomic_load(&s_panel_frames)==1);
 unsigned first_bands=bands;full_modbus_panel_tick();assert(bands==first_bands);
 fail_band=true;press(DIRECT_KEY_DOWN);unsigned after_error=flushed_pixels;
 assert(!s_modbus_drawn);
 full_modbus_panel_tick();assert(flushed_pixels>=after_error+320*240 && s_modbus_drawn);
 unsigned stable=bands;full_modbus_panel_tick();assert(bands==stable);
 press(DIRECT_KEY_ENTER);assert(s_modbus_ui.review && submits==0);
 unsigned review_strips=bands-stable;
 assert(review_strips>0);
 press(DIRECT_KEY_BACK);assert(!s_modbus_ui.review && s_modbus_active && submits==0);
 press(DIRECT_KEY_ENTER);press(DIRECT_KEY_ENTER);assert(submits==1 && s_modbus_ui.pending);
 unsigned pending_pixels=flushed_pixels;
 assert(s_modbus_view.activity_frame!=0);
 live.now_ms+=110;full_modbus_panel_tick();
 assert(flushed_pixels>pending_pixels && flushed_pixels-pending_pixels<320*16 && submits==1);
 save_frame("lvgl_waiting");
 press(DIRECT_KEY_ENTER);press(DIRECT_KEY_BACK);assert(submits==1 && s_modbus_active);
 panel_result_record(&live,41,PANEL_WRITE_CONFIRMED,0);full_modbus_panel_tick();assert(!s_modbus_ui.pending);
 assert(s_modbus_view.result && s_modbus_view.activity_frame==0);
 live.now_ms+=120;full_modbus_panel_tick();
 save_frame("lvgl_result");
 press(DIRECT_KEY_BACK);assert(s_modbus_active && !s_modbus_ui.have_result);
 press(DIRECT_KEY_BACK);assert(!s_modbus_active && s_ui.dirty && !s_have_previous);
 s_modbus_active=true;bus_unavailable=true;full_modbus_panel_tick();
 assert(!panel_online(&s_modbus_snapshot));press(DIRECT_KEY_BACK);assert(!s_modbus_active&&submits==1);
 assert(atomic_load(&s_panel_key_events)==notes);
 user_journey();
 /* Capture the real voltage editor and truthful mismatched readback result. */
 panel_ui_t voltage_ui;panel_ui_init(&voltage_ui);
 voltage_ui.page=PANEL_PAGE_SETTINGS;voltage_ui.setting_group=2;
 voltage_ui.selected=(uint16_t)(panel_setting_find(0x410a)-panel_setting(0));
 voltage_ui.editing=true;voltage_ui.draft=552;
 live.config.profile_override=PANEL_PROFILE_6KU;
 panel_store(&live,0x42,3,live.now_ms);panel_store(&live,0x150,4,live.now_ms);
 panel_store(&live,0x177,564,live.now_ms);
 panel_view_t voltage_view;panel_ui_view(&voltage_ui,&live,&voltage_view);
 assert(voltage_view.setting_editor&&!strcmp(voltage_view.values[1],"56.4 V"));
 assert(voltage_view.key_labels[KEY_LEFT]==KEY_MINUS && voltage_view.key_labels[KEY_RIGHT]==KEY_PLUS);
 assert(voltage_view.key_labels[KEY_DOWN]==KEY_FAST_DOWN && voltage_view.key_labels[KEY_UP]==KEY_FAST_UP);
 panel_lvgl_show_modbus(&voltage_view);panel_lvgl_refresh(live.now_ms,true);save_frame("lvgl_voltage_editor");
 lv_area_t cn=label_bounds(voltage_view.rows[1],45),dn=label_bounds(voltage_view.rows[2],45);
 lv_area_t cv=label_bounds(voltage_view.values[1],80),dv=label_bounds(voltage_view.values[2],80);
 assert(cn.y1==dn.y1 && cv.y1==dv.y1);
 assert(cn.x1>=8 && cn.x2<156 && dn.x1>=164 && dn.x2<312);
 assert(cn.y2<cv.y1 && dn.y2<dv.y1);
 voltage_view.theme_light=true;panel_lvgl_show_modbus(&voltage_view);
 panel_lvgl_refresh(live.now_ms,true);save_frame("lvgl_voltage_editor_light");

 panel_ui_t more;panel_ui_init(&more);more.page=PANEL_PAGE_SETTING_GROUPS;more.selected=6;
 panel_view_t more_view;panel_ui_view(&more,&live,&more_view);panel_lvgl_show_modbus(&more_view);
 panel_lvgl_refresh(live.now_ms,true);save_frame("lvgl_settings_groups_more");
 more.language_en=true;panel_ui_view(&more,&live,&more_view);panel_lvgl_show_modbus(&more_view);
 panel_lvgl_refresh(live.now_ms,true);save_frame("lvgl_settings_groups_more_en");
 more_view.theme_light=true;panel_lvgl_show_modbus(&more_view);
 panel_lvgl_refresh(live.now_ms,true);save_frame("lvgl_settings_groups_more_light");


 panel_ui_t power_en;panel_ui_init(&power_en);
 power_en.page=PANEL_PAGE_POWER;power_en.selected=2;power_en.language_en=true;
 panel_ui_view(&power_en,&live,&voltage_view);
 assert(!strcmp(voltage_view.rows[2],"Disable remote power control"));
 panel_lvgl_show_modbus(&voltage_view);panel_lvgl_refresh(live.now_ms,true);save_frame("lvgl_power_en");
 voltage_ui.language_en=true;panel_ui_view(&voltage_ui,&live,&voltage_view);
 panel_lvgl_show_modbus(&voltage_view);panel_lvgl_refresh(live.now_ms,true);save_frame("lvgl_voltage_editor_en");
 voltage_ui.language_en=false;
 panel_ui_t output_ui=voltage_ui;output_ui.setting_group=1;output_ui.selected=0;output_ui.draft=2;
 panel_store(&live,0x2a0,2200,live.now_ms);panel_ui_view(&output_ui,&live,&voltage_view);
 assert(!strcmp(voltage_view.values[1],"220 V")&&!strcmp(voltage_view.values[2],"230 V"));
 panel_lvgl_show_modbus(&voltage_view);panel_lvgl_refresh(live.now_ms,true);save_frame("lvgl_output_editor");
 voltage_ui.have_result=true;voltage_ui.result_address=0x410a;
 voltage_ui.result_value=552;voltage_ui.result_profile=PANEL_PROFILE_6KU;
 voltage_ui.last_result=PANEL_WRITE_PROCESSED;panel_store(&live,0x177,540,live.now_ms);
 panel_ui_view(&voltage_ui,&live,&voltage_view);
 assert(voltage_view.alert&&!strcmp(voltage_view.values[1],"54.0 V"));
 panel_lvgl_show_modbus(&voltage_view);panel_lvgl_refresh(live.now_ms,true);save_frame("lvgl_mismatch");
 panel_ui_t read_ui;panel_ui_init(&read_ui);read_ui.page=PANEL_PAGE_SETTINGS;read_ui.setting_group=2;
 read_ui.selected=(uint16_t)(panel_setting_find(0x4105)-panel_setting(0));
 live.setting_reads[0]=(panel_setting_read_t){0x4105,20,live.now_ms,PANEL_SETTING_VALUE,false};
 live.setting_reads[1]=(panel_setting_read_t){0x4106,10,live.now_ms,PANEL_SETTING_VALUE,false};
 live.setting_reads[2]=(panel_setting_read_t){0x4109,0,live.now_ms,PANEL_SETTING_EMPTY,false};
 panel_ui_view(&read_ui,&live,&voltage_view);
 assert(voltage_view.setting_count==5&&!strcmp(voltage_view.values[0],"20 A"));
 panel_lvgl_show_modbus(&voltage_view);panel_lvgl_refresh(live.now_ms,true);save_frame("lvgl_setting_reads");
 lv_area_t pn=label_bounds("市电充电电流",30),pv=label_bounds("20 A",30);
 assert(pn.y1==pv.y1 && pn.x2<pv.x1);

 read_ui.editing=true;read_ui.draft=20;panel_ui_view(&read_ui,&live,&voltage_view);
 assert(!strcmp(voltage_view.rows[1],"设备返回值"));
 panel_lvgl_show_modbus(&voltage_view);panel_lvgl_refresh(live.now_ms,true);save_frame("lvgl_returned_value");
 panel_snapshot_t read_only=live;read_only.config.profile_override=PANEL_PROFILE_3KU;
 read_only.detected_profile=PANEL_PROFILE_3KU;
 panel_store(&read_only,0x42,2,read_only.now_ms);
 panel_ui_t info_ui;panel_ui_init(&info_ui);info_ui.page=PANEL_PAGE_SETTINGS;info_ui.setting_group=7;
 info_ui.selected=(uint16_t)(panel_setting_find(0x4110)-panel_setting(0));
 panel_ui_view(&info_ui,&read_only,&voltage_view);
 assert(voltage_view.footer_notice && voltage_view.key_labels[KEY_ENTER]==KEY_INFO);
 assert(strstr(voltage_view.footer,"仅可读取"));
 panel_lvgl_show_modbus(&voltage_view);panel_lvgl_refresh(live.now_ms,true);save_frame("lvgl_setting_info");
 panel_view_t glyph_view={.selected=-1};
 snprintf(glyph_view.title,sizeof(glyph_view.title),"电压，电流，状态");
 snprintf(glyph_view.rows[0],sizeof(glyph_view.rows[0]),"电压，电流，状态");
 snprintf(glyph_view.values[0],sizeof(glyph_view.values[0]),"230.0 V");
 snprintf(glyph_view.rows[1],sizeof(glyph_view.rows[1]),"已断联，上次数据");
 panel_lvgl_show_modbus(&glyph_view);panel_lvgl_refresh(live.now_ms,true);save_frame("lvgl_font_alignment");
 user_key(DIRECT_KEY_DOWN);full_modbus_panel_tick();
 unsigned frames=atomic_load(&s_panel_frames);
 unsigned previous_inversion_commands=inversion_commands;
 s_lcd_test_inverted=false;s_modbus_drawn=false;
 next_key=DIRECT_KEY_DOWN;following_key=DIRECT_KEY_DOWN;
 full_modbus_panel_tick();
 assert(!next_key && s_modbus_ui.detail==2 && atomic_load(&s_panel_frames)==frames+1);
 assert(inversion_commands==previous_inversion_commands+1 && last_inversion_command==0x21 && s_lcd_test_inverted);
 direct_ui_view_t view;direct_ui_init(&s_ui);
 s_ui_data.language_zh=true;s_ui_data.theme_light=false;s_ui_data.dsp_online=true;
 s_ui_data.run.ACInWatt=1800;s_ui_data.run.GridVolt=230;s_ui_data.run.Ppv=4200;
 s_ui_data.run.Vpv1=368;s_ui_data.run.OutputWatt=3200;s_ui_data.run.ACOutputVolt=230;
 s_ui_data.run.BatVolt=524;s_ui_data.run.BatChgCurr=235;s_ui_data.run.BatterySOC=78;
 s_ui_data.sn="MB00001122334455";
 direct_ui_build(&s_ui,&s_ui_data,&view);
 panel_lvgl_show_local(&s_ui,&s_ui_data,&view,NULL,NULL,false,false);
 panel_lvgl_refresh(live.now_ms,true);save_frame("lvgl_home");
 s_ui_data.status_valid=true;s_ui_data.status.fault=1;
 panel_lvgl_show_local(&s_ui,&s_ui_data,&view,NULL,NULL,false,false);
 panel_lvgl_refresh(live.now_ms,false);save_frame("lvgl_home_alarm");
 label_bounds("查看告警",190);
 s_ui_data.status_valid=false;
 panel_lvgl_show_local(&s_ui,&s_ui_data,&view,NULL,NULL,false,false);
 panel_lvgl_refresh(live.now_ms,false);
 label_bounds("逆变器",190);
 lv_area_t pv_name=label_bounds("光伏",0),pv_power=label_bounds("4200W",0),pv_voltage=label_bounds("368V",0);
 lv_area_t load_name=label_bounds("负载",120),load_power=label_bounds("3200W",120),load_voltage=label_bounds("230V",120);
 assert(pv_name.y1<pv_power.y1 && pv_power.y1<pv_voltage.y1);
 assert(load_name.y1<load_power.y1 && load_power.y1<load_voltage.y1);
 assert(pv_name.x1==load_name.x1 && pv_power.x1==load_power.x1);
 assert(pv_voltage.x2==load_voltage.x2);
 label_bounds("电网取电",30);label_bounds("电池充电",120);
 unsigned menu_hint_pixels=0;
 for(unsigned y=220;y<240;y++)for(unsigned x=267;x<320;x++)menu_hint_pixels+=framebuffer[y*320+x]!=lv_color_hex(0x101113).full;
 assert(menu_hint_pixels>40);
 puts("PASS LVGL alignment: home titles, power and voltage form four aligned cards");
 RunParamDef measured_home=s_ui_data.run;
 s_ui_data.run.BatChgCurr=s_ui_data.run.BatDischgCurr=0;
 s_ui_data.run.BatChrWatt=0;s_ui_data.run.BatDisChrWatt=3;
 panel_lvgl_show_local(&s_ui,&s_ui_data,&view,NULL,NULL,false,false);
 panel_lvgl_refresh(live.now_ms,true);save_frame("lvgl_home_zero_amp");
 label_bounds("3W",120);label_bounds("电池放电",120);
 s_ui_data.run=measured_home;
 panel_lvgl_show_local(&s_ui,&s_ui_data,&view,NULL,NULL,false,false);
 panel_lvgl_refresh(live.now_ms,true);

 unsigned warm_pixels=0;
 for(unsigned y=80;y<166;y++)for(unsigned x=102;x<218;x++){
  uint16_t p=framebuffer[y*320+x];
  unsigned r=p>>11,g=((p>>5)&63)/2,b=p&31;
  warm_pixels+=r>b+4 && g>b+1;
 }
 assert(warm_pixels>1000); /* The approved house remains visible, not four text boxes. */
 unsigned animation_start=flushed_pixels;
 panel_lvgl_refresh(live.now_ms+90,false);
 assert(flushed_pixels>animation_start && flushed_pixels-animation_start<320*16);
 printf("PASS home animation: %u pixels per step in fixture\n",flushed_pixels-animation_start);
 label_bounds("实时",0);
 s_ui_data.dsp_online=false;s_ui_data.dsp_display_held=true;
 panel_lvgl_show_local(&s_ui,&s_ui_data,&view,NULL,NULL,false,false);
 panel_lvgl_refresh(live.now_ms+90,false);save_frame("lvgl_home_held");
 label_bounds("上次数据",0);
 unsigned held_pixels=flushed_pixels;
 live.now_ms+=90;
 panel_lvgl_refresh(live.now_ms+90,false);
 assert(flushed_pixels==held_pixels);
 s_ui_data.bms_online=true;s_ui_data.bms.soc_percent=73;s_ui_data.bms.voltage_10mv=5230;
 panel_lvgl_show_local(&s_ui,&s_ui_data,&view,NULL,NULL,false,false);
 panel_lvgl_refresh(live.now_ms+90,false);
 label_bounds("73%",120);label_bounds("电池已连接",190);
 s_ui_data.bms_online=false;
 s_ui_data.dsp_online=true;s_ui_data.dsp_display_held=false;s_ui_data.theme_light=true;
 panel_lvgl_show_local(&s_ui,&s_ui_data,&view,NULL,NULL,false,false);
 panel_lvgl_refresh(live.now_ms+90,false);save_frame("lvgl_home_light");
 s_ui_data.run.ACInWatt=-1800;
 panel_lvgl_show_local(&s_ui,&s_ui_data,&view,NULL,NULL,false,false);
 panel_lvgl_refresh(live.now_ms+90,false);
 label_bounds("电网馈电",30);label_bounds("-1800W",30);
 s_ui_data.run.ACInWatt=1800;
 s_ui_data.theme_light=false;
 s_ui.page=UI_BMS;
 s_ui_data.inverter_battery=(direct_inverter_battery_t){.valid=7,.online=true,.values={521,(uint16_t)-16,64}};
 direct_ui_build(&s_ui,&s_ui_data,&view);
 panel_lvgl_show_local(&s_ui,&s_ui_data,&view,NULL,NULL,false,false);
 panel_lvgl_refresh(live.now_ms+90,false);assert_clean_frame(live.now_ms+90);
 s_ui_data.inverter_battery=(direct_inverter_battery_t){0};
 s_ui.page=UI_MENU;s_ui.selected=1;
 panel_lvgl_show_local(&s_ui,&s_ui_data,&view,NULL,NULL,false,false);
 panel_lvgl_refresh(live.now_ms+120,true);save_frame("lvgl_menu");
 assert(sprite_colors(19,88,92,100)>20);
 unsigned status_lights=0;
 for(unsigned y=112;y<125;y++)for(unsigned x=50;x<90;x++){
  uint16_t pixel=framebuffer[y*320+x];
  if(pixel==lv_color_hex(0x29a7ff).full)status_lights|=1;
  if(pixel==lv_color_hex(0xffd447).full)status_lights|=2;
  if(pixel==lv_color_hex(0x4dd883).full)status_lights|=4;
 }
 assert(status_lights==7);
 s_ui.page=UI_BMS;s_ui.detail_page=0;
 for(unsigned state=0;state<4;state++){
  s_ui_data.bms_received=state>0;s_ui_data.bms_online=state==1 || state==3;
  s_ui_data.bms.voltage_10mv=5245;s_ui_data.bms.current_10ma=-105;
  s_ui_data.bms.percent_divisor=10;s_ui_data.bms.soc_percent=785;s_ui_data.bms.soh_percent=998;
  for(unsigned i=0;i<16;i++)s_ui_data.bms.cell_mv[i]=(uint16_t)(3200+i);
  for(unsigned page=0;page<3;page++){
   s_ui.detail_page=page;direct_ui_build(&s_ui,&s_ui_data,&view);
   panel_lvgl_show_local(&s_ui,&s_ui_data,&view,NULL,NULL,false,false);
   panel_lvgl_refresh(live.now_ms+125,false);assert_clean_frame(live.now_ms+125);
   if(state==1 && page==0){
    assert(sprite_colors(19,62,48,98)>20);
    assert(framebuffer[90*320+118]==lv_color_hex(0x29a7ff).full);
    assert(framebuffer[90*320+298]==lv_color_hex(0x3c454d).full);
    save_frame("lvgl_bms");
   }
   if(state==1 && page==2)save_frame("lvgl_bms_cells");
   if(state==2 && page==0)save_frame("lvgl_bms_offline");
  }
 }
 s_ui.detail_page=0;
 s_ui_data.inverter_battery=(direct_inverter_battery_t){.valid=7,.online=true,.values={521,(uint16_t)-16,64}};
 direct_ui_build(&s_ui,&s_ui_data,&view);
 panel_lvgl_show_local(&s_ui,&s_ui_data,&view,NULL,NULL,false,false);
 panel_lvgl_refresh(live.now_ms+130,true);
 assert(find_label(lv_scr_act(),"78%",0) && !find_label(lv_scr_act(),"64%",0));
 save_frame("lvgl_bms_live");
 unsigned soc_start=flushed_pixels;
 s_ui_data.bms.soc_percent=795;
 direct_ui_build(&s_ui,&s_ui_data,&view);
 panel_lvgl_show_local(&s_ui,&s_ui_data,&view,NULL,NULL,false,false);
 panel_lvgl_refresh(live.now_ms+131,false);
 assert(flushed_pixels>soc_start && flushed_pixels-soc_start<320*90);
 printf("PASS battery refresh: %u pixels on SOC change, complete page transitions\n",flushed_pixels-soc_start);
 assert_clean_frame(live.now_ms+131);
 unsigned battery_submits=submits;
 s_modbus_active=false; /* Render fixtures above set local pages directly. */
 user_key(DIRECT_KEY_ENTER);
 assert(s_ui.page==UI_BMS_PROTOCOL && !s_modbus_active);
 direct_ui_build(&s_ui,&s_ui_data,&view);
 panel_lvgl_show_local(&s_ui,&s_ui_data,&view,NULL,NULL,false,false);
 panel_lvgl_refresh(live.now_ms+132,true);
 assert(find_label(lv_scr_act(),"自动识别",0) && find_label(lv_scr_act(),"手动设置",0));
 user_key(DIRECT_KEY_ENTER);assert(s_ui_data.bms_detection==PANEL_BMS_DETECT_RUNNING);
 direct_ui_build(&s_ui,&s_ui_data,&view);
 panel_lvgl_show_local(&s_ui,&s_ui_data,&view,NULL,NULL,false,false);
 assert(find_label(lv_scr_act(),"正在识别",0));
 s_ui_data.bms_detection=PANEL_BMS_DETECT_MATCHED;
 direct_ui_build(&s_ui,&s_ui_data,&view);
 panel_lvgl_show_local(&s_ui,&s_ui_data,&view,NULL,NULL,false,false);
 assert(find_label(lv_scr_act(),"识别成功",0));
 user_key(DIRECT_KEY_DOWN);user_key(DIRECT_KEY_ENTER);assert(s_ui.editing);
 direct_ui_build(&s_ui,&s_ui_data,&view);
 panel_lvgl_show_local(&s_ui,&s_ui_data,&view,NULL,NULL,false,false);
 panel_lvgl_refresh(live.now_ms+132,true);
 assert(find_label(lv_scr_act(),"BBJ (0x7C)",0) && find_label(lv_scr_act(),"CSERGY Modbus",0));
 assert(find_label(lv_scr_act(),"当前",0) && find_label(lv_scr_act(),"通信正常",0));
 user_key(DIRECT_KEY_DOWN);
 fail_save=true;user_key(DIRECT_KEY_ENTER);
 assert(s_ui_data.bms_protocol==PANEL_BMS_BBJ && strstr(s_ui.message,"Save failed"));
 direct_ui_build(&s_ui,&s_ui_data,&view);
 panel_lvgl_show_local(&s_ui,&s_ui_data,&view,NULL,NULL,false,false);
 panel_lvgl_refresh(live.now_ms+133,true);
 assert(find_label(lv_scr_act(),"保存失败请重试",0));
 fail_save=false;user_key(DIRECT_KEY_ENTER);
 assert(s_ui_data.bms_protocol==PANEL_BMS_CSERGY && submits==battery_submits);
 user_key(DIRECT_KEY_UP);user_key(DIRECT_KEY_ENTER);
 assert(s_ui_data.bms_protocol==PANEL_BMS_BBJ && submits==battery_submits);
 user_key(DIRECT_KEY_BACK);assert(s_ui.page==UI_BMS_PROTOCOL && !s_ui.editing);
 user_key(DIRECT_KEY_UP);user_key(DIRECT_KEY_ENTER);
 assert(s_modbus_active && s_modbus_ui.setting_group==2 &&
        panel_setting(s_modbus_ui.selected)->address==0x4107 && submits==battery_submits);
 user_key(DIRECT_KEY_BACK);
 assert(!s_modbus_active && s_ui.page==UI_BMS_PROTOCOL && submits==battery_submits);
 user_key(DIRECT_KEY_BACK);assert(s_ui.page==UI_BMS);
 onboarding_qr_t qr={0};assert(onboarding_qr_update(&qr,"MB00001122334455","123456"));
 s_ui.page=UI_PHONE_SETUP;
 panel_lvgl_show_local(&s_ui,&s_ui_data,&view,&qr,"123456",true,false);
 panel_lvgl_refresh(live.now_ms+140,true);save_frame("lvgl_phone");
 qr.ready=false;
 panel_lvgl_show_local(&s_ui,&s_ui_data,&view,&qr,"123456",true,false);
 panel_lvgl_refresh(live.now_ms+141,false);
 assert(find_label(lv_scr_act(),"123456",0) && find_label(lv_scr_act(),"请手动输入 PIN",0));
 panel_lvgl_show_local(&s_ui,&s_ui_data,&view,&qr,"",true,false);
 panel_lvgl_refresh(live.now_ms+142,false);
 assert(!find_label(lv_scr_act(),"123456",0) && find_label(lv_scr_act(),"未配置配对码",0));
 s_ui.page=UI_WIFI;s_ui.editing=true;memset(&view,0,sizeof(view));
 snprintf(view.title,sizeof(view.title),"Wi-Fi 配网");
 snprintf(view.rows[0],sizeof(view.rows[0]),"Home Network");
 snprintf(view.rows[1],sizeof(view.rows[1]),"Abcd123!|");
 snprintf(view.footer,sizeof(view.footer),"方向选字  确认输入  返回取消");
 view.selected=0;
 panel_lvgl_show_local(&s_ui,&s_ui_data,&view,NULL,NULL,false,false);
 panel_lvgl_refresh(live.now_ms+143,false);assert_clean_frame(live.now_ms+143);save_frame("lvgl_wifi_keyboard");
 view.selected++;
 panel_lvgl_show_local(&s_ui,&s_ui_data,&view,NULL,NULL,false,false);
 panel_lvgl_refresh(live.now_ms+144,false);assert_clean_frame(live.now_ms+144);
 view.selected=(2<<PANEL_WIFI_MODE_SHIFT);
 panel_lvgl_show_local(&s_ui,&s_ui_data,&view,NULL,NULL,false,false);
 panel_lvgl_refresh(live.now_ms+145,false);assert_clean_frame(live.now_ms+145);save_frame("lvgl_wifi_numeric");
 s_weather=(panel_weather_snapshot_t){.valid=true,.location_valid=true,.days=7,.temperature_10=236,.code=2};
 snprintf(s_weather.city,sizeof(s_weather.city),"Changsha");
 for(unsigned i=0;i<7;i++){
  snprintf(s_weather.daily[i].date,sizeof(s_weather.daily[i].date),"2026-11-%02d",6+(int)i);
  s_weather.daily[i].code=i?61:0;s_weather.daily[i].low_10=120+(int)i*10;s_weather.daily[i].high_10=240+(int)i*10;
 }
 panel_lvgl_weather(&s_weather);s_ui.page=UI_HOME;
 direct_ui_build(&s_ui,&s_ui_data,&view);panel_lvgl_show_local(&s_ui,&s_ui_data,&view,NULL,NULL,false,false);
 panel_lvgl_refresh(live.now_ms+146,false);assert_clean_frame(live.now_ms+146);save_frame("lvgl_home_weather");
 s_ui.page=UI_WEATHER;s_ui.editing=false;s_ui.detail_page=0;weather_ui_view(&view,true);
 assert(strstr(view.rows[0],"长沙") && strstr(view.rows[1],"℃"));
 panel_lvgl_show_local(&s_ui,&s_ui_data,&view,NULL,NULL,false,false);
 panel_lvgl_refresh(live.now_ms+147,false);assert_clean_frame(live.now_ms+147);save_frame("lvgl_weather_7days");
 weather_ui_view(&view,false);s_ui_data.language_zh=false;
 panel_lvgl_show_local(&s_ui,&s_ui_data,&view,NULL,NULL,false,false);
 panel_lvgl_refresh(live.now_ms+148,false);assert_clean_frame(live.now_ms+148);save_frame("lvgl_weather_english");
 weather_ui_key(DIRECT_KEY_ENTER);assert(weather_refreshes==1);
 weather_ui_key(DIRECT_KEY_BACK);assert(s_ui.page==UI_HOME);s_ui_data.language_zh=true;
 s_ui.page=UI_WEATHER;s_ui.detail_page=1;s_ui.editing=true;s_ui.selected=0;
 snprintf(s_wifi_password,sizeof(s_wifi_password),"Beijing");
 weather_ui_view(&view,true);panel_lvgl_show_local(&s_ui,&s_ui_data,&view,NULL,NULL,false,false);
 panel_lvgl_refresh(live.now_ms+149,false);assert_clean_frame(live.now_ms+149);
 assert(find_label(lv_scr_act(),"Beijing|",29));save_frame("lvgl_weather_city_keyboard");
 weather_ui_key(DIRECT_KEY_BACK);
 s_ui.editing=false;s_ui.detail_page=0;
 puts("PASS weather display: real home mark, seven daily rows, English, partial redraw and refresh/back keys");
 /* The shipped graphical wordmark must survive the LVGL migration exactly. */
 for(unsigned light=0;light<2;light++){
  uint32_t start=live.now_ms+3000+light*3000;
  panel_lvgl_refresh(start,false);panel_lvgl_splash(light!=0);
  uint16_t bg=lv_color_hex(light?0xf4f7fc:0x101113).full;
  unsigned logo_pixels=0;
  static const unsigned times[]={0,100,200,350,500,650,800,950,1100,1350,1500,1600,1700};
  for(unsigned step=0;step<sizeof(times)/sizeof(*times);step++){
   unsigned t=times[step];panel_lvgl_refresh(start+t,step==0);assert_clean_frame(start+t);
   char name[64];snprintf(name,sizeof(name),"lvgl_boot_%s_%04u",light?"light":"dark",t);save_frame(name);
   unsigned non_bg=0;
   for(unsigned i=0;i<320*240;i++)non_bg+=framebuffer[i]!=bg;
   if(t==0 || t==1700)assert(non_bg==0);
   if(t==1100){
    for(unsigned part=0;part<2;part++){
     const direct_boot_logo_part_t *a=&direct_boot_logo_parts[part];
     uint16_t ink=lv_color_hex(part?(light?0x111d30:0xffffff):(light?0x1b3f8f:0x4a86ff)).full;
     for(unsigned y=0;y<a->height;y++)for(unsigned x=0;x<a->width;x++){
      if(!(a->data[y*a->stride+x/8]&(0x80>>(x%8))))continue;
      unsigned i=((240-direct_boot_logo_height)/2+a->y_offset+y)*320+
       (320-direct_boot_logo_width)/2+a->x_offset+x;
      assert(framebuffer[i]==ink);logo_pixels++;
     }
    }
    assert(non_bg==logo_pixels&&logo_pixels>5000);
   }
   if(t==650){
    const direct_boot_logo_part_t *a=&direct_boot_logo_parts[0];
    unsigned left=0,right=0;bool have=false;
    for(unsigned y=0;y<a->height;y++)for(unsigned x=0;x<a->width;x++){
     if(!(a->data[y*a->stride+x/8]&(0x80>>(x%8))))continue;
     unsigned i=((240-direct_boot_logo_height)/2+y)*320+(320-direct_boot_logo_width)/2+x;
     if(x<30){left=framebuffer[i];have=true;}if(x+30>a->width)right=framebuffer[i];
    }
    assert(have&&left!=right); /* Color travels across the original mark. */
   }
  }
 }
 s_ui.page=UI_HOME;panel_lvgl_show_local(&s_ui,&s_ui_data,&view,NULL,NULL,false,false);
 panel_lvgl_refresh(live.now_ms+10000,false);assert_clean_frame(live.now_ms+10000);
 puts("PASS LVGL brand splash: original bitmap, dark/light, grayscale-to-color sweep, fade, clean home transition");
 lv_mem_monitor_t memory;lv_mem_monitor(&memory);
 assert(setting_queries>0);
 assert(memory.total_size==LV_MEM_SIZE);
 assert(memory.free_size>=2048 && lv_mem_test()==LV_RES_OK);
 printf("PASS LVGL pool: peak %u bytes, free %u bytes\n",(unsigned)memory.max_used,(unsigned)memory.free_size);
 puts("PASS LVGL native runtime: real partial rendering and animation, one submit, receipt, review cancel and LCD retry");
}
'''
path=root/'tmp/native_runtime_test.c';path.write_text(test,encoding='utf-8')
shared=root.parent/'esp32_direct/main/direct'
lvgl=root/'components/lvgl__lvgl'
conf=root/'full/lv_conf.h'
obj_dir=root/'tmp/lvgl_host'
obj_dir.mkdir(exist_ok=True)
lvgl_flags=['-I'+str(lvgl),'-DPANEL_LVGL_HOST_TEST=1','-DLV_CONF_PATH='+conf.as_posix()]
def compile_lvgl(source):
    target=obj_dir/(source.relative_to(lvgl).as_posix().replace('/','_')+'.o')
    if not target.exists() or target.stat().st_mtime < max(source.stat().st_mtime,conf.stat().st_mtime):
        subprocess.run([sys.argv[1],'-std=c11','-Os',*lvgl_flags,'-c',str(source),'-o',str(target)],check=True)
    return target
with ThreadPoolExecutor(max_workers=6) as pool:
    objects=list(pool.map(compile_lvgl,sorted(p for p in (lvgl/'src').rglob('*.c') if p.name!='qrcodegen.c')))
exe=root/'tmp/native_runtime_test.exe'
subprocess.run([sys.argv[1],'-std=c11','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',
 *lvgl_flags,'-I'+str(shared.parent/'onboarding'),'-I'+str(root.parent/'esp32_direct/components/qrcodegen'),
 '-I'+str(root/'main'),'-I'+str(root/'full'),'-I'+str(shared),'-I'+str(shared.parent/'ota'),
 '-I'+str(shared.parent/'telemetry'),str(path),str(root/'main/panel_ui.c'),str(root/'main/panel_locale.c'),
 str(root/'main/panel_model.c'),str(root/'main/panel_catalog.c'),str(shared/'direct_gfx.c'),
 str(root/'main/panel_fonts.c'),str(root/'main/panel_display_settings.c'),str(root/'full/direct_ui.c'),
 str(root/'full/panel_lvgl.c'),str(root/'full/panel_home_art.c'),str(root/'full/direct_ui_locale.c'),str(shared/'direct_home.c'),str(shared/'direct_boot_logo.c'),
 str(shared.parent/'onboarding/onboarding_qr.c'),str(root.parent/'esp32_direct/components/qrcodegen/qrcodegen.c'),
 *map(str,objects),'-o',str(exe)],check=True)
subprocess.run([str(exe)],cwd=root,check=True)
