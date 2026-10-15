#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "direct_ui_scene.h"
#include "direct_ui_locale.h"
#include "direct_gfx_assets.h"

#define W 320
#define H 240
#define GUARD 32
static uint16_t full[W*H], strips[W*H], reference[W*H];
static uint16_t guarded[GUARD+W*16+GUARD];
static bool fixture_light,fixture_zh;
static void check_glyphs(const char *source)
{
    char buffer[512];const char *s=direct_ui_translate(source,fixture_zh,buffer,sizeof(buffer));
    while(*s){uint32_t cp=direct_gfx_utf8_next(&s);if(!direct_gfx_has_glyph(cp)){
        fprintf(stderr,"FAIL missing glyph U+%04lX from %s\n",(unsigned long)cp,source);exit(3);
    }}
}

static void render(const direct_ui_state_t *u,const direct_ui_data_t *d)
{
    direct_ui_data_t themed=*d;themed.theme_light=fixture_light;themed.language_zh=fixture_zh;d=&themed;
    direct_ui_view_t v; direct_ui_build(u,d,&v);
    check_glyphs(v.title);check_glyphs(v.footer);
    for(unsigned r=0;r<DIRECT_UI_ROWS;r++)check_glyphs(v.rows[r]);
    direct_gfx_t g; direct_gfx_begin(&g,full,W,H,0,0);
    direct_ui_scene_draw(&g,u,d,&v);
    for(int y=0;y<H;y+=16){
        for(unsigned i=0;i<sizeof(guarded)/sizeof(*guarded);i++)guarded[i]=0xdead;
        direct_gfx_begin(&g,guarded+GUARD,W,16,y,0);
        direct_ui_scene_draw(&g,u,d,&v);
        for(unsigned i=0;i<GUARD;i++){
            assert(guarded[i]==0xdead);
            assert(guarded[GUARD+W*16+i]==0xdead);
        }
        memcpy(strips+y*W,guarded+GUARD,W*16*sizeof(uint16_t));
    }
    assert(!memcmp(full,strips,sizeof(full)));
    unsigned nonzero=0;for(unsigned i=0;i<W*H;i++)if(full[i]!=full[0])nonzero++;
    assert(nonzero>100);
}

static void screenshot(const char *name,const direct_ui_state_t *u,const direct_ui_data_t *d)
{
    render(u,d);char path[160];snprintf(path,sizeof(path),"tmp/ui_graphical/%s_%s_%s.ppm",name,fixture_zh?"zh":"en",fixture_light?"light":"dark");
    FILE *f=fopen(path,"wb");assert(f);fprintf(f,"P6\n%d %d\n255\n",W,H);
    for(unsigned i=0;i<W*H;i++){
        uint16_t p=full[i];unsigned char rgb[3]={
            (unsigned char)(((p>>11)&31)*255/31),
            (unsigned char)(((p>>5)&63)*255/63),
            (unsigned char)((p&31)*255/31)};
        assert(fwrite(rgb,1,3,f)==3);
    }
    assert(!fclose(f));printf("PASS render: %s %s %s\n",name,fixture_zh?"zh":"en",fixture_light?"light":"dark");
}
/* Compare actual button pixels with an independently drawn, explicit label.
 * OTA stops incoming transfer; editing retains the existing Undo wording. */
static void check_footer_label(unsigned button,const char *label)
{
    static uint16_t expected[W*20];direct_gfx_t g;
    direct_gfx_begin(&g,expected,W,20,220,fixture_light?0xf7bf:0x0085);
    int left=(int)(button*W/6),right=(int)((button+1)*W/6);
    int width=direct_gfx_text_width(label,12);
    uint16_t color=button==5?(fixture_light?0x02f3:0x07ff):(fixture_light?0x12ef:0xa6bf);
    direct_gfx_text(&g,left+(right-left-width)/2,221,label,12,color);
    for(int y=221;y<H;y++)for(int x=left+1;x<right;x++)
        assert(full[y*W+x]==expected[(y-220)*W+x]);
}

