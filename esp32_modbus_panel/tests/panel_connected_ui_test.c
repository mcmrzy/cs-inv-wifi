#include "direct_ui.h"
#include "direct_key_codec.h"
#include "panel_firmware_catalog.h"
#include "panel_weather.h"
#include "panel_wifi.h"
#include "panel_key_labels.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static direct_ui_state_t s_ui;
static direct_ui_data_t s_ui_data;
static unsigned loads,starts,selections,weather_refreshes;
static panel_firmware_catalog_snapshot_t cloud;
bool panel_firmware_catalog_snapshot(panel_firmware_catalog_snapshot_t *out){*out=cloud;return true;}
bool panel_firmware_catalog_refresh(void){loads++;cloud.state=PANEL_CATALOG_LOADING;return true;}
bool panel_firmware_catalog_select(unsigned i){selections++;cloud.index=i;return true;}
OtaResult panel_firmware_catalog_start(void){starts++;return OTA_RESULT_OK;}
const char *ota_result_code(OtaResult result){(void)result;return "error";}
void panel_weather_refresh(void){weather_refreshes++;}
bool panel_weather_search(const char *s){(void)s;return true;}
bool panel_weather_set_city(int i){return i==-1;}
bool panel_weather_city(unsigned i,char *out,size_t n){(void)i;(void)out;(void)n;return false;}
static char s_wifi_password[65];
static void wifi_edit_reset(void){s_ui.editing=false;s_wifi_password[0]=0;}
static bool keyboard_key(uint8_t k,unsigned limit){(void)limit;return k==DIRECT_KEY_ENTER;}
static void keyboard_labels(uint8_t *keys,uint8_t submit){keys[KEY_ENTER]=submit;}
#include "panel_firmware_ui.inc"
#include "panel_weather_ui.inc"

int main(void)
{
    direct_ui_view_t view;
    s_ui.page=UI_UPGRADE;s_ui.submenu_selected=4;s_ui_data.version="0.4.11-modbus-full";
    assert(firmware_ui_tick()&&loads==1);assert(firmware_ui_tick()&&loads==1);
    cloud.state=PANEL_CATALOG_OFFLINE;firmware_ui_tick();firmware_ui_view(&view,true);
    assert(strstr(view.rows[2],"连接云端"));
    cloud.state=PANEL_CATALOG_TIMEOUT;firmware_ui_tick();firmware_ui_view(&view,false);
    assert(strstr(view.rows[2],"timed out"));firmware_ui_key(DIRECT_KEY_ENTER);assert(loads==2);
    cloud=(panel_firmware_catalog_snapshot_t){.state=PANEL_CATALOG_READY,.total=3,.file_size=1600000};
    strcpy(cloud.version,"0.4.12-modbus-full");strcpy(cloud.changelog,"修复断联恢复提示，改进英文显示，完善天气和升级列表。");
    firmware_ui_tick();firmware_ui_view(&view,true);assert(strstr(view.rows[1],"1 / 3")&&view.rows[4][0]);
    assert(view.key_labels[KEY_ENTER]==KEY_RUN && view.key_labels[KEY_RIGHT]==KEY_REFRESH &&
           view.key_labels[KEY_UP]==KEY_NONE && view.key_labels[KEY_DOWN]==KEY_NEXT);
    firmware_ui_key(DIRECT_KEY_DOWN);assert(selections==1&&cloud.index==1);
    firmware_ui_key(DIRECT_KEY_UP);assert(selections==2&&cloud.index==0);
    cloud.current_version=true;firmware_ui_tick();firmware_ui_key(DIRECT_KEY_ENTER);assert(!starts);
    cloud.current_version=false;firmware_ui_tick();firmware_ui_key(DIRECT_KEY_ENTER);assert(starts==1);
    firmware_ui_key(DIRECT_KEY_BACK);assert(s_ui.page==UI_SUBMENU&&s_ui.selected==4);
    s_ui.page=UI_WEATHER;weather_ui_view(&view,true);assert(strstr(view.rows[2],"等待联网"));
    assert(view.key_labels[KEY_BACK]==KEY_RETURN && view.key_labels[KEY_RIGHT]==KEY_CITY &&
           view.key_labels[KEY_ENTER]==KEY_REFRESH && view.key_labels[KEY_LEFT]==KEY_NONE);
    s_weather.state=PANEL_WEATHER_LOCATION_FAILED;weather_ui_view(&view,false);assert(strstr(view.rows[2],"lookup failed"));
    s_weather.valid=s_weather.location_valid=true;s_weather.days=7;
    strcpy(s_weather.city,"San Francisco");
    for(unsigned i=0;i<7;i++){
        snprintf(s_weather.daily[i].date,sizeof(s_weather.daily[i].date),"2026-11-%02u",6+i);
        s_weather.daily[i].code=i?61:0;s_weather.daily[i].low_10=-15;s_weather.daily[i].high_10=235;
    }
    weather_ui_view(&view,false);assert(strstr(view.rows[0],"San Francisco")&&strstr(view.rows[1],"Clear")&&strstr(view.rows[7],"Rain"));
    assert(strstr(view.rows[1],"-2 / 24℃"));s_weather.stale=true;weather_ui_view(&view,true);assert(strstr(view.rows[0],"上次预报"));
    weather_ui_key(DIRECT_KEY_ENTER);assert(weather_refreshes==1);
    weather_ui_key(DIRECT_KEY_RIGHT);assert(s_ui.detail_page==1&&!s_ui.editing);
    weather_ui_key(DIRECT_KEY_RIGHT);assert(s_ui.editing);
    weather_ui_key(DIRECT_KEY_ENTER);assert(!s_ui.editing&&s_ui.detail_page==1);
    weather_ui_key(DIRECT_KEY_LEFT);assert(s_ui.detail_page==0);
    weather_ui_key(DIRECT_KEY_BACK);assert(s_ui.page==UI_HOME);
    puts("PASS connected panel UI: catalog entry/list/retry/current-version/start, weather 7 days, location failure, history and return");
}
