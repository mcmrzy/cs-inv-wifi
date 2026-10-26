#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "direct_ui.h"
#include "direct_ui_assets.h"
#include "direct_ui_locale.h"

int main(void) {
    assert(direct_ui_menu_count()==6);
    for(unsigned i=0;i<direct_ui_menu_count();++i)assert(direct_ui_menu_name(i) && direct_ui_menu_name(i)[0]);
    assert(!direct_ui_menu_name(6) && !direct_ui_menu_name(65535));
    assert(direct_ui_group_count(7)==84 && direct_ui_group_count(DIRECT_UI_GROUPS)==0);
    bool reachable[84]={0};
    for(unsigned group=0;group<DIRECT_UI_GROUPS;++group){
        for(unsigned position=0;position<direct_ui_group_count(group);++position){
            int index=direct_ui_group_param(group,position);
            assert(index>=0 && index<84);reachable[index]=true;
            if(group==7)assert(index==(int)position);
        }
        assert(direct_ui_group_param(group,direct_ui_group_count(group))==-1);
    }
    for(unsigned i=0;i<84;++i)assert(reachable[i]);
    assert(direct_ui_group_param(DIRECT_UI_GROUPS,0)==-1 && direct_ui_group_param(65535,0)==-1);
    char formatted[32];
    direct_ui_config_value(7,2200,formatted,sizeof(formatted));assert(!strcmp(formatted,"220.0V"));
    direct_ui_config_value(8,5000,formatted,sizeof(formatted));assert(!strcmp(formatted,"50.00Hz"));
    direct_ui_config_value(13,3,formatted,sizeof(formatted));assert(!strcmp(formatted,"Solar+Utility"));
    direct_ui_config_value(4,4,formatted,sizeof(formatted));assert(!strcmp(formatted,"Lithium"));
    direct_ui_config_value(83,560,formatted,sizeof(formatted));assert(!strcmp(formatted,"56.0V"));
    direct_ui_config_value(84,1,formatted,sizeof(formatted));assert(!strcmp(formatted,"--"));
    direct_ui_config_value(0,1,NULL,0);formatted[0]='X';
    direct_ui_config_value(0,1,formatted,0);assert(formatted[0]=='X');
    direct_ui_config_value(7,2200,formatted,1);assert(formatted[0]==0);
    direct_ui_state_t u;direct_ui_data_t d={0};direct_ui_view_t view;
    direct_ui_init(&u);d.dsp_online=true;d.version="test";d.sn="SN";
    direct_ui_build(&u,&d,&view);assert(strstr(view.title,"Network offline") && !strstr(view.title,"DSP"));
    d.network_online=true;d.dsp_online=false;
    direct_ui_build(&u,&d,&view);assert(strstr(view.title,"Network online"));
    d.dsp_online=true;d.network_online=false;
    direct_ui_key(&u,6,100,&d);assert(u.page==UI_BMS);
    direct_ui_key(&u,6,101,&d);assert(u.page==UI_PARAMETERS);
    for(unsigned i=0;i<DIRECT_UI_PARAMETER_PAGES;i++){
        assert(u.detail_page==i);direct_ui_build(&u,&d,&view);
        assert(strstr(view.title,"Running parameters"));direct_ui_key(&u,5,102+i,&d);
    }
    assert(u.detail_page==0);direct_ui_key(&u,1,110,&d);assert(u.page==UI_HOME);
    direct_ui_key(&u,2,111,&d);assert(u.page==UI_MENU);
    for(unsigned menu=0;menu<6;menu++){
        u.page=UI_MENU;u.selected=(uint8_t)menu;direct_ui_key(&u,2,120+menu,&d);
        assert(u.page==UI_SUBMENU || u.page==UI_CONFIG);direct_ui_build(&u,&d,&view);assert(view.title[0]);
    }
    u.page=UI_SUBMENU;u.menu_selected=4;u.selected=1;d.language_zh=true;
    direct_ui_key(&u,2,130,&d);assert(u.page==UI_APPEARANCE && !u.appearance_theme && u.appearance_language);
    direct_ui_event_t style=direct_ui_key(&u,6,131,&d);assert(style.action==UI_ACT_APPEARANCE_PREVIEW && style.value==3);
    direct_ui_key(&u,5,132,&d);style=direct_ui_key(&u,4,133,&d);assert(style.value==1);
    direct_ui_key(&u,5,134,&d);style=direct_ui_key(&u,2,135,&d);assert(style.action==UI_ACT_APPEARANCE_SAVE);
    style=direct_ui_key(&u,1,136,&d);assert(style.action==UI_ACT_APPEARANCE_CANCEL && u.page==UI_SUBMENU && u.selected==1);
    u.page=UI_CONFIG;u.group=7;u.selected=7;u.editing=false;
    assert(direct_ui_key(&u,2,140,&d).action==UI_ACT_REFRESH && !u.editing);
    d.valid[7]=true;d.config[7]=2200;d.writable[7]=false;direct_ui_key(&u,2,141,&d);assert(!u.editing);
    d.writable[7]=true;direct_ui_key(&u,2,142,&d);assert(u.editing && u.draft==2200);
    direct_ui_key(&u,6,143,&d);assert(u.draft==2210);
    assert(direct_ui_key(&u,2,144,&d).action==UI_ACT_NONE && u.page==UI_REVIEW);
    direct_ui_event_t event=direct_ui_key(&u,2,145,&d);assert(event.action==UI_ACT_WRITE && event.index==7 && event.value==2210);
    direct_ui_write_result(&u,1);assert(u.pending);direct_ui_write_result(&u,-1);assert(!u.pending && u.editing && u.draft==2210 && u.page==UI_REVIEW);
    u.draft=2410;assert(direct_ui_key(&u,2,146,&d).action==UI_ACT_NONE && !u.pending);
    u.page=UI_CONFIG;u.editing=false;u.selected=13;d.valid[13]=true;d.writable[13]=true;d.config[13]=1;
    direct_ui_key(&u,2,150,&d);direct_ui_key(&u,6,151,&d);assert(u.draft==3);
    assert(direct_ui_value_allowed(8,5000) && direct_ui_value_allowed(8,6000));assert(!direct_ui_value_allowed(8,5500) && !direct_ui_value_allowed(13,2));
    d.dsp_online=false;u.editing=false;direct_ui_key(&u,2,152,&d);assert(u.editing); /* saved controller settings remain editable */
    u.page=UI_FACTORY;u.confirm=false;u.group=0;assert(direct_ui_key(&u,2,160,&d).action==UI_ACT_NONE);
    direct_ui_key(&u,6,161,&d);assert(direct_ui_key(&u,2,162,&d).action==UI_ACT_LOCAL_RESET);
    u.group=2;assert(direct_ui_key(&u,2,163,&d).action==UI_ACT_LCD_RESET);
    u.page=UI_HOME;u.parent_page=UI_HOME;for(unsigned i=0;i<20;i++)assert(direct_ui_key(&u,7,170+i,&d).action==UI_ACT_NONE);
    int time_local[]={2026,10,8,16,0,0,480},time_utc[]={2026,10,8,8,0,0,0};int64_t a,b;
    assert(direct_ui_time_utc(time_local,&a) && direct_ui_time_utc(time_utc,&b) && a==b);
    int leap[]={2024,2,29,0,0,0,0};assert(direct_ui_time_utc(leap,&a) && a==1709164800);
    leap[0]=2026;assert(!direct_ui_time_utc(leap,&a));
    leap[0]=2024;leap[1]=4;leap[2]=31;assert(!direct_ui_time_utc(leap,&a));
    for(unsigned i=0;i<84;i++)assert(strcmp(direct_ui_param_name(i),"Unknown") && direct_ui_param_name(i)[0]);
    for(unsigned c=32;c<=127;c++){const direct_glyph_t *g=direct_ui_glyph((uint8_t)c);assert(g && g->height==14 && g->width && g->stride);}
    assert(!direct_ui_glyph(0));assert(direct_alarm_message(DIRECT_ALARM_WARNING,63));
    assert(direct_alarm_message(DIRECT_ALARM_BMS,36));assert(!direct_alarm_message(DIRECT_ALARM_BMS,37));
    memset(&d,0,sizeof(d));u.page=UI_ALARM;u.selected=0;d.status_valid=true;d.status.warning[0]=1;
    direct_ui_build(&u,&d,&view);assert(strstr(view.rows[2],direct_alarm_message(DIRECT_ALARM_WARNING,0)));
    d.status.warning[0]=0;d.status.warning[1]=1;direct_ui_build(&u,&d,&view);
    assert(strstr(view.rows[2],direct_alarm_message(DIRECT_ALARM_WARNING,16)));
    d.status.warning[0]=65535;d.status.warning[1]=65535;d.status.warning[2]=65535;d.status.fault=0xffffffff;d.bms_online=true;d.bms.warning=65535;d.bms.protection=32767;
    for(unsigned i=0;i<70;i++)direct_ui_key(&u,5,9000+i,&d);
    assert(u.selected==70);direct_ui_build(&u,&d,&view);assert(view.rows[2][0]);
    d.status_valid=false;d.bms_online=false;d.run.Warning=UINT64_MAX;d.run.FaultValue=UINT32_MAX;
    u.selected=0;direct_ui_key(&u,5,10000,&d);assert(u.selected==0);
    direct_ui_build(&u,&d,&view);assert(strstr(view.rows[2],"Alarm status unavailable"));
    assert(!strstr(view.rows[2],"No active"));
    d.bms_online=true;d.bms.warning=1;d.bms.protection=0;
    direct_ui_build(&u,&d,&view);assert(strstr(view.rows[2],direct_alarm_message(DIRECT_ALARM_BMS,0)));
    d.bms_online=false;u.page=UI_PARAMETERS;u.detail_page=4;
    direct_ui_build(&u,&d,&view);assert(strstr(view.title,"5/5"));
    assert(strstr(view.rows[1],"unavailable") && strstr(view.rows[4],"--"));
    d.status_valid=true;d.status.inverter_status[0]=3;d.status.mode=16;
    direct_ui_build(&u,&d,&view);assert(strstr(view.rows[1],"valid") && strstr(view.rows[2],"valid"));
    assert(strstr(view.rows[3],"16") && strstr(view.rows[7],"unavailable"));
    d.language_zh=true;d.status_valid=false;d.bms_online=true;d.bms.warning=1U<<10;d.bms.protection=0;
    u.page=UI_ALARM;u.selected=0;direct_ui_build(&u,&d,&view);
    char chinese_alarm[256];
    const char *translated=direct_ui_translate(direct_alarm_message(DIRECT_ALARM_BMS,10),true,chinese_alarm,sizeof(chinese_alarm));
    assert(!strcmp(view.rows[2],translated)); /* Translate the full long alarm before wrapping. */
    puts("PASS UI: navigation, appearance preview/save/cancel, unknown/offline writes, units, confirmation, keys, UTC/date, UTF8 alarm lines");
}
