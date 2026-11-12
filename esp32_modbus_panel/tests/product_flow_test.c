#include "panel_ui.h"
#include "panel_catalog.h"
#include "panel_key_labels.h"
#include "direct_key_codec.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static panel_snapshot_t s;
static panel_command_t c;
static void set(uint16_t r,uint16_t v){assert(panel_store(&s,r,v,s.now_ms));}
static bool press(panel_ui_t *u,uint8_t key){panel_ui_sync(u,&s);return panel_ui_key(u,key,&s,&c);}
int main(void){
 s.now_ms=1000;s.config=(panel_config_t){1,9600,0,PANEL_PROFILE_UNKNOWN};
 set(0x42,5);set(0x150,4);set(0x2a1,5000);
 panel_ui_t u;panel_ui_init(&u);u.page=PANEL_PAGE_SETTINGS;u.selected=0;
 panel_view_t v;
 /* Log reproduction: frequency works, UP selects model-specific voltage. */
 panel_ui_view(&u,&s,&v);
 assert(v.setting_list && v.footer_notice && strstr(v.footer,"请先确认实际机型"));
 assert(v.key_labels[KEY_ENTER]==KEY_INFO);
 assert(!press(&u,DIRECT_KEY_ENTER));assert(u.blocked && !u.editing);
 panel_ui_view(&u,&s,&v);assert(strstr(v.rows[1],"机型") && strstr(v.footer,"确认机型"));
 assert(v.key_labels[KEY_ENTER]==KEY_OPEN);
 assert(!press(&u,DIRECT_KEY_ENTER));assert(u.page==PANEL_PAGE_COMM && u.model_setup && u.selected==3 && u.editing);
 assert(!press(&u,DIRECT_KEY_ENTER) && !u.review); /* Unknown is never a confirmed model. */
 const panel_profile_t profiles[]={PANEL_PROFILE_2KU,PANEL_PROFILE_3KU,PANEL_PROFILE_6KU,
     PANEL_PROFILE_12KU,PANEL_PROFILE_UNKNOWN,PANEL_PROFILE_2KU,PANEL_PROFILE_3KU,PANEL_PROFILE_6KU};
 for(unsigned i=0;i<sizeof(profiles)/sizeof(profiles[0]);i++){
     assert(!press(&u,DIRECT_KEY_RIGHT) && u.comm_draft.profile_override==profiles[i]);
 }
 assert(!press(&u,DIRECT_KEY_ENTER) && u.review);
 assert(press(&u,DIRECT_KEY_ENTER) && c.type==PANEL_CMD_CONFIG);
 panel_ui_submitted(&u,41,c.type);s.config=c.config;panel_result_record(&s,41,PANEL_WRITE_IDLE,0);
 panel_ui_sync(&u,&s);assert(!u.model_setup && u.page==PANEL_PAGE_SETTINGS && u.selected==0);
 assert(!press(&u,DIRECT_KEY_ENTER) && u.editing);
 /* No-op frequency changes do not create a write or telemetry outage. */
 panel_ui_init(&u);u.page=PANEL_PAGE_SETTINGS;u.selected=1;
 assert(!press(&u,DIRECT_KEY_ENTER) && u.draft==0);
 assert(!press(&u,DIRECT_KEY_ENTER) && !u.pending && !u.review);
 assert(strstr(u.message,"未改变"));
 panel_ui_init(&u);u.page=PANEL_PAGE_SETTINGS;u.selected=1;
 press(&u,DIRECT_KEY_ENTER);press(&u,DIRECT_KEY_UP);assert(u.draft==1);
 press(&u,DIRECT_KEY_DOWN);assert(u.draft==0);
 press(&u,DIRECT_KEY_LEFT);assert(strstr(u.message,"边界"));
 u.editing=false;u.have_result=true;u.result_address=0x4101;u.result_value=1;
 u.last_result=PANEL_WRITE_REJECTED;u.result_exception=3;
 panel_ui_view(&u,&s,&v);assert(strstr(v.rows[3],"拒绝参数值"));
 assert(!press(&u,DIRECT_KEY_ENTER) && !u.have_result && !u.editing && !u.pending);
 /* Restriction reasons identify fixed lead values / standby independently. */
 set(0x150,0);assert(panel_setting_access(&s,0x410a)==PANEL_ACCESS_FIXED);
 panel_ui_init(&u);u.page=PANEL_PAGE_SETTINGS;
 u.selected=(uint16_t)(panel_setting_find(0x410a)-panel_setting(0));
 panel_ui_view(&u,&s,&v);
 assert(v.setting_list && v.footer_notice && v.key_labels[KEY_ENTER]==KEY_INFO && strstr(v.footer,"固定"));
 assert(!press(&u,DIRECT_KEY_ENTER) && u.blocked && !u.editing);
 panel_ui_view(&u,&s,&v);
 assert(v.key_labels[KEY_ENTER]==KEY_NONE && v.key_labels[KEY_BACK]==KEY_CLOSE && !v.rows[4][0]);
 assert(!press(&u,DIRECT_KEY_ENTER) && u.blocked && !u.pending);
 assert(!press(&u,DIRECT_KEY_BACK) && !u.blocked && u.page==PANEL_PAGE_SETTINGS);
 s.config.profile_override=PANEL_PROFILE_12KU;
 assert(panel_setting_access(&s,0x4110)==PANEL_ACCESS_STANDBY);
 u.selected=(uint16_t)(panel_setting_find(0x4110)-panel_setting(0));
 assert(!press(&u,DIRECT_KEY_ENTER) && u.blocked && !u.editing);
 set(0x42,2);panel_ui_sync(&u,&s);
 assert(!u.blocked && !u.editing);
 panel_ui_view(&u,&s,&v);
 assert(v.key_labels[KEY_ENTER]==KEY_EDIT && !v.footer_notice);
 s.now_ms=7001;assert(panel_setting_access(&s,0x4102)==PANEL_ACCESS_OFFLINE);
 puts("PASS product flow: blocked setting recovery/model confirmation, restored selection, no-op write suppression and explicit reasons");
}
