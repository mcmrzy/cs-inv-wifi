#include "panel_key_labels.h"
/* LCD-owned catalog navigation; downloading and installation use the existing OTA owner. */
static bool s_catalog_open;
static panel_firmware_catalog_snapshot_t s_catalog;

static bool firmware_ui_tick(void)
{
    if(s_ui.page!=UI_UPGRADE || s_ui_data.ota_busy){s_catalog_open=false;return false;}
    panel_firmware_catalog_snapshot(&s_catalog);
    if(!s_catalog_open || s_catalog.state==PANEL_CATALOG_IDLE){
        s_catalog_open=true;
        panel_firmware_catalog_refresh();
        panel_firmware_catalog_snapshot(&s_catalog);
    }
    return true;
}

static void firmware_ui_key(uint8_t key)
{
    s_ui.dirty=true;s_ui.message[0]=0;
    panel_firmware_catalog_snapshot(&s_catalog);
    if(key==DIRECT_KEY_BACK){s_catalog_open=false;s_ui.page=UI_SUBMENU;s_ui.selected=s_ui.submenu_selected;return;}
    if(key==DIRECT_KEY_RIGHT){panel_firmware_catalog_refresh();return;}
    if(s_catalog.state==PANEL_CATALOG_READY){
        if(key==DIRECT_KEY_UP && s_catalog.index)panel_firmware_catalog_select(s_catalog.index-1);
        else if(key==DIRECT_KEY_DOWN && s_catalog.index+1<s_catalog.total)panel_firmware_catalog_select(s_catalog.index+1);
        else if(key==DIRECT_KEY_ENTER && !s_catalog.current_version){
            OtaResult result=panel_firmware_catalog_start();
            if(result!=OTA_RESULT_OK && result!=OTA_RESULT_DUPLICATE)
                snprintf(s_ui.message,sizeof(s_ui.message),"%s",ota_result_code(result));
        }
    }else if(key==DIRECT_KEY_ENTER && s_catalog.state!=PANEL_CATALOG_LOADING)panel_firmware_catalog_refresh();
}

static void firmware_ui_description(direct_ui_view_t *v,const char *text)
{
    unsigned line=4,used=0,width=0;
    while(*text && line<6){
        unsigned char c=(unsigned char)*text;
        unsigned bytes=c<128?1:(c&0xe0)==0xc0?2:(c&0xf0)==0xe0?3:4;
        unsigned advance=c<128?8:16;
        if(c=='\n' || used+bytes>=sizeof(v->rows[0]) || width+advance>280){
            line++;used=width=0;if(c=='\n'){text++;continue;}if(line>=6)break;
        }
        for(unsigned n=0;n<bytes;n++){if(!text[n])return;v->rows[line][used++]=text[n];}
        v->rows[line][used]=0;width+=advance;text+=bytes;
    }
}

static void firmware_ui_view(direct_ui_view_t *v,bool zh)
{
    memset(v,0,sizeof(*v));v->selected=-1;
    v->key_labels[KEY_BACK]=KEY_RETURN;v->key_labels[KEY_RIGHT]=KEY_REFRESH;
    snprintf(v->title,sizeof(v->title),"%s",zh?"固件升级":"Firmware update");
    snprintf(v->rows[0],sizeof(v->rows[0]),"%s %s",zh?"当前":"Current",s_ui_data.version?s_ui_data.version:"--");
    if(s_catalog.state==PANEL_CATALOG_READY){
        if(s_catalog.index)v->key_labels[KEY_UP]=KEY_PREV;
        if(s_catalog.index+1<s_catalog.total)v->key_labels[KEY_DOWN]=KEY_NEXT;
        if(!s_catalog.current_version)v->key_labels[KEY_ENTER]=KEY_RUN;
        snprintf(v->rows[1],sizeof(v->rows[1]),"%s %u / %u",zh?"可用固件":"Available",s_catalog.index+1,s_catalog.total);
        snprintf(v->rows[2],sizeof(v->rows[2]),"%s",s_catalog.version);
        snprintf(v->rows[3],sizeof(v->rows[3]),"ESP  %lu KB",(unsigned long)((s_catalog.file_size+1023)/1024));
        firmware_ui_description(v,s_catalog.changelog);
        if(s_catalog.current_version)snprintf(v->rows[6],sizeof(v->rows[6]),"%s",zh?"已是当前版本":"Already installed");
    }else{
        if(s_catalog.state!=PANEL_CATALOG_LOADING)v->key_labels[KEY_ENTER]=KEY_REFRESH;
        static const char *const cn[]={"按确认获取列表","正在获取固件列表","","暂无可用固件","请先连接云端","请求超时，请重试","获取失败，请重试"};
        static const char *const en[]={"Press OK to load","Loading firmware list","","No available firmware","Connect to cloud first","Request timed out","Could not load list"};
        snprintf(v->rows[2],sizeof(v->rows[2]),"%s",zh?cn[s_catalog.state]:en[s_catalog.state]);
    }
    if(s_ui_data.ota.state==OTA_STATE_FAILED || s_ui_data.ota.state==OTA_STATE_ROLLED_BACK)
        snprintf(v->rows[6],sizeof(v->rows[6]),"%s %s",zh?"上次升级":"Last update",s_ui_data.ota.error_code);
    if(s_ui.message[0])snprintf(v->rows[6],sizeof(v->rows[6]),"%s",s_ui.message);
}
