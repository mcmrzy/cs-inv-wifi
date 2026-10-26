#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "direct_ui.h"
#include "direct_key_codec.h"
static direct_ui_data_t data;
static direct_ui_event_t press(direct_ui_state_t *u,unsigned k){return direct_ui_key(u,(uint8_t)k,100,&data);}
int main(void){
 const uint16_t frames[]={0xa1ef,0xa1f7,0xa1df,0xa1fb,0xa1fd,0xa1fe};
 const unsigned keys[]={DIRECT_KEY_BACK,DIRECT_KEY_LEFT,DIRECT_KEY_UP,DIRECT_KEY_DOWN,DIRECT_KEY_RIGHT,DIRECT_KEY_ENTER};
 for(unsigned i=0;i<6;i++)assert(direct_key_decode(frames[i])==(int)keys[i]);
 direct_ui_state_t u;direct_ui_init(&u);data.dsp_online=true;
 press(&u,DIRECT_KEY_RIGHT);assert(u.page==UI_BMS);
 press(&u,DIRECT_KEY_DOWN);assert(u.detail_page==1);
 press(&u,DIRECT_KEY_ENTER);assert(u.page==UI_MENU); /* consistent confirm opens menu */
 press(&u,DIRECT_KEY_BACK);assert(u.page==UI_BMS && u.detail_page==1);
 press(&u,DIRECT_KEY_RIGHT);assert(u.page==UI_PARAMETERS);
 press(&u,DIRECT_KEY_DOWN);press(&u,DIRECT_KEY_DOWN);assert(u.detail_page==2);
 press(&u,DIRECT_KEY_LEFT);assert(u.page==UI_BMS && u.detail_page==1);
 press(&u,DIRECT_KEY_RIGHT);assert(u.page==UI_PARAMETERS && u.detail_page==2);
 press(&u,DIRECT_KEY_RIGHT);assert(u.page==UI_ALARM);
 press(&u,DIRECT_KEY_ENTER);assert(u.page==UI_MENU);
 u.selected=1;press(&u,DIRECT_KEY_RIGHT);assert(u.selected==1); /* no diagonal row crossing */
 press(&u,DIRECT_KEY_DOWN);assert(u.selected==3);press(&u,DIRECT_KEY_LEFT);assert(u.selected==2);
 press(&u,DIRECT_KEY_UP);assert(u.selected==0);press(&u,DIRECT_KEY_LEFT);assert(u.selected==0);
 u.page=UI_CONFIG;u.group=7;u.selected=2;u.editing=false;
 press(&u,DIRECT_KEY_RIGHT);assert(u.selected==8);
 press(&u,DIRECT_KEY_LEFT);assert(u.selected==2);
 u.selected=83;press(&u,DIRECT_KEY_RIGHT);assert(u.selected==5);
 u.page=UI_SUBMENU;u.menu_selected=4;u.selected=0;
 press(&u,DIRECT_KEY_RIGHT);assert(u.selected==6);
 press(&u,DIRECT_KEY_LEFT);assert(u.selected==0);
 u.selected=7;direct_ui_view_t view;direct_ui_build(&u,&data,&view);
 assert(view.selected==1 && !strcmp(view.rows[1],"Phone connection"));
 u.page=UI_FACTORY;u.confirm=false;press(&u,DIRECT_KEY_LEFT);assert(!u.confirm);
 press(&u,DIRECT_KEY_RIGHT);assert(u.confirm);press(&u,DIRECT_KEY_RIGHT);assert(u.confirm);
 u.page=UI_HOME;u.parent_page=UI_HOME;u.main_mode=1;press(&u,DIRECT_KEY_BACK);assert(u.main_mode==0);
 const char *hints[6];direct_ui_key_hints(&u,&data,hints);
 assert(!strcmp(hints[1],"Prev") && !strcmp(hints[4],"Next") && !strcmp(hints[5],"Menu"));
 u.page=UI_CONFIG;u.editing=true;direct_ui_key_hints(&u,&data,hints);
 assert(!strcmp(hints[0],"Cancel") && !strcmp(hints[1],"-") && !strcmp(hints[2],"Fast+") && !strcmp(hints[3],"Fast-") && !strcmp(hints[4],"+") && !strcmp(hints[5],"Review"));
 u.pending=true;direct_ui_key_hints(&u,&data,hints);for(unsigned i=0;i<6;i++)assert(!strcmp(hints[i],"--"));
 puts("PASS physical six-key layout: decode order, menu on confirm, remembered pages, geometric grid, list paging, bounded submenu, safe Yes/No and layered exit");
}
