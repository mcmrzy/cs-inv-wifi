#include "panel_ui.h"
#include "panel_catalog.h"
#include "panel_key_labels.h"
#include "direct_key_codec.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static panel_snapshot_t s;
static void value(uint16_t reg,uint16_t raw){assert(panel_store(&s,reg,raw,1000));}
int main(void){
 s.now_ms=1000;s.config=(panel_config_t){1,9600,0,PANEL_PROFILE_6KU};
 value(0x42,3);value(0x150,4);
 panel_ui_t u;panel_ui_init(&u);u.page=PANEL_PAGE_SETTINGS;
 panel_command_t c;panel_view_t v;char text[64];
 /* No readback: double ENTER must never select/send the minimum voltage. */
 panel_ui_key(&u,DIRECT_KEY_ENTER,&s,&c);assert(u.editing && u.draft==0xffff);
 panel_ui_view(&u,&s,&v);
 assert(v.setting_editor && v.footer_notice && v.key_labels[KEY_ENTER]==KEY_NONE);
 assert(strstr(v.footer,"方向键选择拟设值"));
 assert(!panel_ui_key(&u,DIRECT_KEY_ENTER,&s,&c) && !u.review);
 panel_ui_key(&u,DIRECT_KEY_RIGHT,&s,&c);assert(u.draft!=0xffff);
 panel_ui_view(&u,&s,&v);assert(strstr(v.footer,"确认保存") && v.key_labels[KEY_ENTER]==KEY_SAVE);
 assert(panel_ui_key(&u,DIRECT_KEY_ENTER,&s,&c) && c.address==0x4100 && !u.review);
 assert(!panel_ui_key(&u,DIRECT_KEY_ENTER,&s,&c));
 /* Documented setpoints provide independent current values. */
 assert(panel_setting_find(0x4108)->read_address==0x159);
 assert(panel_setting_find(0x4118)->read_address==0x15c);
 assert(panel_setting_find(0x4113)->read_address==0x17e);
 assert(panel_setting_find(0x4115)->read_address==0x180);
 assert(panel_setting_find(0x410a)->read_address==0x177);
 assert(panel_setting_find(0x410b)->read_address==0x178);
 panel_setting_format(&s,0x411b,120,text,sizeof(text));assert(!strcmp(text,"12.0 kW"));
 panel_setting_format(&s,0x4107,3,text,sizeof(text));assert(strstr(text,"磷酸铁锂"));
 panel_setting_format(&s,0x411e,4,text,sizeof(text));assert(strstr(text,"PYL"));
 panel_setting_format(&s,0x4119,0xfff2,text,sizeof(text));assert(!strcmp(text,"不限时间"));
 /* Signed generator power and fractional frequency come from 0054..0057. */
 value(0x54,2301);value(0x55,321);value(0x56,5001);value(0x57,(uint16_t)-120);
 panel_ui_init(&u);u.page=PANEL_PAGE_DETAILS;u.detail=5;
 panel_ui_view(&u,&s,&v);assert(strstr(v.title,"发电机"));
 assert(strstr(v.values[0],"230.1 V") && strstr(v.values[1],"3.21 A"));
 assert(strstr(v.values[2],"50.01 Hz") && strstr(v.values[3],"-120 W"));
 value(0x43,8);value(0x44,12);panel_ui_view(&u,&s,&v);
 assert(strstr(v.values[5],"连接正常") && strstr(v.values[6],"充电与供电"));
 value(0x8f,0xffff);value(0x91,0x8000);value(0x92,0);u.detail=3;panel_ui_view(&u,&s,&v);
 assert(!v.values[4][0] && !v.values[5][0] && !v.values[6][0]);
 value(0x9c,123);value(0x9d,65500);u.detail=6;panel_ui_view(&u,&s,&v);
 assert(strstr(v.values[0],"12.3 kWh") && strstr(v.values[1],"6550.0 kWh"));
 value(0x351,0xffff);u.detail=7;panel_ui_view(&u,&s,&v);
 assert(strstr(v.values[5],"开启") && !strstr(v.values[5],"-1"));
 s.config.profile_override=PANEL_PROFILE_4KL;u.detail=7;panel_ui_view(&u,&s,&v);
 assert(!strcmp(v.rows[3],"电池型号") && !strcmp(v.values[3],"--"));
 s.config.profile_override=PANEL_PROFILE_6KU;
 u.detail=0;value(0x53, (uint16_t)-420);
 panel_ui_view(&u,&s,&v);
 assert(!strcmp(v.rows[3],"向电网送电")&&!strcmp(v.values[3],"420 W"));
 u.detail=1;value(0x81,(uint16_t)-120);value(0x85,620);
 panel_ui_view(&u,&s,&v);
 assert(!strcmp(v.rows[3],"充电功率")&&!strcmp(v.values[3],"620 W"));
 panel_invalidate(&s,0x81);panel_ui_view(&u,&s,&v);
 assert(!strcmp(v.rows[3],"电池功率")&&!strcmp(v.values[1],"--"));
 panel_ui_init(&u);u.page=PANEL_PAGE_ALARMS;value(0x46,1);
 panel_ui_key(&u,DIRECT_KEY_DOWN,&s,&c);assert(u.selected==0);
 value(0x46,63);panel_ui_key(&u,DIRECT_KEY_DOWN,&s,&c);assert(u.selected==1);
 panel_ui_key(&u,DIRECT_KEY_DOWN,&s,&c);assert(u.selected==0);
 /* Expired current values must not seed a new edit. */
 value(0x2a1,5000);s.now_ms=7000;value(0x42,3);s.updated_ms[panel_slot(0x42)]=7000;
 panel_ui_init(&u);u.page=PANEL_PAGE_SETTINGS;u.selected=1;
 panel_ui_key(&u,DIRECT_KEY_ENTER,&s,&c);assert(u.editing && u.draft==0xffff);
 /* Back exits a function immediately, but never discards an active review. */
 panel_ui_init(&u);u.page=PANEL_PAGE_DETAILS;
 assert(panel_ui_can_exit(&u,&s,PANEL_PAGE_DETAILS));u.review=true;
 assert(!panel_ui_can_exit(&u,&s,PANEL_PAGE_DETAILS));u.review=false;s.command_busy=true;
 assert(panel_ui_can_exit(&u,&s,PANEL_PAGE_DETAILS));s.command_busy=false;
 u.page=PANEL_PAGE_SETTINGS;assert(!panel_ui_can_exit(&u,&s,PANEL_PAGE_SETTING_GROUPS));
 panel_ui_key(&u,DIRECT_KEY_BACK,&s,&c);assert(panel_ui_can_exit(&u,&s,PANEL_PAGE_SETTING_GROUPS));
 /* The 3KU charge limit uses an independent, model-specific readback address. */
 s.now_ms=1000;s.config.profile_override=PANEL_PROFILE_3KU;
 value(0x42,3);value(0x179,6000);
 panel_ui_init(&u);u.page=PANEL_PAGE_SETTINGS;
 u.selected=(uint16_t)(panel_setting_find(0x4106)-panel_setting(0));
 assert(!panel_ui_key(&u,DIRECT_KEY_ENTER,&s,&c) && u.editing && u.draft==6 && u.edit_original_valid);
 assert(!panel_ui_key(&u,DIRECT_KEY_ENTER,&s,&c) && !u.editing && strstr(u.message,"无需发送"));
 assert(!panel_ui_key(&u,DIRECT_KEY_ENTER,&s,&c) && u.editing);
 assert(!panel_ui_key(&u,DIRECT_KEY_RIGHT,&s,&c) && u.draft==7);
 assert(panel_ui_key(&u,DIRECT_KEY_ENTER,&s,&c) && c.address==0x4106 && c.value==7);
 puts("PASS user panel: deliberate unknown drafts, documented readbacks, units/enums, generator and stale edit");
}
