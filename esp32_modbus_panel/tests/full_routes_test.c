#include "direct_ui.h"
#include "full_ui_bridge.h"
#include "ota_image_identity.h"
#include "panel_types.h"
#include "full_board_pins.h"
#include "full_write_result.h"
#include "panel_display_settings.h"
#include "direct_key_codec.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    static direct_ui_data_t d;
    direct_ui_state_t u;direct_ui_init(&u);
    assert(direct_ui_menu_count()==5);
    assert(direct_ui_submenu_count(4)==7);
    assert(direct_ui_submenu_count(5)==8);
    u.page=UI_MENU;u.selected=2;
    direct_ui_event_t e=direct_ui_key(&u,2,100,&d);
    assert(e.action==FULL_UI_OPEN_MODBUS && e.index==PANEL_PAGE_POWER);
    const panel_page_t destinations[]={PANEL_PAGE_DETAIL_GROUPS,PANEL_PAGE_SETTING_GROUPS,PANEL_PAGE_POWER,PANEL_PAGE_ALARMS};
    for(unsigned i=0;i<4;i++){
        direct_ui_init(&u);u.page=UI_MENU;u.selected=(uint8_t)i;
        e=direct_ui_key(&u,2,100,&d);
        assert(e.action==FULL_UI_OPEN_MODBUS && e.index==destinations[i] && u.page==UI_MENU && u.selected==i);
    }
    for(unsigned i=0;i<2;i++){
        direct_ui_init(&u);u.page=UI_SUBMENU;u.menu_selected=5;u.selected=(uint8_t)(i+1);
        e=direct_ui_key(&u,2,100,&d);
        assert(e.action==FULL_UI_OPEN_MODBUS && e.index==(i?PANEL_PAGE_DIAG:PANEL_PAGE_COMM));
    }
    direct_ui_init(&u);e=direct_ui_key(&u,6,100,&d);
    assert(e.action==UI_ACT_NONE && u.page==UI_BMS && u.detail_page==0);
    direct_ui_view_t bms_view;
    d.language_zh=true;
    direct_ui_build(&u,&d,&bms_view);
    assert(strstr(bms_view.rows[0],"电池未连接") && !strstr(bms_view.rows[1],"0.00"));
    d.bms_received=true;d.bms_online=true;d.bms.voltage_10mv=5245;
    d.bms.current_10ma=-105;d.bms.percent_divisor=10;d.bms.soc_percent=785;d.bms.soh_percent=998;
    for(unsigned i=0;i<16;i++)d.bms.cell_mv[i]=(uint16_t)(3200+i);
    direct_ui_build(&u,&d,&bms_view);
    assert(strstr(bms_view.rows[0],"电池已连接") && strstr(bms_view.rows[1],"52.45 V"));
    assert(strstr(bms_view.rows[2],"-1.05 A") && strstr(bms_view.rows[3],"78%") && strstr(bms_view.rows[4],"99%"));
    assert(strstr(bms_view.rows[7],"15 mV"));
    direct_ui_key(&u,6,101,&d);assert(u.page==UI_BMS && u.detail_page==1);
    direct_ui_build(&u,&d,&bms_view);assert(strstr(bms_view.rows[1],"01: 3200") && strstr(bms_view.rows[8],"08: 3207"));
    direct_ui_key(&u,6,102,&d);assert(u.detail_page==2);
    direct_ui_build(&u,&d,&bms_view);assert(strstr(bms_view.rows[1],"09: 3208") && strstr(bms_view.rows[8],"16: 3215"));
    d.bms_online=false;direct_ui_build(&u,&d,&bms_view);
    assert(strstr(bms_view.rows[0],"已断联") && strstr(bms_view.rows[1],"3208"));
    d.bms_online=true;direct_ui_build(&u,&d,&bms_view);assert(strstr(bms_view.rows[0],"电池已连接"));
    u.detail_page=0;d.inverter_battery=(direct_inverter_battery_t){.valid=7,.online=true,.values={520,(uint16_t)-12,67}};
    direct_ui_build(&u,&d,&bms_view);
    assert(strstr(bms_view.rows[0],"电池已连接") && strstr(bms_view.rows[3],"78%"));
    assert(full_battery_source(&d)==FULL_BATTERY_DIRECT && full_battery_pages(&d)==3);
    direct_ui_key(&u,DIRECT_KEY_RIGHT,102,&d);assert(u.detail_page==1);
    direct_ui_key(&u,DIRECT_KEY_LEFT,102,&d);assert(u.detail_page==0);
    direct_ui_build(&u,&d,&bms_view);assert(!strstr(bms_view.rows[0],"直连") && strstr(bms_view.rows[3],"78%"));
    e=direct_ui_key(&u,DIRECT_KEY_ENTER,102,&d);assert(e.action==UI_ACT_NONE && u.page==UI_BMS_PROTOCOL);
    direct_ui_build(&u,&d,&bms_view);
    assert(strstr(bms_view.rows[0],"自动识别") && bms_view.selected==0);
    e=direct_ui_key(&u,DIRECT_KEY_ENTER,102,&d);assert(e.action==FULL_UI_DETECT_BMS);
    d.bms_detection=PANEL_BMS_DETECT_RUNNING;
    e=direct_ui_key(&u,DIRECT_KEY_ENTER,102,&d);assert(e.action==UI_ACT_NONE);
    d.bms_detection=PANEL_BMS_DETECT_MISSING;
    direct_ui_build(&u,&d,&bms_view);assert(strstr(bms_view.rows[4],"未识别"));
    d.bms_detection=PANEL_BMS_DETECT_IDLE;
    direct_ui_key(&u,DIRECT_KEY_DOWN,102,&d);
    direct_ui_key(&u,DIRECT_KEY_ENTER,102,&d);assert(u.editing);
    direct_ui_build(&u,&d,&bms_view);
    assert(strstr(bms_view.rows[0],"BBJ (0x7C)") && strstr(bms_view.rows[1],"CSERGY Modbus") && bms_view.selected==0);
    assert(strstr(bms_view.rows[3],"通信正常"));
    d.bms_received=false;d.bms_online=false;
    direct_ui_build(&u,&d,&bms_view);
    assert(strstr(bms_view.rows[3],"正在检测")); /* Inverter data must not mark UART1 online. */
    d.bms_no_reply=true;direct_ui_build(&u,&d,&bms_view);
    assert(strstr(bms_view.rows[3],"无有效应答"));
    d.bms_no_reply=false;d.bms_received=true;direct_ui_build(&u,&d,&bms_view);
    assert(strstr(bms_view.rows[3],"已断联"));
    d.bms_online=true;
    direct_ui_key(&u,DIRECT_KEY_DOWN,102,&d);
    e=direct_ui_key(&u,DIRECT_KEY_ENTER,102,&d);assert(e.action==FULL_UI_SAVE_BMS_PROTOCOL && e.value==PANEL_BMS_CSERGY);
    direct_ui_key(&u,DIRECT_KEY_DOWN,102,&d);
    e=direct_ui_key(&u,DIRECT_KEY_ENTER,102,&d);assert(e.action==FULL_UI_SAVE_BMS_PROTOCOL && e.value==PANEL_BMS_BBJ);
    direct_ui_key(&u,DIRECT_KEY_BACK,102,&d);assert(u.page==UI_BMS_PROTOCOL && !u.editing);
    direct_ui_key(&u,DIRECT_KEY_BACK,102,&d);assert(u.page==UI_BMS);
    u.detail_page=0;d.inverter_battery.online=false;direct_ui_build(&u,&d,&bms_view);
    assert(strstr(bms_view.rows[0],"电池已连接") && strstr(bms_view.rows[3],"78%"));
    d.bms_online=false;direct_ui_build(&u,&d,&bms_view);
    assert(strstr(bms_view.rows[0],"上次数据") && strstr(bms_view.rows[3],"78%"));
    /* Legacy inverter caches cannot create a second battery or an online pack. */
    d.inverter_battery.online=true;d.bms_received=false;d.bms_online=false;
    direct_ui_build(&u,&d,&bms_view);
    assert(full_battery_source(&d)==FULL_BATTERY_NONE && full_battery_pages(&d)==1);
    assert(strstr(bms_view.rows[0],"电池未连接") && !strstr(bms_view.rows[1],"52.0"));
    d.bms_received=true;d.bms_online=true;d.bms.voltage_10mv=327670;
    direct_ui_build(&u,&d,&bms_view);
    assert(strstr(bms_view.rows[0],"电池未连接") && !strstr(bms_view.rows[1],"3276"));
    d.bms.voltage_10mv=5245;d.bms_protocol=PANEL_BMS_CSERGY;d.bms.cell_mv[31]=3400;
    assert(full_battery_pages(&d)==5);
    d.bms.cell_mv[31]=0;assert(full_battery_pages(&d)==3);
    u.parent_page=UI_PARAMETERS;
    e=direct_ui_key(&u,DIRECT_KEY_BACK,103,&d);
    assert(e.action==FULL_UI_OPEN_MODBUS && e.index==PANEL_PAGE_DETAIL_GROUPS && e.value==3);
    assert(u.parent_page==UI_HOME);
    direct_ui_key(&u,1,103,&d);assert(u.page==UI_HOME);
    d.language_zh=false;d.bms_received=false;d.bms_online=false;
    direct_ui_init(&u);e=direct_ui_key(&u,4,100,&d);
    assert(e.action==FULL_UI_OPEN_MODBUS && e.index==PANEL_PAGE_ALARMS);
    direct_ui_init(&u);e=direct_ui_key(&u,5,100,&d);
    assert(e.action==FULL_UI_OPEN_MODBUS && e.index==PANEL_PAGE_DETAILS && e.value==0);
    e=direct_ui_key(&u,3,101,&d);
    assert(e.action==FULL_UI_OPEN_MODBUS && e.value==PANEL_DETAIL_PAGES-1);
    const char *hints[6];
    direct_ui_key_hints(&u,&d,hints);
    assert(!strcmp(hints[2],"Modes") && !strcmp(hints[3],"Grid"));
    direct_ui_key(&u,2,102,&d);assert(u.page==UI_MENU && u.selected==0);
    direct_ui_key(&u,DIRECT_KEY_DOWN,103,&d);assert(u.selected==3);
    direct_ui_key(&u,DIRECT_KEY_RIGHT,104,&d);assert(u.selected==4);
    direct_ui_key(&u,DIRECT_KEY_UP,105,&d);assert(u.selected==2);
    direct_ui_key(&u,DIRECT_KEY_LEFT,106,&d);assert(u.selected==0);
    const uint8_t neighbours[4][5]={{0,1,2,0,2},{1,1,0,1,3},{3,1,4,3,4},{2,0,2,4,4}};
    for(unsigned i=0;i<5;i++)for(uint8_t key=3;key<=6;key++){
        u.page=UI_MENU;u.selected=(uint8_t)i;
        direct_ui_key(&u,key,107,&d);
        assert(u.selected==neighbours[key-3][i]);
    }
    const unsigned selections[]={0,1,2,3,4,5,6};
    const direct_ui_page_t pages[]={UI_APPEARANCE,UI_TIME,UI_HELP,UI_PHONE_SETUP,UI_UPGRADE,UI_WIFI,UI_BMS_PROTOCOL};
    for(unsigned i=0;i<7;i++){
        direct_ui_init(&u);u.page=UI_SUBMENU;u.menu_selected=4;u.selected=(uint8_t)selections[i];
        direct_ui_key(&u,2,100,&d);assert(u.page==pages[i]);
    }
    direct_ui_key(&u,DIRECT_KEY_BACK,100,&d);assert(u.page==UI_SUBMENU && u.selected==6);
    direct_ui_init(&u);u.page=UI_UPGRADE;u.parent_page=UI_SUBMENU;
    d.ota_busy=true;d.ota.state=OTA_STATE_RECEIVING;
    e=direct_ui_key(&u,2,101,&d);assert(e.action==UI_ACT_NONE&&u.confirm);
    e=direct_ui_key(&u,1,102,&d);assert(e.action==UI_ACT_NONE&&!u.confirm&&u.page==UI_UPGRADE);
    direct_ui_key(&u,2,103,&d);
    e=direct_ui_key(&u,2,104,&d);assert(e.action==UI_ACT_OTA_CANCEL&&!u.confirm);
    d.ota.state=OTA_STATE_INSTALLING;
    e=direct_ui_key(&u,1,105,&d);assert(e.action==UI_ACT_NONE&&u.page==UI_UPGRADE);
    d.ota_busy=false;d.ota.state=OTA_STATE_SUCCEEDED;
    direct_ui_key(&u,1,106,&d);assert(u.page==UI_SUBMENU);
    direct_ui_init(&u);u.page=UI_SUBMENU;u.menu_selected=4;u.selected=1;
    d.clock_valid=true;d.year=2026;d.month=11;d.day=3;d.timezone_minutes=330;
    direct_ui_key(&u,2,107,&d);assert(u.page==UI_TIME&&!u.editing);
    u.selected=6;direct_ui_key(&u,6,108,&d);assert(u.time_fields[6]==345);
    e=direct_ui_key(&u,2,109,&d);assert(e.action==UI_ACT_SET_TIME&&e.value==0);
    u.selected=4;direct_ui_key(&u,6,110,&d);
    e=direct_ui_key(&u,2,111,&d);assert(e.action==UI_ACT_SET_TIME&&e.value==1);
    uint8_t metadata[DIRECT_FW_METADATA_SIZE]={'C','S','I','V',1,0,DIRECT_FW_DEVICE,0};
    memcpy(metadata+8,"CS-MB-PANEL",11);
    assert(direct_ota_image_identity_matches(metadata,sizeof(metadata),DIRECT_FW_DEVICE));
    assert(!direct_ota_image_identity_matches(metadata,sizeof(metadata),DIRECT_FW_FACTORY));
    memset(metadata+8,0,sizeof(metadata)-8);memcpy(metadata+8,"CS-L10-6K2",10);
    assert(!direct_ota_image_identity_matches(metadata,sizeof(metadata),DIRECT_FW_DEVICE));
    memset(metadata,0,sizeof(metadata)); /* Minimal v0.1 has no product metadata. */
    assert(!direct_ota_image_identity_matches(metadata,sizeof(metadata),DIRECT_FW_DEVICE));
    /* Allow three additional independent SOC readbacks within the snapshot budget. */
    assert(sizeof(panel_snapshot_t)-sizeof(((panel_snapshot_t *)0)->setting_reads)<2128);
    direct_ui_init(&u);u.page=UI_HELP;direct_ui_view_t view;
    direct_ui_build(&u,&d,&view);
    assert(!strcmp(view.rows[0],"CS-MB-PANEL"));
    assert(strstr(view.rows[4],"Wi-Fi") && strstr(view.rows[5],"Cloud") && !view.rows[6][0]);
    u.menu_selected=4;
    direct_ui_key(&u,DIRECT_KEY_LEFT,110,&d);assert(u.detail_page==0);
    direct_ui_key(&u,DIRECT_KEY_ENTER_BACK,111,&d);assert(u.page==UI_SUBMENU && u.menu_selected==5);
    e=direct_ui_key(&u,DIRECT_KEY_ENTER,112,&d);
    assert(e.action==FULL_UI_OPEN_MODBUS && e.index==PANEL_PAGE_SETTING_GROUPS);
    direct_ui_key(&u,DIRECT_KEY_BACK,113,&d);assert(u.page==UI_HELP && u.menu_selected==4);
    assert(direct_board_gpio_allowed(8,DIRECT_PIN_LCD_DC));
    assert(!direct_board_gpio_allowed(8,DIRECT_PIN_OTHER));
    assert(direct_board_gpio_allowed(2,DIRECT_PIN_KEY_DATA));
    assert(!direct_board_gpio_allowed(2,DIRECT_PIN_OTHER));
    assert(!direct_board_gpio_allowed(9,DIRECT_PIN_OTHER));
    assert(direct_board_gpio_allowed(9,DIRECT_PIN_LED_DATA));
    assert(!direct_board_gpio_allowed(9,DIRECT_PIN_LCD_DC));
    assert(full_write_result(PANEL_WRITE_CONFIRMED,true)==0);
    assert(full_write_result(PANEL_WRITE_CONFIRMED,false)==FULL_WRITE_UNCONFIRMED);
    assert(full_write_result(PANEL_WRITE_PROCESSED,true)==FULL_WRITE_UNCONFIRMED);
    assert(full_write_result(PANEL_WRITE_PROTOCOL_ACCEPTED,true)==FULL_WRITE_PROTOCOL_ACCEPTED);
    assert(full_write_result(PANEL_WRITE_PROTOCOL_ACCEPTED,false)==FULL_WRITE_UNCONFIRMED);
    assert(full_write_result(PANEL_WRITE_UNCERTAIN,true)==FULL_WRITE_UNCONFIRMED);
    assert(full_write_result(PANEL_WRITE_REJECTED,true)!=FULL_WRITE_UNCONFIRMED);
    u.page=UI_REVIEW;u.pending=true;
    direct_ui_write_result(&u,FULL_WRITE_UNCONFIRMED);
    assert(!u.pending && !strcmp(u.message,"Result unconfirmed"));
    direct_ui_init(&u);u.page=UI_SUBMENU;u.menu_selected=4;u.selected=0;
    direct_ui_key(&u,2,100,&d);assert(u.page==UI_APPEARANCE);
    direct_ui_key(&u,5,101,&d);direct_ui_key(&u,5,102,&d);assert(u.selected==2);
    direct_ui_key_hints(&u,&d,hints);
    assert(!strcmp(hints[1],"-") && !strcmp(hints[4],"+") && !strcmp(hints[5],"Save"));
    e=direct_ui_key(&u,4,103,&d);panel_display_settings_t prefs;
    assert(e.action==UI_ACT_APPEARANCE_PREVIEW && panel_display_unpack(e.index,&prefs) && prefs.brightness==90);
    direct_ui_key(&u,5,104,&d);e=direct_ui_key(&u,6,105,&d);
    assert(panel_display_unpack(e.index,&prefs) && prefs.timeout_index==3);
    direct_ui_key(&u,5,106,&d);assert(u.selected==4);
    e=direct_ui_key(&u,2,107,&d);assert(e.action==UI_ACT_APPEARANCE_SAVE);
    assert(panel_display_unpack(e.index,&prefs) && prefs.brightness==90 && prefs.timeout_index==3);
    assert(direct_ui_key(&u,1,108,&d).action==UI_ACT_APPEARANCE_CANCEL);
    d.valid[15]=true;d.config[15]=0;
    direct_ui_key(&u,2,109,&d);assert(u.page==UI_APPEARANCE && u.time_fields[1]==0);
    bool original_theme=u.appearance_theme;
    e=direct_ui_key(&u,2,110,&d);
    assert(e.action==UI_ACT_APPEARANCE_SAVE && u.selected==0 && u.appearance_theme==original_theme);
    puts("PASS full routes: original menus/phone/theme/clock/OTA, Modbus, cross-product OTA rejection, compact RAM");
}
