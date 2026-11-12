#include "panel_ui.h"
#include "panel_catalog.h"
#include "direct_key_codec.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static panel_snapshot_t s;
static panel_command_t c;
static void value(uint16_t address,uint16_t raw){assert(panel_store(&s,address,raw,s.now_ms));}
static void setup(panel_ui_t *u){
 memset(&s,0,sizeof(s));s.now_ms=1000;s.context_generation=1;
 s.config=(panel_config_t){1,4800,0,PANEL_PROFILE_6KU};value(0x42,5);value(0x150,4);value(0x2a1,5000);
 panel_ui_init(u);u->page=PANEL_PAGE_SETTINGS;u->selected=1;
}
static bool press(panel_ui_t *u,uint8_t key){return panel_ui_key(u,key,&s,&c);}
int main(void){
 panel_ui_t u;panel_view_t view;setup(&u);
 /* A context switch cancels the draft before processing a key. */
 press(&u,DIRECT_KEY_ENTER);press(&u,DIRECT_KEY_RIGHT);assert(u.editing);
 s.context_generation++;panel_ui_sync(&u,&s);assert(!u.review && !u.editing && !u.pending);
 /* A remote change cannot be overwritten using an old value. */
 setup(&u);u.selected=3;value(0x140,0);press(&u,DIRECT_KEY_ENTER);press(&u,DIRECT_KEY_RIGHT);
 value(0x140,2);
 assert(!press(&u,DIRECT_KEY_ENTER) && !u.pending && strstr(u.message,"变化"));
 /* Once a known value expires, saving its old draft cannot send a write. */
 setup(&u);press(&u,DIRECT_KEY_ENTER);press(&u,DIRECT_KEY_RIGHT);
 s.now_ms+=PANEL_FRESH_MS+1;value(0x42,5);
 assert(!press(&u,DIRECT_KEY_ENTER) && !u.pending && strstr(u.message,"失效"));
 /* A lost result unlocks conservatively; a live request never unlocks early. */
 setup(&u);press(&u,DIRECT_KEY_ENTER);press(&u,DIRECT_KEY_RIGHT);
 assert(press(&u,DIRECT_KEY_ENTER));panel_ui_submitted(&u,41,c.type);
 s.command_busy=true;s.now_ms+=2000;panel_ui_sync(&u,&s);assert(u.pending);
 s.command_busy=false;s.command_sequence=45;panel_ui_sync(&u,&s);
 assert(!u.pending && u.have_result && u.last_result==PANEL_WRITE_UNCERTAIN);
 panel_result_record(&s,41,PANEL_WRITE_CONFIRMED,0);panel_ui_sync(&u,&s);assert(u.last_result==PANEL_WRITE_UNCERTAIN);
 /* Context changes while waiting cannot adopt a receipt from another device. */
 setup(&u);press(&u,DIRECT_KEY_ENTER);press(&u,DIRECT_KEY_RIGHT);
 assert(press(&u,DIRECT_KEY_ENTER));panel_ui_submitted(&u,41,c.type);s.context_generation++;
 panel_result_record(&s,41,PANEL_WRITE_CONFIRMED,0);panel_ui_sync(&u,&s);
 assert(!u.pending && u.have_result && u.last_result==PANEL_WRITE_UNCERTAIN);
 /* Every baud accepted by persistence is editable without jumping rates. */
 setup(&u);u.page=PANEL_PAGE_COMM;u.selected=1;press(&u,DIRECT_KEY_ENTER);
 assert(u.comm_draft.baud==4800);press(&u,DIRECT_KEY_RIGHT);assert(u.comm_draft.baud==9600);
 press(&u,DIRECT_KEY_LEFT);assert(u.comm_draft.baud==4800);
 /* Other owners block writes, not data browsing or exiting a read page. */
 setup(&u);s.command_busy=true;assert(!press(&u,DIRECT_KEY_ENTER) && !u.editing);
 u.have_result=true;assert(!press(&u,DIRECT_KEY_ENTER) && !u.have_result && !u.editing);
 press(&u,DIRECT_KEY_DOWN);assert(u.selected==2);
 u.page=PANEL_PAGE_DETAILS;assert(panel_ui_can_exit(&u,&s,PANEL_PAGE_DETAILS));
 /* A fixed sales SKU prevents a profile change and blocks model-dependent
  * settings when the inverter reports a different known candidate. */
 setup(&u);s.profile_locked=true;s.detected_profile=PANEL_PROFILE_12KU;
 value(0x38,1);value(0x39,0);
 assert(!panel_setting_allowed(&s,0x4100,1) &&
        panel_setting_access(&s,0x4100)==PANEL_ACCESS_PROFILE_CONFLICT);
 assert(panel_setting_allowed(&s,0x4101,1));
 u.page=PANEL_PAGE_COMM;u.selected=3;u.language_en=true;
 assert(!press(&u,DIRECT_KEY_ENTER) && !u.editing);
 panel_ui_view(&u,&s,&view);assert(strstr(view.rows[3],"Factory model") && strstr(view.footer,"locked"));
 /* Model-only setup cancels directly back to the precise setting. */
 setup(&u);s.config.profile_override=PANEL_PROFILE_UNKNOWN;u.selected=0;
 press(&u,DIRECT_KEY_ENTER);press(&u,DIRECT_KEY_ENTER);assert(u.model_setup && u.editing);
 press(&u,DIRECT_KEY_BACK);assert(!u.model_setup && u.page==PANEL_PAGE_SETTINGS && u.selected==0);
 /* Listed known current values remain distinct from unprovided readback. */
 setup(&u);panel_ui_view(&u,&s,&view);assert(strstr(view.values[1],"50 Hz") && !strcmp(view.values[0],"正在读取"));
 s.now_ms+=6000;panel_ui_view(&u,&s,&view);assert(!strchr(view.values[1],'*') && (view.stale_rows&2));
 /* Missing one alarm word must not hide a known fresh active fault. */
 setup(&u);u.page=PANEL_PAGE_ALARMS;value(0x46,1);
 panel_ui_view(&u,&s,&view);assert(view.alert && strstr(view.rows[2],"F01"));
 assert(strstr(view.subtitle,"部分"));
 s.now_ms+=PANEL_FRESH_MS+1;value(0x48,1);
 panel_ui_view(&u,&s,&view);
 assert(view.alert && strstr(view.rows[2],"W01 ") && strstr(view.rows[3],"历史 F01"));
 assert(!strcmp(view.rows[0],"当前 1  /  上次 1"));
 s.now_ms+=PANEL_FRESH_MS+1;panel_ui_view(&u,&s,&view);
 assert(!view.alert && !strcmp(view.rows[0],"当前待确认  /  上次 2"));
 setup(&u);u.page=PANEL_PAGE_ALARMS;
 for(uint16_t reg=0x46;reg<=0x49;reg++)value(reg,0);
 s.now_ms+=PANEL_FRESH_MS+1;value(0x49,0);panel_ui_view(&u,&s,&view);
 assert(!view.alert && strstr(view.rows[2],"待确认"));
 /* Receipt values are interpreted with the model used by that request. */
  setup(&u);u.selected=0;press(&u,DIRECT_KEY_ENTER);press(&u,DIRECT_KEY_RIGHT);
 assert(press(&u,DIRECT_KEY_ENTER));panel_ui_submitted(&u,41,c.type);
 s.context_generation++;s.config.profile_override=PANEL_PROFILE_3KU;panel_ui_sync(&u,&s);
 panel_ui_view(&u,&s,&view);assert(strstr(view.rows[0],"208 V") && !strstr(view.rows[0],"100 V"));
 /* Unsigned accumulated/absolute channels never become negative at bit 15. */
 setup(&u);u.page=PANEL_PAGE_DETAILS;u.detail=2;value(0x98,40000);
 panel_ui_view(&u,&s,&view);assert(!strcmp(view.values[2],"40000 W"));
 assert(!strcmp(view.values[0],"--"));
 setup(&u);u.setting_group=1;panel_invalidate(&s,0x2a1);
 assert(!press(&u,DIRECT_KEY_ENTER)&&u.editing);
 assert(!press(&u,DIRECT_KEY_RIGHT)&&u.editing);
 assert(press(&u,DIRECT_KEY_ENTER)&&c.address==0x4101&&!u.review&&u.pending);
 puts("PASS native state: context/remote changes, lost receipts, baud preservation, browse during busy, model cancel and current value columns");
}
