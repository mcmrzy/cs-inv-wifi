#include "panel_ui.h"
#include "panel_catalog.h"
#include "direct_gfx.h"
#include "direct_gfx_assets.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static panel_snapshot_t s;
static unsigned renders;
static void value(unsigned address,uint16_t raw)
{s.valid[panel_slot(address)]=true;s.values[panel_slot(address)]=raw;s.updated_ms[panel_slot(address)]=1000;}
static void check_text(const char *text)
{
    const char *p=text;
    while(*p){uint32_t cp=direct_gfx_utf8_next(&p);if(!direct_gfx_has_glyph(cp))fprintf(stderr,"Missing glyph U+%04X in %s\n",cp,text);assert(direct_gfx_has_glyph(cp));}
    if(direct_gfx_text_width(text,16)>304)fprintf(stderr,"CLIPPED %d: %s\n",direct_gfx_text_width(text,16),text);
    assert(direct_gfx_text_width(text,16)<=304);
}
static void render_one(panel_ui_t ui,const char *name)
{
    renders++;
    panel_view_t v;assert(panel_ui_view(&ui,&s,&v));
    v.theme_light=strstr(name,"light")!=NULL;
    check_text(v.title);check_text(v.subtitle);check_text(v.footer);
    for(unsigned i=0;i<7;i++){
        check_text(v.rows[i]);check_text(v.values[i]);
        if(v.values[i][0]){
            int width=direct_gfx_text_width(v.rows[i],16)+direct_gfx_text_width(v.values[i],16);
            if(width>288)fprintf(stderr,"ROW CLIPPED %s [%s | %s]: %d\n",name,v.rows[i],v.values[i],width);
            assert(width<=288);
        }
    }
    if(ui.language_en){
        const char *fields[]={v.title,v.subtitle,v.footer};
        for(unsigned i=0;i<3;i++)for(const unsigned char *p=(const unsigned char *)fields[i];*p;p++){
            if(*p>=128)fprintf(stderr,"UNTRANSLATED %s: %s\n",name,fields[i]);
            assert(*p<128);
        }
        for(unsigned i=0;i<7;i++){
            for(const unsigned char *p=(const unsigned char *)v.rows[i];*p;p++)assert(*p<128);
            for(const unsigned char *p=(const unsigned char *)v.values[i];*p;p++)assert(*p<128);
        }
    }
    uint16_t *full=calloc(320*240+2,sizeof(uint16_t));
    uint16_t *strips=calloc(320*240+2,sizeof(uint16_t));assert(full && strips);
    full[320*240]=0xcafe;strips[320*240]=0xcafe;
    panel_ui_draw(&v,full,0,240);
    for(int y=0;y<240;y+=16)panel_ui_draw(&v,strips+y*320,y,16);
    assert(!memcmp(full,strips,320*240*sizeof(uint16_t)));
    assert(full[320*240]==0xcafe && strips[320*240]==0xcafe);
    char path[128];snprintf(path,sizeof(path),"tmp/%s.ppm",name);
    FILE *f=fopen(path,"wb");assert(f);fprintf(f,"P6\n320 240\n255\n");
    for(unsigned i=0;i<320*240;i++){
        uint16_t p=full[i];unsigned char rgb[]={((p>>11)&31)*255/31,((p>>5)&63)*255/63,(p&31)*255/31};
        fwrite(rgb,1,3,f);
    }
    fclose(f);free(full);free(strips);
}
static void render(panel_ui_t ui,const char *name)
{
    ui.language_en=false;render_one(ui,name);
    char english[96];snprintf(english,sizeof(english),"%s_en",name);
    ui.language_en=true;render_one(ui,english);
}
int main(void)
{
    s.now_ms=1000;s.config=(panel_config_t){1,9600,0,PANEL_PROFILE_UNKNOWN};
    value(0x42,3);value(0x43,8);value(0x44,8);value(0x50,2301);value(0x53,100);value(0xa0,1500);
    value(0x80,516);value(0x81,(uint16_t)-124);value(0x84,675);value(0x5b,456);
    value(0x38,0x10);value(0x39,0);value(0x46,0);value(0x47,0);value(0x48,0);value(0x49,0);
    value(0x140,1);value(0x150,4);s.detected_profile=PANEL_PROFILE_6KU;
    value(0x142,1);value(0x143,2);value(0x141,0);value(0x351,10);
    value(0x350,256);value(0x9c,123);value(0x9d,4567);
    value(0x58,2300);value(0x59,198);value(0x5a,5000);value(0x5c,470);
    value(0x88,516);value(0x89,(uint16_t)-123);value(0x8a,67);value(0x8b,250);
    value(0x8f,1);value(0x91,0);value(0x92,0);
    value(0x85,(uint16_t)-635);value(0x154,3800);value(0x177,564);value(0x178,540);
    panel_ui_t ui;panel_ui_init(&ui);
    render(ui,"home");
    ui.page=PANEL_PAGE_MENU;render(ui,"menu");
    ui.page=PANEL_PAGE_DETAIL_GROUPS;render(ui,"data_index");
    ui.selected=4;render(ui,"data_index_more");ui.selected=0;
    ui.page=PANEL_PAGE_COMM;render(ui,"communication");
    ui.page=PANEL_PAGE_SETTINGS;ui.setting_group=1;
    ui.selected=(uint16_t)panel_setting_group_index(1,1);
    value(0x2a1,5000);render(ui,"quick_settings");
    ui.editing=true;ui.edit_original_valid=true;ui.draft=1;render(ui,"quick_edit");
    ui.editing=false;ui.edit_original_valid=false;ui.setting_group=0;
    ui.page=PANEL_PAGE_COMM;
    ui.selected=4;ui.review=true;render(ui,"search_review");
    ui.review=false;ui.selected=0;
    ui.page=PANEL_PAGE_DIAG;render(ui,"diagnostics");
    ui.detail=1;render(ui,"ota_unknown");
    s.ota_valid=7;s.ota_updated_ms[0]=s.ota_updated_ms[1]=s.ota_updated_ms[2]=1000;
    render(ui,"ota_diagnostics");
    s.now_ms=7000;render(ui,"ota_stale");s.now_ms=1000;ui.detail=0;
    ui.page=PANEL_PAGE_SETTINGS;render(ui,"settings");
    ui.setting_group=2;ui.selected=(uint16_t)panel_setting_group_index(2,0);render(ui,"settings_battery");
    ui.selected=0;ui.setting_group=0;ui.draft=0xffff;ui.editing=true;render(ui,"setting_unselected");ui.editing=false;
    ui.page=PANEL_PAGE_SETTING_GROUPS;render(ui,"setting_groups");
    ui.selected=7;render(ui,"setting_groups_more");
    ui.page=PANEL_PAGE_SETTINGS;
    for(unsigned i=0;i<panel_setting_count();i++){
        ui.selected=(uint16_t)i;render(ui,"catalog_check");
    }
    for(unsigned group=1;group<PANEL_SETTING_GROUPS;group++){
        ui.setting_group=(uint8_t)group;ui.selected=(uint16_t)panel_setting_group_index(group,0);
        render(ui,"setting_group_items");
    }
    ui.setting_group=0;
    value(0x2a1,5000);ui.selected=1;ui.editing=true;ui.review=true;ui.draft=1;render(ui,"setting_review");
    ui.editing=false;ui.review=false;ui.page=PANEL_PAGE_POWER;ui.selected=1;
    ui.draft=2;ui.review=true;render(ui,"power_review");
    ui.review=false;ui.page=PANEL_PAGE_DETAILS;
    for(unsigned i=0;i<PANEL_DETAIL_PAGES;i++){
        char name[32];snprintf(name,sizeof(name),"details_%u",i);
        ui.detail=(uint8_t)i;render(ui,name);
    }
    s.now_ms=7000;render(ui,"details_stale");s.now_ms=1000;
    ui.detail=5;value(0x54,2301);value(0x55,321);value(0x56,5001);value(0x57,1250);
    render(ui,"generator");
    ui.page=PANEL_PAGE_SETTINGS;ui.editing=true;ui.review=false;
    for(unsigned i=0;i<panel_setting_count();i++){
        ui.selected=(uint16_t)i;ui.draft=0xffff;render(ui,"unselected_draft");
    }
    ui.editing=false;ui.selected=0;ui.page=PANEL_PAGE_HOME;s.config.profile_override=PANEL_PROFILE_6KU;
    render(ui,"home_light");
    ui.page=PANEL_PAGE_DETAILS;ui.detail=5;render(ui,"generator_light");
    ui.page=PANEL_PAGE_ALARMS;value(0x46,0xffff);value(0x48,0xffff);render(ui,"alarms");
    s.now_ms=7000;render(ui,"stale_alarms");
    value(0x46,2);
    assert(panel_store(&s,0x48,1,s.now_ms));
    render(ui,"mixed_alarms");
    ui.page=PANEL_PAGE_HOME;render(ui,"stale_home");
    s.now_ms=1000;ui.page=PANEL_PAGE_POWER;ui.pending=true;
    ui.pending_type=PANEL_CMD_WRITE;ui.pending_sequence=41;
    s.command_sequence=42;s.write_state=PANEL_WRITE_REJECTED;
    panel_result_record(&s,41,PANEL_WRITE_CONFIRMED,0);
    panel_ui_sync(&ui,&s);render(ui,"command_receipt");
    ui.result_address=0x4101;ui.result_value=1;
    render(ui,"result_confirmed");
    ui.last_result=PANEL_WRITE_UNCERTAIN;render(ui,"result_uncertain");
    ui.last_result=PANEL_WRITE_PROCESSED;render(ui,"result_unconfirmed");
    ui.result_address=0x4100;ui.result_value=2;ui.result_profile=PANEL_PROFILE_6KU;
    ui.last_result=PANEL_WRITE_PROTOCOL_ACCEPTED;render(ui,"result_protocol_accepted");
    ui.result_address=0x4101;ui.result_value=1;
    ui.last_result=PANEL_WRITE_REJECTED;ui.result_exception=3;render(ui,"result_rejected");
    panel_ui_init(&ui);ui.page=PANEL_PAGE_SETTINGS;ui.selected=2;
    s.config.profile_override=PANEL_PROFILE_UNKNOWN;
    panel_command_t command;panel_ui_key(&ui,2,&s,&command);render(ui,"blocked_model");
    panel_ui_key(&ui,2,&s,&command);render(ui,"model_setup");
    panel_ui_init(&ui);ui.page=PANEL_PAGE_ALARMS;
    for(unsigned reg=0x46;reg<=0x49;reg++)value(reg,0xffff);
    for(unsigned page=0;page<13;page++){ui.selected=(uint16_t)page;render(ui,"all_alarm_bits");}
    s.detected_profile=PANEL_PROFILE_3KU;
    for(unsigned page=0;page<13;page++){ui.selected=(uint16_t)page;render(ui,"all_3ku_alarm_bits");}
    s.detected_profile=PANEL_PROFILE_6KU;
    ui.page=PANEL_PAGE_DETAILS;ui.selected=0;
    for(unsigned state=0;state<=10;state++){value(0x42,(uint16_t)state);render(ui,"run_states");}
    value(0x42,3);ui.page=PANEL_PAGE_SETTINGS;
    for(unsigned profile=1;profile<PANEL_PROFILE_COUNT;profile++){
        s.config.profile_override=(panel_profile_t)profile;
        for(unsigned i=0;i<panel_setting_count();i++){
            ui.selected=(uint16_t)i;ui.editing=true;ui.review=true;
            uint16_t next;ui.draft=0xffff;
            if(panel_setting_step(&s,panel_setting(i)->address,0xffff,-1,&next))ui.draft=next;
            render(ui,"model_maximum_draft");
        }
    }
    printf("PASS display: complete glyphs, strip/full equivalence, bounded buffers, %u views including all settings and OTA diagnostics\n",renders);
}
