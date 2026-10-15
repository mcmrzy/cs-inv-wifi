#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "direct_ui.h"
static direct_ui_data_t d;
static direct_ui_event_t key(direct_ui_state_t *u,unsigned k){return direct_ui_key(u,(uint8_t)k,100,&d);}
static void editor(direct_ui_state_t *u,unsigned index){direct_ui_init(u);u->page=UI_CONFIG;u->group=7;u->selected=(uint8_t)index;assert(key(u,2).action==UI_ACT_NONE && u->editing);}
int main(void){
 direct_ui_data_t alarm={0};assert(!direct_ui_has_alarm(NULL));assert(!direct_ui_has_alarm(&alarm));
 alarm.status.fault=1;assert(!direct_ui_has_alarm(&alarm));alarm.status_valid=true;assert(direct_ui_has_alarm(&alarm));
 alarm.status_valid=false;alarm.bms.protection=1;alarm.bms_online=true;assert(direct_ui_has_alarm(&alarm));
 alarm.bms_online=false;alarm.run.Warning=1;assert(!direct_ui_has_alarm(&alarm));alarm.dsp_online=true;assert(direct_ui_has_alarm(&alarm));
 char value[24];
 direct_ui_config_value(10,20,value,sizeof(value));assert(!strcmp(value,"20A"));
 direct_ui_config_value(3,200,value,sizeof(value));assert(!strcmp(value,"200Ah"));
 direct_ui_config_value(36,40,value,sizeof(value));assert(!strcmp(value,"40%"));
 direct_ui_config_value(49,6000,value,sizeof(value));assert(!strcmp(value,"6000W"));
 direct_ui_config_value(28,60,value,sizeof(value));assert(!strcmp(value,"60min"));
 direct_ui_config_value(30,7,value,sizeof(value));assert(!strcmp(value,"7d"));
 direct_ui_config_value(32,8,value,sizeof(value));assert(!strcmp(value,"8h"));
 direct_ui_state_t u;direct_ui_init(&u);d.dsp_online=true;
 key(&u,6);key(&u,6);assert(u.page==UI_PARAMETERS);key(&u,2);assert(u.page==UI_MENU);key(&u,1);assert(u.page==UI_PARAMETERS);

 for(unsigned i=0;i<84;i++){d.valid[i]=true;d.writable[i]=true;}
 direct_ui_init(&u);key(&u,6);assert(u.page==UI_BMS && !u.detail_page);
 key(&u,2);assert(u.page==UI_MENU);key(&u,1);assert(u.page==UI_BMS && u.detail_page==0);
 key(&u,3);assert(u.detail_page==1);key(&u,3);assert(u.detail_page==0);
 key(&u,5);assert(u.detail_page==1);key(&u,5);assert(u.detail_page==0);
 key(&u,6);assert(u.page==UI_PARAMETERS && !u.detail_page);
 direct_ui_init(&u);key(&u,2);u.selected=3;key(&u,2);u.selected=1;key(&u,2);
 assert(u.page==UI_PARAMETERS && u.detail_page==4 && u.parent_page==UI_SUBMENU);
 for(unsigned k=2;k<=6;k++){key(&u,k);assert(u.page==UI_PARAMETERS && u.detail_page==4);}
 key(&u,1);assert(u.page==UI_SUBMENU && u.selected==1 && u.menu_selected==3);
 /* Traverse every top-level destination and all advanced indexes through actual navigation. */
 unsigned expected[]={2,0,2,2,10,0};
 for(unsigned m=0;m<6;m++){
  direct_ui_init(&u);key(&u,2);u.selected=(uint8_t)m;key(&u,2);
  assert(direct_ui_submenu_count(m)==expected[m]);
  for(unsigned j=0;j<expected[m];j++){
   u.page=UI_SUBMENU;u.menu_selected=(uint8_t)m;u.selected=(uint8_t)j;key(&u,2);
   assert(u.page!=UI_MENU && u.page!=UI_SUBMENU);key(&u,1);
   assert(u.page==UI_SUBMENU && u.selected==j);key(&u,1);assert(u.page==UI_MENU && u.selected==m);
  }
 }
 direct_ui_init(&u);key(&u,2);u.selected=5;key(&u,2);assert(u.page==UI_CONFIG && u.group==7);
 for(unsigned i=0;i<84;i++){assert(direct_ui_group_param(u.group,u.selected)==(int)i);key(&u,5);}assert(!u.selected);
 d.config[10]=20;editor(&u,10);assert(u.draft==20);key(&u,3);assert(u.draft==30);key(&u,6);assert(u.draft==31);
 key(&u,2);assert(u.page==UI_REVIEW && !u.pending);key(&u,1);assert(u.page==UI_CONFIG && u.editing && u.draft==31);
 key(&u,2);d.dsp_online=false;d.writable[10]=false;assert(key(&u,2).action==UI_ACT_NONE && !u.pending && u.draft==31);
 d.dsp_online=true;d.writable[10]=false;assert(key(&u,2).action==UI_ACT_NONE && !u.pending);
 d.writable[10]=true;d.valid[10]=false;assert(key(&u,2).action==UI_ACT_REFRESH && !u.pending);
 d.valid[10]=true;assert(key(&u,2).action==UI_ACT_WRITE && u.pending);assert(key(&u,2).action==UI_ACT_NONE);
 direct_ui_write_result(&u,1);assert(u.pending);direct_ui_write_result(&u,-1);assert(u.page==UI_REVIEW && u.draft==31 && u.editing);
 assert(key(&u,2).action==UI_ACT_WRITE);direct_ui_write_result(&u,0);assert(u.page==UI_CONFIG && !u.editing);
 editor(&u,10);key(&u,3);key(&u,1);assert(!u.editing && d.config[10]==20);
 d.config[13]=1;editor(&u,13);key(&u,3);assert(u.draft==3);key(&u,5);assert(u.draft==1);
 d.config[8]=5000;editor(&u,8);key(&u,3);assert(u.draft==6000);key(&u,3);assert(u.draft==6000);
 d.config[26]=0;d.config[4]=4;editor(&u,26);key(&u,6);key(&u,2);assert(key(&u,2).action==UI_ACT_NONE && !u.pending);
 d.config[4]=2;assert(key(&u,2).action==UI_ACT_WRITE);direct_ui_write_result(&u,-2);assert(u.page==UI_REVIEW && u.draft==1);
 d.dsp_online=false;d.config[41]=0;editor(&u,41);key(&u,6);key(&u,2);assert(key(&u,2).action==UI_ACT_WRITE);
 direct_ui_init(&u);for(unsigned i=0;i<30;i++)assert(key(&u,7).action==UI_ACT_NONE);
 puts("PASS UI v050: six groups, all 84 reachable, hierarchy, draft/review/revalidation, duplicate block, retry, enum limits, lead-acid prerequisite, local offline policy");
}
