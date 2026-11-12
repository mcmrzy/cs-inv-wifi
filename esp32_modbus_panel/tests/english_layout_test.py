"""Exercise actual LVGL English labels with their real font, bounds and parent clips."""
from pathlib import Path
import subprocess
import sys

root=Path(__file__).resolve().parents[1]
native=root/'tests/native_runtime_test.py'
prefix=native.read_text(encoding='utf-8').split("path=root/'tmp/native_runtime_test.c'",1)[0]
scope={'__file__':str(native)}
exec(compile(prefix,str(native),'exec'),scope)
test=scope['test'].replace('int main(void){','void native_regression_main(void){')
test+=r'''
static unsigned failures,views;
static const char *test_pin="123456";
static void check_labels(lv_obj_t *obj){
 if(lv_obj_has_flag(obj,LV_OBJ_FLAG_HIDDEN))return;
 if(lv_obj_check_type(obj,&lv_label_class)){
  const char *s=lv_label_get_text(obj);lv_point_t needed;
  lv_label_t *actual=(lv_label_t *)obj;
  if(lv_label_get_long_mode(obj)==LV_LABEL_LONG_DOT && actual->dot_end!=UINT32_MAX){
   fprintf(stderr,"ELLIPSIS view=%u %s\n",views,s);failures++;
  }
  const lv_font_t *font=lv_obj_get_style_text_font(obj,0);
  lv_txt_get_size(&needed,s,font,lv_obj_get_style_text_letter_space(obj,0),0,LV_COORD_MAX,LV_TEXT_FLAG_NONE);
  if(*s && needed.x>lv_obj_get_width(obj)){
   fprintf(stderr,"CLIP view=%u %s need=%d width=%d\n",views,s,needed.x,lv_obj_get_width(obj));failures++;
  }
 }
 for(unsigned i=0;i<lv_obj_get_child_cnt(obj);i++)check_labels(lv_obj_get_child(obj,(int)i));
}
static void local_frame(void){
 direct_ui_view_t v;direct_ui_build(&s_ui,&s_ui_data,&v);
 const char *hints[6];direct_ui_key_hints(&s_ui,&s_ui_data,hints);
 for(unsigned i=0;i<6;i++){
  lv_point_t needed;lv_txt_get_size(&needed,hints[i],&panel_lvgl_body_font,-2,0,LV_COORD_MAX,LV_TEXT_FLAG_NONE);
  unsigned width=(i+1)*320/6-i*320/6;
  if(s_ui.page!=UI_UPGRADE && s_ui.page!=UI_WIFI && s_ui.page!=UI_WEATHER && needed.x>(int)width){fprintf(stderr,"HINT CLIP %s %d\n",hints[i],needed.x);failures++;}
 }
 panel_lvgl_show_local(&s_ui,&s_ui_data,&v,NULL,test_pin,true,false);
 panel_lvgl_refresh(++live.now_ms,true);views++;check_labels(lv_scr_act());
}
static void modbus_frame(panel_ui_t *u){
 panel_view_t v;panel_ui_view(u,&live,&v);panel_lvgl_show_modbus(&v);
 panel_lvgl_refresh(++live.now_ms,true);views++;check_labels(lv_scr_act());
 if(v.setting_editor){
  lv_area_t current=label_bounds(v.rows[1],45),draft=label_bounds(v.rows[2],45);
  if(current.y1!=draft.y1){fprintf(stderr,"ALIGN current/draft names view=%u\n",views);failures++;}
 }
}
static unsigned blue_pixels(unsigned x,unsigned y,unsigned w,unsigned h){
 unsigned n=0;for(unsigned row=y;row<y+h;row++)for(unsigned col=x;col<x+w;col++)
  n+=framebuffer[row*320+col]==lv_color_hex(0x29a7ff).full;
 return n;
}
int main(void){
 live.now_ms=10000;assert(panel_lvgl_init(s_strip_pixels,write_pixels,live.now_ms));
 s_ui_data=(direct_ui_data_t){.language_zh=false,.clock_valid=true,.clock="11-05 23:59",
  .hour=23,.minute=59,.sn="MB00001122334455",.version="0.4.11-modbus-full",
  .model="PWR4KL24H1",.hardware="CS-MB-PANEL",.factory_ready=true,.factory_locked=true};
 direct_ui_init(&s_ui);local_frame();
 direct_ui_event_t event=direct_ui_key(&s_ui,DIRECT_KEY_BACK,live.now_ms,&s_ui_data);
 assert(s_ui.page==UI_WEATHER && event.action==UI_ACT_NONE && s_ui.detail_page==0);
 direct_ui_key(&s_ui,DIRECT_KEY_BACK,live.now_ms,&s_ui_data);assert(s_ui.page==UI_HOME);
 assert(blue_pixels(298,4,15,20)==0);
 s_ui_data.dsp_online=true;s_ui_data.wifi_connected=true;s_ui_data.wifi_rssi_valid=true;s_ui_data.wifi_rssi_dbm=-50;local_frame();
 assert(blue_pixels(270,4,23,20)>15);
 for(unsigned y=5;y<23;y++)for(unsigned x=0;x<11;x++)
  assert(framebuffer[y*320+270+x]==framebuffer[y*320+291-x]);
 s_ui_data.bluetooth_connected=true;local_frame();assert(blue_pixels(298,4,15,20)>35);save_frame("english_home_wireless");
 s_ui_data.bluetooth_connected=false;
 direct_ui_view_t changed;direct_ui_build(&s_ui,&s_ui_data,&changed);
 panel_lvgl_show_local(&s_ui,&s_ui_data,&changed,NULL,NULL,true,false);panel_lvgl_refresh(++live.now_ms,false);
 assert(blue_pixels(298,4,15,20)==0);assert_clean_frame(live.now_ms);
 s_ui_data.dsp_online=false;s_ui_data.dsp_display_held=true;local_frame();
 for(unsigned menu=0;menu<5;menu++){s_ui.page=UI_MENU;s_ui.selected=menu;local_frame();}
 for(unsigned menu=4;menu<6;menu++)for(unsigned i=0;i<direct_ui_submenu_count(menu);i++){
  s_ui.page=UI_SUBMENU;s_ui.menu_selected=menu;s_ui.selected=i;local_frame();
 }
 static const direct_ui_page_t pages[]={UI_APPEARANCE,UI_HELP,UI_PHONE_SETUP,UI_TIME,UI_PRODUCTION,UI_UPGRADE};
 for(unsigned i=0;i<sizeof(pages)/sizeof(*pages);i++){
  s_ui.page=pages[i];s_ui.detail_page=0;s_ui.selected=0;local_frame();
 }
 direct_ui_view_t footer_view={.selected=-1};
 s_ui.page=UI_WIFI;
 const char *caption="Connection failed. Try again.";
 snprintf(s_ui.message,sizeof(s_ui.message),"%s",caption);
 footer_view.key_labels[KEY_BACK]=KEY_RETURN;
 footer_view.key_labels[KEY_RIGHT]=KEY_SCAN;
 footer_view.key_labels[KEY_ENTER]=KEY_LINK;
 panel_lvgl_show_local(&s_ui,&s_ui_data,&footer_view,NULL,NULL,false,false);
 panel_lvgl_refresh(++live.now_ms,true);
 lv_obj_t *footer_obj=find_label(lv_scr_act(),caption,198);
 assert(footer_obj&&lv_label_get_long_mode(footer_obj)==LV_LABEL_LONG_CLIP);
 assert(lv_obj_get_style_text_align(footer_obj,0)==LV_TEXT_ALIGN_CENTER);
 assert(blue_pixels(268,220,51,20)>20);
 memset(footer_view.key_labels,0,sizeof(footer_view.key_labels));
 panel_lvgl_show_local(&s_ui,&s_ui_data,&footer_view,NULL,NULL,false,false);
 panel_lvgl_refresh(++live.now_ms,false);
 assert(blue_pixels(268,220,51,20)==0);
 s_ui.message[0]=0;
 check_labels(lv_scr_act());
 s_ui.page=UI_PHONE_SETUP;test_pin="";local_frame();s_ui_data.sn="";local_frame();
 test_pin="123456";s_ui_data.sn="MB00001122334455";
 s_ui.page=UI_UPGRADE;
 for(unsigned state=OTA_STATE_IDLE;state<=OTA_STATE_ROLLED_BACK;state++){
  s_ui_data.ota.state=state;s_ui_data.ota_busy=state>OTA_STATE_IDLE&&state<OTA_STATE_SUCCEEDED;
  snprintf(s_ui_data.ota.version,sizeof(s_ui_data.ota.version),"0.4.11-modbus-full");
  snprintf(s_ui_data.ota.error_code,sizeof(s_ui_data.ota.error_code),"CHECKSUM_MISMATCH");local_frame();
 }
 s_ui_data.ota_busy=false;
 s_ui_data.bms_received=true;s_ui_data.bms_online=true;s_ui_data.bms.percent_divisor=10;
 s_ui_data.bms.soc_percent=785;s_ui_data.bms.soh_percent=998;s_ui_data.bms.voltage_10mv=5245;
 s_ui_data.bms.current_10ma=-105;for(unsigned i=0;i<16;i++)s_ui_data.bms.cell_mv[i]=3200+i;
 s_ui.page=UI_BMS;for(unsigned i=0;i<3;i++){s_ui.detail_page=i;local_frame();}
 s_ui.detail_page=0;s_ui_data.bms_online=false;local_frame();
 s_ui_data.inverter_battery=(direct_inverter_battery_t){.online=true,.valid=63,.values={524,(uint16_t)-123,78,251,0,0}};local_frame();
 panel_ui_t u;panel_ui_init(&u);u.language_en=true;
 live.config=(panel_config_t){1,9600,0,PANEL_PROFILE_6KU};
 for(unsigned i=0;i<panel_setting_count();i++){
  u.page=PANEL_PAGE_SETTINGS;u.selected=i;
  for(unsigned group=0;group<PANEL_SETTING_GROUPS;group++)
   if(panel_setting_group_index(group,panel_setting_group_position(group,i))==i){u.setting_group=group;break;}
  modbus_frame(&u);
  u.editing=true;u.draft=12345;modbus_frame(&u);u.editing=false;
  u.have_result=true;u.result_address=panel_setting(i)->address;u.result_value=12345;
  u.result_profile=PANEL_PROFILE_6KU;u.last_result=PANEL_WRITE_UNCERTAIN;modbus_frame(&u);u.have_result=false;
 }
 for(unsigned reg=0;reg<PANEL_REG_COUNT;reg++)panel_store(&live,reg,1,live.now_ms);
 panel_store(&live,0x42,3,live.now_ms);
 for(unsigned page=0;page<=PANEL_PAGE_DETAIL_GROUPS;page++){
  panel_ui_init(&u);u.language_en=true;u.page=page;
  unsigned steps=page==PANEL_PAGE_DETAILS?PANEL_DETAIL_PAGES:page==PANEL_PAGE_ALARMS?6:1;
  for(unsigned i=0;i<steps;i++){u.detail=i;u.selected=i;modbus_frame(&u);}
 }
 assert(!failures);
 printf("PASS English layout: %u actual LVGL local and parameter views fit their text bounds\n",views);
}
'''
path=root/'tmp/english_layout_test.c';path.write_text(test,encoding='utf-8')
shared=root.parent/'esp32_direct/main/direct'
lvgl=root/'components/lvgl__lvgl';conf=root/'full/lv_conf.h'
objects=sorted(p for p in (root/'tmp/lvgl_host').glob('*.o') if not p.name.endswith('qrcodegen.c.o'))
assert objects,'Run native_runtime_test.py first to prepare LVGL host objects'
exe=path.with_suffix('.exe')
subprocess.run([sys.argv[1],'-std=c11','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',
 '-I'+str(lvgl),'-DPANEL_LVGL_HOST_TEST=1','-DLV_CONF_PATH='+conf.as_posix(),
 '-I'+str(shared.parent/'onboarding'),'-I'+str(root.parent/'esp32_direct/components/qrcodegen'),
 '-I'+str(root/'main'),'-I'+str(root/'full'),'-I'+str(shared),'-I'+str(shared.parent/'ota'),
 '-I'+str(shared.parent/'telemetry'),str(path),str(root/'main/panel_ui.c'),str(root/'main/panel_locale.c'),
 str(root/'main/panel_model.c'),str(root/'main/panel_catalog.c'),str(shared/'direct_gfx.c'),
 str(root/'main/panel_fonts.c'),str(root/'main/panel_display_settings.c'),str(root/'full/direct_ui.c'),
 str(root/'full/panel_lvgl.c'),str(root/'full/panel_home_art.c'),str(root/'full/direct_ui_locale.c'),
 str(shared/'direct_home.c'),str(shared/'direct_boot_logo.c'),str(shared.parent/'onboarding/onboarding_qr.c'),
 str(root.parent/'esp32_direct/components/qrcodegen/qrcodegen.c'),*map(str,objects),'-o',str(exe)],check=True)
subprocess.run([str(exe)],cwd=root,check=True)