static direct_ui_data_t online_data(void)
{
    direct_ui_data_t d={0};d.dsp_online=true;d.status_valid=true;d.bms_online=true;d.network_online=true;
    d.version="0.5.0-direct-ui";d.sn="H1ZZX0013900001P";d.clock="10-12 16:42";
    d.clock_valid=true;d.year=2026;d.month=10;d.day=8;d.hour=16;d.minute=42;d.timezone_minutes=480;
    d.run.SysStatus=0x0214;d.run.ACInWatt=1800;d.run.GridVolt=230;d.run.GridFreq=500;
    d.run.Ppv=4200;d.run.Vpv1=368;d.run.Vpv2=372;d.run.OutputWatt=3200;
    d.run.ACOutputVolt=230;d.run.ACOutputFreq=500;d.run.OutputCurr=1391;d.run.LoadPercent=516;
    d.run.BatVolt=524;d.run.BatterySOC=78;d.run.BatChgCurr=235;d.run.BusVolt=3850;d.run.InvertTemp=432;
    d.bms.voltage_10mv=5240;d.bms.current_10ma=-1250;d.bms.soc_percent=78;d.bms.soh_percent=98;
    for(unsigned i=0;i<16;i++)d.bms.cell_mv[i]=(uint16_t)(3260+i*3);
    d.bms.cell_mv[5]=3321;d.bms.cell_mv[13]=3223;
    for(unsigned i=0;i<CONFIG_PARAM_MAX_INDEX;i++){d.valid[i]=true;d.writable[i]=true;d.config[i]=(uint16_t)i;}
    d.config[7]=2300;d.config[8]=5000;d.config[41]=1;
    return d;
}

static void offline_independent(direct_ui_state_t *u,direct_ui_data_t *d)
{
    d->dsp_online=false;d->bms_online=false;d->status_valid=false;
    render(u,d);memcpy(reference,full,sizeof(full));
    memset(&d->run,0,sizeof(d->run));memset(&d->status,0,sizeof(d->status));memset(&d->bms,0,sizeof(d->bms));
    render(u,d);
    if(memcmp(reference,full,sizeof(full))){
        fprintf(stderr,"FAIL offline sentinel: page=%u mode=%u detail=%u\n",u->page,u->main_mode,u->detail_page);
        exit(2);
    }
}

