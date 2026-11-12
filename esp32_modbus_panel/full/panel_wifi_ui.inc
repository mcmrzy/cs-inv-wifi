#include "panel_key_labels.h"
/* LCD task owns the editor. The network worker owns scan and connection. */
static uint8_t s_wifi_choice,s_wifi_length;
static char s_wifi_password[65];

static void wifi_edit_reset(void)
{
    memset(s_wifi_password,0,sizeof(s_wifi_password));
    s_wifi_length=0;
    s_ui.draft=0;
    s_ui.editing=false;
}

static void wifi_ui_scan(void)
{
    wifi_edit_reset();s_wifi_choice=0;
    if(!panel_wifi_scan())snprintf(s_ui.message,sizeof(s_ui.message),"Wi-Fi busy");
}

static bool wifi_password_valid(void)
{
    if(s_wifi_length<8)return false;
    if(s_wifi_length<64)return true;
    for(unsigned i=0;i<64;i++){
        char c=s_wifi_password[i];
        if(!((c>='0' && c<='9') || ((c|32)>='a' && (c|32)<='f')))return false;
    }
    return true;
}

static bool keyboard_key(uint8_t key,unsigned limit)
{
    if(key==DIRECT_KEY_BACK){wifi_edit_reset();return false;}
    unsigned i=s_ui.selected;
    const char *chars=panel_wifi_charset(s_ui.draft);
    unsigned count=(unsigned)strlen(chars);
    if(key==DIRECT_KEY_ENTER){
        if(i<count && s_wifi_length<limit){s_wifi_password[s_wifi_length++]=chars[i];s_wifi_password[s_wifi_length]=0;}
        else if(i>=PANEL_WIFI_MODE_FIRST && i<PANEL_WIFI_ACTION_FIRST){s_ui.draft=i-PANEL_WIFI_MODE_FIRST;i=0;}
        else if(i==PANEL_WIFI_ACTION_FIRST && s_wifi_length)s_wifi_password[--s_wifi_length]=0;
        else if(i==PANEL_WIFI_ACTION_FIRST+1)return true;
        else if(i==PANEL_WIFI_ACTION_FIRST+2)wifi_edit_reset();
    }else if(i>=PANEL_WIFI_ACTION_FIRST){
        unsigned action=i-PANEL_WIFI_ACTION_FIRST;
        if(key==DIRECT_KEY_UP){i=(count-1)/PANEL_WIFI_COLUMNS*PANEL_WIFI_COLUMNS+action*4+1;if(i>=count)i=count-1;}
        if(key==DIRECT_KEY_DOWN)i=PANEL_WIFI_MODE_FIRST+s_ui.draft;
        if(key==DIRECT_KEY_LEFT)i=PANEL_WIFI_ACTION_FIRST+(action+2)%3;
        if(key==DIRECT_KEY_RIGHT)i=PANEL_WIFI_ACTION_FIRST+(action+1)%3;
    }else if(i>=PANEL_WIFI_MODE_FIRST){
        unsigned mode=i-PANEL_WIFI_MODE_FIRST;
        if(key==DIRECT_KEY_DOWN)i=mode*PANEL_WIFI_COLUMNS/4;
        if(key==DIRECT_KEY_UP)i=PANEL_WIFI_ACTION_FIRST+mode*3/4;
        if(key==DIRECT_KEY_LEFT)i=PANEL_WIFI_MODE_FIRST+(mode+3)%4;
        if(key==DIRECT_KEY_RIGHT)i=PANEL_WIFI_MODE_FIRST+(mode+1)%4;
    }else if(key==DIRECT_KEY_UP)i=i>=PANEL_WIFI_COLUMNS?i-PANEL_WIFI_COLUMNS:PANEL_WIFI_MODE_FIRST+i*4/PANEL_WIFI_COLUMNS;
    else if(key==DIRECT_KEY_DOWN)i=i+PANEL_WIFI_COLUMNS<count?i+PANEL_WIFI_COLUMNS:PANEL_WIFI_ACTION_FIRST+i%PANEL_WIFI_COLUMNS/4;
    else if(key==DIRECT_KEY_LEFT)i=i%PANEL_WIFI_COLUMNS?i-1:(i+9<count?i+9:count-1);
    else if(key==DIRECT_KEY_RIGHT)i=i%PANEL_WIFI_COLUMNS<9 && i+1<count?i+1:i/PANEL_WIFI_COLUMNS*PANEL_WIFI_COLUMNS;
    s_ui.selected=(uint8_t)i;
    return false;
}

static void keyboard_labels(uint8_t *labels,uint8_t submit)
{
    labels[KEY_BACK]=KEY_CANCEL;
    labels[KEY_LEFT]=KEY_ARROW_LEFT;labels[KEY_RIGHT]=KEY_ARROW_RIGHT;
    labels[KEY_UP]=KEY_UPWARD;labels[KEY_DOWN]=KEY_DOWNWARD;
    unsigned selected=s_ui.selected;
    labels[KEY_ENTER]=selected==PANEL_WIFI_ACTION_FIRST?(s_wifi_length?KEY_DELETE:KEY_NONE):
        selected==PANEL_WIFI_ACTION_FIRST+1?submit:selected==PANEL_WIFI_ACTION_FIRST+2?KEY_CANCEL:
        selected>=PANEL_WIFI_MODE_FIRST?KEY_MODE:KEY_TYPE;
}

