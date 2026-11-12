#include "direct_ui.h"
#include "direct_key_codec.h"
#include "panel_wifi.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static direct_ui_state_t s_ui;
static panel_wifi_status_t status;
static unsigned scans,connects;
static const char *expected_password="Abcd123!";
static const panel_wifi_ap_t aps[]={{"Home",true},{"Guest",false}};
panel_wifi_status_t panel_wifi_status(void){return status;}
const panel_wifi_ap_t *panel_wifi_ap(unsigned i){return i<2?&aps[i]:NULL;}
bool panel_wifi_scan(void){scans++;status=PANEL_WIFI_SCANNING;return true;}
bool panel_wifi_connect(const char *ssid,const char *pass)
{
    assert((!strcmp(ssid,"Home") && !strcmp(pass,expected_password)) || (!strcmp(ssid,"Guest") && !*pass));
    connects++;status=PANEL_WIFI_CONNECTING;return true;
}
#include "panel_wifi_ui.inc"

static void enter_char(char c)
{
    for(unsigned mode=0;mode<4;mode++){
        const char *p=strchr(panel_wifi_charset(mode),c);
        if(!p)continue;
        if(s_ui.draft!=mode){s_ui.selected=PANEL_WIFI_MODE_FIRST+mode;wifi_ui_key(DIRECT_KEY_ENTER);}
        s_ui.selected=(uint8_t)(p-panel_wifi_charset(mode));wifi_ui_key(DIRECT_KEY_ENTER);return;
    }
    assert(false);
}

int main(void)
{
    s_ui.page=UI_WIFI;s_ui.submenu_selected=5;
    direct_ui_view_t v;
    wifi_ui_scan();wifi_ui_view(&v,true);assert(scans==1 && strstr(v.rows[0],"扫描"));
    wifi_ui_key(DIRECT_KEY_ENTER);assert(!connects);
    status=PANEL_WIFI_LIST;wifi_ui_key(DIRECT_KEY_ENTER);assert(s_ui.editing);
    s_ui.selected=PANEL_WIFI_ACTION_FIRST+1;wifi_ui_key(DIRECT_KEY_ENTER);assert(s_ui.editing && !connects);
    for(const char *p=expected_password;*p;p++)enter_char(*p);
    wifi_ui_view(&v,true);assert(strstr(v.rows[1],expected_password) && !strstr(v.rows[1],"***"));
    assert(!strcmp(s_wifi_password,"Abcd123!") && s_wifi_length==8);
    enter_char('Z');s_ui.selected=PANEL_WIFI_ACTION_FIRST;wifi_ui_key(DIRECT_KEY_ENTER);
    assert(!strcmp(s_wifi_password,expected_password));
    /* Every mode and action is reachable using only direction keys. */
    s_ui.selected=0;wifi_ui_key(DIRECT_KEY_UP);assert(s_ui.selected==PANEL_WIFI_MODE_FIRST);
    wifi_ui_key(DIRECT_KEY_LEFT);assert(s_ui.selected==PANEL_WIFI_MODE_FIRST+3);
    wifi_ui_key(DIRECT_KEY_ENTER);assert(s_ui.draft==3&&s_ui.selected==0);
    for(unsigned i=0;i<4;i++)wifi_ui_key(DIRECT_KEY_DOWN);
    assert(s_ui.selected>=PANEL_WIFI_ACTION_FIRST);
    wifi_ui_key(DIRECT_KEY_DOWN);assert(s_ui.selected==PANEL_WIFI_MODE_FIRST+3);
    s_ui.selected=PANEL_WIFI_ACTION_FIRST+1;wifi_ui_key(DIRECT_KEY_ENTER);
    assert(connects==1 && !s_ui.editing && !s_wifi_length && !s_wifi_password[0]);
    /* Both visible lines retain all 64 characters without hidden truncation. */
    status=PANEL_WIFI_LIST;wifi_ui_key(DIRECT_KEY_ENTER);
    for(unsigned i=0;i<64;i++)enter_char('a');
    enter_char('b');
    assert(s_wifi_length==64 && strlen(s_wifi_password)==64 && wifi_password_valid());
    wifi_ui_view(&v,true);assert(strlen(v.rows[1])==32 && strlen(v.rows[2])==33 && v.rows[2][32]=='|');
    s_wifi_password[63]='G';assert(!wifi_password_valid());
    wifi_ui_key(DIRECT_KEY_BACK);assert(!s_ui.editing&&!s_wifi_password[0]);
    status=PANEL_WIFI_SAVE_FAILED;wifi_ui_view(&v,true);assert(strstr(v.rows[0],"保存失败"));
    wifi_ui_key(DIRECT_KEY_RIGHT);assert(scans==2);
    status=PANEL_WIFI_LIST;wifi_ui_key(DIRECT_KEY_DOWN);wifi_ui_key(DIRECT_KEY_ENTER);
    assert(connects==2 && !s_ui.editing);
    status=PANEL_WIFI_CONNECTED;wifi_ui_view(&v,true);assert(strstr(v.rows[0],"已连接并保存"));
    wifi_ui_key(DIRECT_KEY_BACK);assert(s_ui.page==UI_SUBMENU && s_ui.selected==5);
    puts("PASS panel Wi-Fi: scan, visible case-sensitive password, four keyboard modes, navigation, 64-char display, validation, open network and save status");
}
