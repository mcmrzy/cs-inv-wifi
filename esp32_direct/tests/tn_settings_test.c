#include "direct_ui.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static direct_ui_data_t d;
static void key(direct_ui_state_t *u,unsigned k){direct_ui_key(u,(uint8_t)k,100,&d);}
static bool low_frequency(unsigned index){
 return index==9 || index==18 || index==19 || index==22 ||
  (index>=26 && index<=31) || (index>=34 && index<=35) ||
  (index>=38 && index<=39) || (index>=42 && index<=46) || index>=49;
}
static void simple_group(unsigned group){
 assert(direct_ui_group_count(group)<=9);
 for(unsigned n=0;n<direct_ui_group_count(group);n++)
  assert(!low_frequency((unsigned)direct_ui_group_param(group,n)) ||
   direct_ui_group_param(group,n)==82 || direct_ui_group_param(group,n)==83);
}
int main(void){
 direct_ui_state_t u;direct_ui_init(&u);key(&u,2);key(&u,2);
 assert(u.page==UI_CONFIG);
 assert(direct_ui_group_count(u.group)==6);
 const unsigned expected[]={0,13,1,10,4,48};
 for(unsigned i=0;i<6;i++)assert(direct_ui_group_param(u.group,i)==(int)expected[i]);
 direct_ui_view_t v;direct_ui_build(&u,&d,&v);
 assert(!strncmp(v.rows[0],"Output source:",14));
 assert(v.rows[5][0]);
 for(unsigned group=0;group<=3;group++)simple_group(group);
 simple_group(5);simple_group(8);
 key(&u,1);assert(u.page==UI_MENU && u.selected==0);
 u.selected=5;key(&u,2);assert(u.page==UI_SUBMENU);
 assert(direct_ui_submenu_count(5)>=6);
 bool all_found=false;bool advanced_seen[CONFIG_PARAM_MAX_INDEX]={0};
 for(unsigned i=0;i<direct_ui_submenu_count(5);i++){
  u.page=UI_SUBMENU;u.menu_selected=5;u.selected=(uint8_t)i;key(&u,2);
  if(u.page==UI_CONFIG && direct_ui_group_count(u.group)==CONFIG_PARAM_MAX_INDEX){
   for(unsigned n=0;n<CONFIG_PARAM_MAX_INDEX;n++)assert(direct_ui_group_param(u.group,n)==(int)n);
   all_found=true;
  }
  else if(u.page==UI_CONFIG)for(unsigned n=0;n<direct_ui_group_count(u.group);n++)
   advanced_seen[direct_ui_group_param(u.group,n)]=true;
  key(&u,1);assert(u.page==UI_SUBMENU && u.selected==i && u.menu_selected==5);
 }
 assert(all_found);
 for(unsigned n=0;n<CONFIG_PARAM_MAX_INDEX;n++)if(low_frequency(n) && n!=82 && n!=83)assert(advanced_seen[n]);
 u.page=UI_CONFIG;u.group=7;u.selected=2;u.editing=false;
 key(&u,6);assert(u.selected==8);key(&u,4);assert(u.selected==2);
 u.selected=83;key(&u,6);assert(u.selected==5);
 puts("PASS TN settings: common six, named rows, six-row paging, advanced groups, all protocol parameters reachable, remembered returns");
}