static void wifi_ui_key(uint8_t key)
{
    panel_wifi_status_t state=panel_wifi_status();
    s_ui.dirty=true;s_ui.message[0]=0;
    if(key==DIRECT_KEY_BACK){
        if(s_ui.editing)wifi_edit_reset();
        else{s_ui.page=UI_SUBMENU;s_ui.selected=s_ui.submenu_selected;}
        return;
    }
    if(state==PANEL_WIFI_SCANNING || state==PANEL_WIFI_CONNECTING)return;
    const panel_wifi_ap_t *ap=panel_wifi_ap(s_wifi_choice);
    if(s_ui.editing){
        if(keyboard_key(key,64) && ap){
            if(!wifi_password_valid())snprintf(s_ui.message,sizeof(s_ui.message),"8-63 / 64 HEX");
            else if(panel_wifi_connect(ap->ssid,s_wifi_password))wifi_edit_reset();
            else snprintf(s_ui.message,sizeof(s_ui.message),"Wi-Fi busy");
        }
        return;
    }
    if(key==DIRECT_KEY_RIGHT){wifi_ui_scan();return;}
    if(key==DIRECT_KEY_ENTER){
        if(!ap || state==PANEL_WIFI_EMPTY || state==PANEL_WIFI_SCAN_FAILED || state==PANEL_WIFI_CONNECTED){wifi_ui_scan();return;}
        if(ap->secured){s_ui.editing=true;s_ui.draft=0;s_ui.selected=0;}
        else if(!panel_wifi_connect(ap->ssid,""))snprintf(s_ui.message,sizeof(s_ui.message),"Wi-Fi busy");
    }else if(state==PANEL_WIFI_LIST){
        if(key==DIRECT_KEY_UP && s_wifi_choice)s_wifi_choice--;
        if(key==DIRECT_KEY_DOWN && panel_wifi_ap(s_wifi_choice+1))s_wifi_choice++;
    }
}

static void wifi_ui_view(direct_ui_view_t *v,bool zh)
{
    memset(v,0,sizeof(*v));v->selected=-1;
    snprintf(v->title,sizeof(v->title),"%s",zh?"Wi-Fi 配网":"Wi-Fi setup");
    panel_wifi_status_t state=panel_wifi_status();
    v->key_labels[KEY_BACK]=KEY_RETURN;
    if(s_ui.editing){
        keyboard_labels(v->key_labels,KEY_LINK);
        snprintf(v->title,sizeof(v->title),"%s",zh?"Wi-Fi 密码":"Wi-Fi password");
        const panel_wifi_ap_t *ap=panel_wifi_ap(s_wifi_choice);
        if(ap)snprintf(v->rows[0],sizeof(v->rows[0]),"%s",ap->ssid);
        snprintf(v->rows[1],sizeof(v->rows[1]),"%.*s%s",32,s_wifi_password,s_wifi_length<32?"|":"");
        snprintf(v->rows[2],sizeof(v->rows[2]),"%s%s",s_wifi_password+(s_wifi_length<32?s_wifi_length:32),s_wifi_length>=32?"|":"");
        v->selected=(s_ui.draft<<PANEL_WIFI_MODE_SHIFT)|s_ui.selected;
    }else if(state==PANEL_WIFI_LIST){
        const panel_wifi_ap_t *selected=panel_wifi_ap(s_wifi_choice);
        v->key_labels[KEY_RIGHT]=KEY_SCAN;v->key_labels[KEY_ENTER]=!selected?KEY_SCAN:selected->secured?KEY_OPEN:KEY_LINK;
        if(s_wifi_choice)v->key_labels[KEY_UP]=KEY_UPWARD;
        if(panel_wifi_ap(s_wifi_choice+1))v->key_labels[KEY_DOWN]=KEY_DOWNWARD;
        unsigned first=s_wifi_choice/5U*5U;
        for(unsigned i=0;i<5;i++){
            const panel_wifi_ap_t *ap=panel_wifi_ap(first+i);
            if(ap)snprintf(v->rows[i],sizeof(v->rows[i]),"%c %s",ap->secured?'*':' ',ap->ssid);
        }
        v->selected=s_wifi_choice%5U;
    }else{
        if(state!=PANEL_WIFI_SCANNING && state!=PANEL_WIFI_CONNECTING){
            v->key_labels[KEY_RIGHT]=KEY_SCAN;
            v->key_labels[KEY_ENTER]=state>=PANEL_WIFI_FAILED?KEY_OK:KEY_SCAN;
        }
        static const char *const cn[]={"按确认开始扫描","正在扫描网络","","未找到可用网络","扫描失败",
            "正在连接，请稍候","已连接并保存","连接失败","未找到该网络","保存失败"};
        static const char *const en[]={"Press ENTER to scan","Scanning networks","","No compatible network","Scan failed",
            "Connecting","Connected and saved","Connection failed","Network not found","Save failed"};
        snprintf(v->rows[0],sizeof(v->rows[0]),"%s",zh?cn[state]:en[state]);
        if(state>=PANEL_WIFI_FAILED)snprintf(v->rows[2],sizeof(v->rows[2]),"%s",zh?"确认重试或右键重扫":"ENTER retry or Right scan");
    }
}