static void fixtures(void)
{
    direct_ui_state_t u;direct_ui_init(&u);direct_ui_data_t d=online_data();
    screenshot("home_online",&u,&d);
    d.dsp_online=false;d.dsp_display_held=true;d.measurement_age_ms=3200;screenshot("home_last_sample",&u,&d);
    memcpy(reference,full,sizeof(full));
    d.measurement_age_ms=3600000;render(&u,&d);
    assert(!memcmp(reference,full,sizeof(full))); /* Age cannot clear cached pixels. */
    u.page=UI_PARAMETERS;d.status_valid=false;d.dsp_status_held=true;
    d.status.mode=9;screenshot("parameters_stale",&u,&d);
    assert(!direct_ui_has_alarm(&d));
    u.page=UI_HOME;d.status_valid=true;d.dsp_status_held=false;
    d.dsp_online=true;d.dsp_display_held=false;d.measurement_age_ms=0;render(&u,&d);
    memcpy(reference,full,26*W*sizeof(uint16_t));
    d.dsp_online=false;d.status_valid=false;d.status.fault=1;render(&u,&d);
    assert(!memcmp(reference,full,26*W*sizeof(uint16_t))); /* header depends on network, not DSP */
    d=online_data();d.network_online=false;screenshot("home_network_offline",&u,&d);
    assert(memcmp(reference,full,26*W*sizeof(uint16_t)));
    d=online_data();
    d.run.BatterySOC=100;screenshot("home_soc100",&u,&d);d.run.BatterySOC=78;
    d.run.SysStatus=0x0808;d.run.Ppv=0;d.run.OutputWatt=-3200;d.run.BatChgCurr=0;d.run.BatDischgCurr=100;
    screenshot("home_generator_discharge",&u,&d);
    d.run.ACInWatt=32767;d.run.GridVolt=65535;d.run.Ppv=65535;d.run.Vpv1=65535;
    d.run.OutputWatt=-32768;d.run.BatterySOC=65535;d.run.BatVolt=65535;
    screenshot("home_maxvalues",&u,&d);
    for(unsigned y=149;y<173;y++)for(unsigned x=0;x<W;x++)
        if(x<12 || x>=308)assert(full[y*W+x]==(fixture_light?0xf7bf:0x0085));
    offline_independent(&u,&d);screenshot("home_offline",&u,&d);
    d=online_data();u.main_mode=1;offline_independent(&u,&d);screenshot("home_details_offline",&u,&d);u.main_mode=0;
    d=online_data();u.page=UI_MENU;u.selected=2;screenshot("menu",&u,&d);
    for(unsigned i=0;i<direct_ui_menu_count();i++){
     char name[32];snprintf(name,sizeof(name),"menu_%u",i);u.selected=(uint8_t)i;screenshot(name,&u,&d);
     u.page=UI_SUBMENU;u.menu_selected=(uint8_t)i;
     for(unsigned n=0;n<direct_ui_submenu_count(i);n++){
       u.selected=(uint8_t)n;u.submenu_selected=(uint8_t)n;
       snprintf(name,sizeof(name),"submenu_%u_%u",i,n);screenshot(name,&u,&d);
     }
     u.page=UI_MENU;
    }
    u.page=UI_CONFIG;u.group=7;u.selected=7;screenshot("config",&u,&d);
    u.editing=true;u.draft=2310;screenshot("editing",&u,&d);
    check_footer_label(0,fixture_zh?"取消":"Undo");
    u.page=UI_REVIEW;screenshot("review",&u,&d);
    snprintf(u.message,sizeof(u.message),"No confirmation / timeout");screenshot("review_timeout",&u,&d);u.message[0]=0;
    u.pending=true;screenshot("pending",&u,&d);u.editing=u.pending=false;
    u.page=UI_PARAMETERS;u.selected=0;u.detail_page=0;screenshot("parameters",&u,&d);
    offline_independent(&u,&d);screenshot("parameters_offline",&u,&d);
    d=online_data();u.detail_page=1;offline_independent(&u,&d);screenshot("parameters2_offline",&u,&d);
    d=online_data();u.detail_page=3;d.run.Ibus=1234;d.run.Ppv1=678;d.run.Ppv2=789;
    d.polls=17;d.errors=2;screenshot("diagnostics_online",&u,&d);
    offline_independent(&u,&d);screenshot("diagnostics_offline",&u,&d);
    d=online_data();u.detail_page=4;d.status.inverter_status[0]=3;d.config[49]=6200;
    screenshot("generator_online",&u,&d);
    offline_independent(&u,&d);screenshot("generator_offline",&u,&d);
    d=online_data();u.page=UI_BMS;u.detail_page=0;screenshot("bms_summary",&u,&d);
    u.detail_page=1;screenshot("bms_cells",&u,&d);
    d.bms_online=false;screenshot("bms_offline",&u,&d);
    d=online_data();u.page=UI_ALARM;u.selected=0;d.status.fault=1;d.status.warning[0]=3;screenshot("alarms",&u,&d);
    d.run.Warning=UINT64_MAX;d.status.fault=UINT32_MAX;d.bms.warning=UINT16_MAX;d.bms.protection=UINT16_MAX;
    offline_independent(&u,&d);screenshot("alarms_offline",&u,&d);
    d=online_data();d.status_valid=false;d.bms.warning=1U<<10;d.bms.protection=0;
    screenshot("alarms_long_bms",&u,&d);
    d=online_data();
    u.page=UI_UPGRADE;d.ota_busy=true;d.ota.state=OTA_STATE_RECEIVING;d.ota.progress=63;d.ota.received=920000;d.ota.total=1464000;strcpy(d.ota.version,"0.6.1");d.ota.source=OTA_SOURCE_LOCAL;
    screenshot("upgrade_receiving",&u,&d);
    check_footer_label(5,fixture_zh?"取消":"Stop");
    d.ota.state=OTA_STATE_DOWNLOADING;d.ota.source=OTA_SOURCE_MQTT;strcpy(d.ota.stage,"downloading");
    strcpy(d.ota.message,"Receiving firmware");screenshot("upgrade_downloading",&u,&d);
    d.ota.target=OTA_TARGET_DSP;d.ota.state=OTA_STATE_INSTALLING;strcpy(d.ota.stage,"transferring");
    strcpy(d.ota.message,"Entering direct target Boot (waiting for HR)");screenshot("upgrade_target_boot",&u,&d);
    strcpy(d.ota.message,"Writing target Boot image");screenshot("upgrade_transferring",&u,&d);
    d.ota.state=OTA_STATE_VERIFYING;strcpy(d.ota.stage,"verifying");strcpy(d.ota.message,"Waiting for target to run the new App");
    screenshot("upgrade_waiting_target",&u,&d);
    d.ota.state=OTA_STATE_FAILED;strcpy(d.ota.stage,"failed");strcpy(d.ota.error_code,"HASH_MISMATCH");
    strcpy(d.ota.message,"Firmware verification failed");screenshot("upgrade_failed",&u,&d);
    strcpy(d.ota.error_code,"HTTP_ERROR");strcpy(d.ota.message,"HTTP client init failed free=1234 max=5678 | https needs TLS heap");
    screenshot("upgrade_http_failed",&u,&d);
    strcpy(d.ota.error_code,"SECURITY_VERSION_READ_FAILED");screenshot("upgrade_security_failed",&u,&d);
    strcpy(d.ota.error_code,"FUTURE_UNKNOWN_ERROR");screenshot("upgrade_unknown_failed",&u,&d);
    d.ota.state=OTA_STATE_SUCCEEDED;d.ota_busy=false;strcpy(d.ota.stage,"succeeded");strcpy(d.ota.error_code,"OK");
    strcpy(d.ota.message,"Target transfer complete; target response not observed");screenshot("upgrade_no_target_response",&u,&d);
    strcpy(d.ota.message,"Target transfer complete; new App responded");screenshot("upgrade_target_responded",&u,&d);
    strcpy(d.ota.version,"very-long-firmware-version-12345");screenshot("upgrade_long_version",&u,&d);
    d.ota.target=OTA_TARGET_ESP;d.ota.state=OTA_STATE_REBOOTING;strcpy(d.ota.stage,"rebooting");
    strcpy(d.ota.version,"0.6.26");screenshot("upgrade_rebooting",&u,&d);
    d.ota_busy=false;u.page=UI_PRODUCTION;d.model="CS-L10-6K2";d.hardware="UNSET";screenshot("production_pending",&u,&d);
    d.factory_ready=true;d.factory_keys=63;d.factory_write_enabled=true;screenshot("production_written",&u,&d);
    u.page=UI_HELP;u.detail_page=0;screenshot("help",&u,&d);
    u.detail_page=1;screenshot("lcd_test_a",&u,&d);
    u.detail_page=2;screenshot("lcd_test_b",&u,&d);
    u.detail_page=0;
    u.page=UI_TIME;u.selected=3;int fields[]={2026,10,8,16,42,0,480};memcpy(u.time_fields,fields,sizeof(fields));screenshot("time",&u,&d);
    u.page=UI_FACTORY;u.group=0;u.confirm=false;screenshot("factory_no",&u,&d);
    u.confirm=true;screenshot("factory_yes",&u,&d);
    u.group=1;screenshot("restart",&u,&d);u.group=2;screenshot("lcd_reset",&u,&d);
    u.page=UI_APPEARANCE;u.selected=0;screenshot("appearance",&u,&d);
}

int main(void)
{
    for(unsigned locale=0;locale<2;locale++)for(unsigned theme=0;theme<2;theme++){
        fixture_zh=locale;fixture_light=theme;
        for(size_t i=0;i<direct_ui_locale_count();i++)check_glyphs(direct_ui_locale_zh(i));
        for(unsigned p=0;p<84;p++)check_glyphs(direct_ui_param_name(p));
        for(unsigned m=0;m<direct_ui_menu_count();m++)check_glyphs(direct_ui_menu_name(m));
        const char*extra[]={"Appearance","Theme","Language","Save","Light","Dark","Chinese","English","System menu","DSP offline","Input status unknown","Generator input valid","Generator input not valid","Alarm status unavailable","Live data unavailable","BMS not responding","MIN 14  MAX 06","L/R Choose","ENTER Confirm","Current value","New value","Review change","L/R step  UP/DN fast","ENTER Review","ENTER Edit","LCD reset","Reset display?"};
        for(unsigned i=0;i<sizeof(extra)/sizeof(*extra);i++)check_glyphs(extra[i]);
        fixtures();
    }
    puts("PASS: theme/language matrix, full-frame/16-line strip equality, guards, glyph coverage, offline independence, power margins, all UI fixtures");
    return 0;
}
