#include "full_modbus_map.h"
#include "panel_catalog.h"
#include <assert.h>
#include <stdio.h>
#include <stddef.h>
#include <string.h>

static panel_snapshot_t s;
static void set(unsigned a,uint16_t v,uint32_t stamp)
{panel_store(&s,(uint16_t)a,v,stamp);}
static void setup(void)
{
    memset(&s,0,sizeof(s));s.now_ms=2000;
    s.config=(panel_config_t){1,9600,0,PANEL_PROFILE_6KU};
    const unsigned regs[]={0x42,0x43,0x44,0x46,0x47,0x48,0x49,0x50,0x51,0x52,0x53,
        0x58,0x59,0x5a,0x5b,0x5c,0x5d,0x80,0x81,0x84,0x85,
        0x96,0x97,0x98,0x99,0x9a,0x9b,0xa0,0x154,0x330,0x331};
    for(unsigned i=0;i<sizeof(regs)/sizeof(regs[0]);i++)set(regs[i],0,1900);
    set(0x42,3,1999);set(0x43,2|(1U<<6),1900);set(0x44,(1U<<1)|(1U<<9),1900);
    set(0x50,2305,1700);set(0x51,1000,1900);set(0x52,5001,1900);set(0x53,1000,1900);
    set(0x58,2300,1900);set(0x59,100,1900);set(0x5a,5000,1900);set(0x5b,500,1900);
    set(0x5c,550,1900);set(0x5d,120,1900);set(0x80,512,1900);set(0x81,100,1900);
    set(0x84,675,1900);set(0x85,512,1900);set(0x96,3000,1800);set(0x97,1234,1900);
    set(0x98,600,1900);set(0x99,3100,1900);set(0x9a,100,1900);set(0x9b,400,1900);
    set(0xa0,1000,1900);set(0x154,4000,1900);set(0x330,321,1900);set(0x331,456,1900);
    set(0x150,4,1900);
}
int main(void)
{
    RunParamDef run;uint32_t received=0;panel_command_t command;direct_dsp_status_t status;
    setup();s.config.profile_override=PANEL_PROFILE_UNKNOWN;
    assert(!full_mb_registry_model(&s));
    const uint16_t codes[]={0x10,0x40,0x60,0x50};
    const char *models[]={"6KU","12KU","PWR2KH12H1","3KU"};
    set(0x38,0,s.now_ms);
    for(unsigned i=0;i<sizeof(codes)/sizeof(codes[0]);i++){
        set(0x39,codes[i],s.now_ms);
        assert(!strcmp(full_mb_registry_model(&s),models[i]));
    }
    s.config.profile_override=PANEL_PROFILE_6KU;
    assert(!full_mb_registry_model(&s)); /* Live identity disagrees with saved profile. */
    s.config.profile_override=PANEL_PROFILE_UNKNOWN;
    set(0x39,0x11,s.now_ms);assert(!strcmp(full_mb_registry_model(&s),"6KU"));
    s.config.profile_override=PANEL_PROFILE_6KU;
    assert(!strcmp(full_mb_registry_model(&s),"6KU"));
    s.config.profile_override=PANEL_PROFILE_6KU_PARALLEL;
    assert(!strcmp(full_mb_registry_model(&s),"6KU"));
    s.config.profile_override=PANEL_PROFILE_UNKNOWN;
    set(0x39,0x70,s.now_ms);assert(!full_mb_registry_model(&s));
    set(0x39,0x10,0);s.now_ms=PANEL_FRESH_MS+1;set(0x42,3,s.now_ms);
    assert(!full_mb_registry_model(&s));
    setup();memset(&run,0xa5,sizeof(run));
    assert(full_mb_measurement(&s,&run,&received));
    assert(received==1700 && run.GridVolt==230 && run.GridFreq==500);
    assert(run.Vpv1==300 && run.Vpv2==310 && run.BatterySOC==67);
    assert(run.BatDischgCurr==100 && run.BatChgCurr==0 && run.BatDisChrWatt==512 && run.BatChrWatt==0);
    assert(run.InvertTemp==456 && run.BoostTemp==32 && run.BusVolt==4000);
    assert((run.SysStatus&8) && (run.SysStatus&0x400) && (run.SysStatus&0x100));
    set(0x81,(uint16_t)-123,1900);set(0x85,(uint16_t)-630,1900);
    assert(full_mb_measurement(&s,&run,&received));
    assert(run.BatChgCurr==123 && run.BatDischgCurr==0 && run.BatChrWatt==630);
    s.now_ms=7001;set(0x42,3,7001);RunParamDef saved=run;
    assert(!full_mb_measurement(&s,&run,&received) && !memcmp(&run,&saved,sizeof(run)));
    setup();panel_invalidate(&s,0x330);assert(!full_mb_measurement(&s,&run,&received));
    assert(panel_online(&s)); /* Link is alive even though the full sample is incomplete. */
    uint16_t problem;
    assert(full_mb_measurement_check(&s,&problem)==FULL_MB_DATA_MISSING && problem==0x330);
    bool reported[RUN_PARAM_WORDS];
    assert(full_mb_partial_telemetry(&s,&run,reported));
    assert(reported[offsetof(RunParamDef,ACInWatt)/2] && run.ACInWatt==1000);
    assert(reported[offsetof(RunParamDef,OutputWatt)/2] && run.OutputWatt==500);
    assert(reported[offsetof(RunParamDef,InvertTemp)/2] && run.InvertTemp==456);
    assert(!reported[offsetof(RunParamDef,BoostTemp)/2]);
    assert(!reported[offsetof(RunParamDef,ACChrWatt)/2]);
    s.now_ms+=FULL_MB_MEASUREMENT_FRESH_MS+1;
    assert(!full_mb_partial_telemetry(&s,&run,reported));
    s.now_ms=2000;

    panel_home_readings_t home;
    full_mb_home(&s,NULL,&home);
    assert(home.meter[PANEL_HOME_GRID].watts==1000);
    assert(home.meter[PANEL_HOME_SOLAR].watts==1000);
    assert(home.meter[PANEL_HOME_LOAD].watts==500);
    assert(home.meter[PANEL_HOME_GRID].decivolts==2305);
    assert(home.battery_soc==67 && home.battery_direction==1);
    for(unsigned i=0;i<PANEL_HOME_COUNT;i++)
        assert((home.meter[i].fresh&(PANEL_HOME_POWER|PANEL_HOME_VOLTAGE))==(PANEL_HOME_POWER|PANEL_HOME_VOLTAGE));
    s.now_ms=3900;set(0x58,2301,3900);set(0x5b,0,3900);panel_invalidate(&s,0x96);
    full_mb_home(&s,NULL,&home);
    assert(home.meter[PANEL_HOME_GRID].available==(PANEL_HOME_POWER|PANEL_HOME_VOLTAGE) && !home.meter[PANEL_HOME_GRID].fresh);
    assert(home.meter[PANEL_HOME_SOLAR].available==PANEL_HOME_POWER && !home.meter[PANEL_HOME_SOLAR].fresh);
    assert(home.meter[PANEL_HOME_LOAD].fresh==(PANEL_HOME_POWER|PANEL_HOME_VOLTAGE) && !home.meter[PANEL_HOME_LOAD].watts);
    s.now_ms=20;set(0x50,2305,UINT32_MAX-100U);set(0x53,20,5);
    uint32_t paused_at=UINT32_MAX-50U;
    full_mb_home(&s,&paused_at,&home);
    assert(home.meter[PANEL_HOME_GRID].fresh==PANEL_HOME_POWER);
    s.scanning=true;full_mb_home(&s,NULL,&home);
    assert(!home.meter[PANEL_HOME_GRID].fresh && home.meter[PANEL_HOME_GRID].available);
    setup();s.config.profile_override=PANEL_PROFILE_3KU;
    set(0x5e,200,1900);set(0x53,650,1900);
    set(0x81,0,1900);set(0x85,(uint16_t)-3,1900);
    full_mb_home(&s,NULL,&home);
    assert(home.meter[PANEL_HOME_GRID].watts==-450 && home.meter[PANEL_HOME_SOLAR].watts==1000);
    assert(home.meter[PANEL_HOME_BATTERY].watts==3 && home.battery_direction==-1);
    set(0x53,(uint16_t)-135,1950);set(0x5e,135,1950);
    full_mb_home(&s,NULL,&home);
    assert(home.meter[PANEL_HOME_GRID].watts==135 && (home.meter[PANEL_HOME_GRID].fresh&PANEL_HOME_POWER));
    assert(home.meter[PANEL_HOME_LOAD].watts==500 && (home.meter[PANEL_HOME_LOAD].fresh&PANEL_HOME_POWER));
    assert(full_mb_measurement(&s,&run,&received) && run.ACInWatt==135 && run.OutputWatt==500);
    set(0x53,450,1950);set(0x5e,(uint16_t)-450,1950);
    full_mb_home(&s,NULL,&home);
    assert(home.meter[PANEL_HOME_GRID].watts==-450 && (home.meter[PANEL_HOME_GRID].fresh&PANEL_HOME_POWER));
    assert(full_mb_measurement(&s,&run,&received) && run.ACInWatt==-450);
    set(0x5e,135,1950);set(0x53,0xffff,1950);
    full_mb_home(&s,NULL,&home);
    assert(home.meter[PANEL_HOME_GRID].available==PANEL_HOME_VOLTAGE);
    assert(home.meter[PANEL_HOME_GRID].fresh==PANEL_HOME_VOLTAGE);
    set(0x5e,0xffff,1950);set(0x53,135,1950);
    full_mb_home(&s,NULL,&home);
    assert(home.meter[PANEL_HOME_GRID].available==PANEL_HOME_VOLTAGE);
    set(0x5e,135,1950);set(0x53,0,1950);
    full_mb_home(&s,NULL,&home);
    assert(home.meter[PANEL_HOME_GRID].watts==135 &&
           (home.meter[PANEL_HOME_GRID].fresh&PANEL_HOME_POWER));
    set(0x81,0,1950);set(0x84,0x7fff,1950);set(0x80,0xffff,1950);
    full_mb_home(&s,NULL,&home);
    assert(home.meter[PANEL_HOME_BATTERY].available==PANEL_HOME_POWER && !home.battery_direction);
    panel_config_t replacement=s.config;replacement.address=2;
    panel_device_context_reset(&s,&replacement);full_mb_home(&s,NULL,&home);
    for(unsigned i=0;i<PANEL_HOME_COUNT;i++)assert(!home.meter[i].available);
    setup();s.now_ms=3901;
    assert(full_mb_measurement_check(&s,&problem)==FULL_MB_DATA_STALE && problem==0x43);
    setup();s.now_ms=8000;
    assert(full_mb_measurement_check(&s,&problem)==FULL_MB_DATA_STALE && problem==0x42);
    setup();set(0x84,1001,1900);assert(!full_mb_measurement(&s,&run,&received));
    setup();set(0x50,0xffff,1900);assert(!full_mb_measurement(&s,&run,&received));
    assert(full_mb_measurement_check(&s,&problem)==FULL_MB_DATA_INVALID && problem==0x50);
    setup();set(0x81,0x8000,1900);set(0x85,0x8000,1900);
    assert(full_mb_measurement(&s,&run,&received) && run.BatChgCurr==32768 && run.BatChrWatt==32768);
    setup();set(0x81,0,1900);set(0x85,0,1900);set(0xa0,0,1900);
    assert(full_mb_measurement(&s,&run,&received) && run.BatChgCurr==0 && run.BatDischgCurr==0 && run.Ppv==0);
    setup();set(0x51,(uint16_t)-1000,1900);set(0x53,(uint16_t)-1000,1900);
    const uint16_t grid_import_paths[]={1U<<0,1U<<1};
    for(unsigned i=0;i<sizeof(grid_import_paths)/sizeof(grid_import_paths[0]);i++){
        set(0x44,grid_import_paths[i],1900);
        full_mb_home(&s,NULL,&home);
        assert(home.meter[PANEL_HOME_GRID].watts==1000);
        assert(full_mb_measurement(&s,&run,&received) && run.ACInWatt==1000 && run.ACInVA>0);
    }
    set(0x44,1U<<7,1900);set(0x53,1000,1900);
    full_mb_home(&s,NULL,&home);
    assert(home.meter[PANEL_HOME_GRID].watts==-1000);
    assert(full_mb_measurement(&s,&run,&received) && run.ACInWatt==-1000 && run.ACInVA<0);
    set(0x44,(1U<<7)|(1U<<1),1900);
    int32_t watts;bool fresh;
    assert(panel_grid_watts(&s,&watts,&fresh) && watts==1000 && fresh);
    set(0x53,0,1900);set(0x44,1U<<7,1900);
    assert(panel_grid_watts(&s,&watts,&fresh) && watts==0 && fresh);
    set(0x53,1000,1900);
    s.now_ms=PANEL_FRESH_MS+1U;set(0x44,1U<<7,0);
    assert(panel_grid_watts(&s,&watts,&fresh) && watts==1000 && fresh);

    setup();set(0x46,1U<<1,1900);set(0x48,(1U<<2)|(1U<<5)|(1U<<6),1900);
    assert(full_mb_status(&s,&status,&received));
    assert(status.fault==(1U<<11)); /* bus high, not legacy battery high */
    assert(status.warning[1]&1); /* legacy battery low */
    assert(status.warning[2]&(1U<<3)); /* battery over charged */
    assert(status.warning[2]&1); /* BMS communication lost */
    set(0x46,1U<<0,1900);assert(full_mb_status(&s,&status,&received));
    assert(status.fault==(1UL<<31)); /* unmapped, never mislabeled grid voltage low */
    setup();set(0x49,1U<<12,1900);assert(full_mb_status(&s,&status,&received));
    assert(status.warning[1]&(1U<<15)); /* preserves presence of unmapped meter alarm */
    s.now_ms=7001;set(0x42,3,7001);assert(!full_mb_status(&s,&status,&received));

    setup();uint16_t value=999;
    set(0x142,0,1900);assert(full_mb_read_config(&s,0,false,&value) && value==2);
    set(0x142,1,1900);assert(full_mb_read_config(&s,0,false,&value) && value==4);
    set(0x142,2,1900);assert(full_mb_read_config(&s,0,false,&value) && value==3);
    set(0x142,3,1900);assert(!full_mb_read_config(&s,0,false,&value));
    set(0x143,0,1900);assert(full_mb_read_config(&s,13,false,&value) && value==3);
    assert(full_mb_read_config(&s,13,true,&value) && value==2);
    set(0x2a1,5000,1900);assert(full_mb_read_config(&s,8,false,&value) && value==5000);
    assert(!full_mb_read_config(&s,7,false,&value)); /* no measured voltage as setting */
    set(0x2a0,2400,1900);
    assert(full_mb_read_config(&s,7,true,&value) && value==2400);
    set(0x2a0,2350,1900);assert(!full_mb_read_config(&s,7,true,&value));
    set(0x150,3,1900);assert(!full_mb_read_config(&s,4,false,&value));
    assert(full_mb_read_config(&s,23,false,&value) && value==0);

    setup();assert(full_mb_make_write(&s,0,2,false,&command) && command.address==0x4102 && command.value==0);
    assert(!full_mb_make_write(&s,0,1,false,&command));
    assert(full_mb_make_write(&s,1,60,false,&command) && command.address==0x4106 && command.value==6);
    assert(full_mb_make_write(&s,1,600,true,&command) && command.value==6);
    assert(!full_mb_make_write(&s,1,61,false,&command));
    assert(full_mb_make_write(&s,7,2300,false,&command) && command.value==2);
    assert(full_mb_make_write(&s,8,6000,false,&command) && command.value==1);
    assert(full_mb_make_write(&s,5,1,false,&command) && command.address==0x4128);
    assert(full_mb_make_write(&s,6,0,false,&command) && command.address==0x4127);
    assert(full_mb_make_write(&s,16,1,false,&command) && command.address==0x4132);
    assert(full_mb_make_write(&s,17,1,false,&command) && command.address==0x4129);
    assert(full_mb_make_write(&s,28,60,false,&command) && command.address==0x4114);
    assert(full_mb_make_write(&s,13,3,true,&command) && command.value==1);
    assert(full_mb_make_write(&s,11,480,false,&command) && command.address==0x410c && command.value==480);
    assert(full_mb_make_write(&s,36,50,false,&command) && command.address==0x4122);
    assert(!full_mb_make_write(&s,48,5,false,&command)); /* BBJ != documented protocol enum */
    assert(!full_mb_make_write(&s,4,4,false,&command)); /* managed lithium != chemistry */
    assert(full_mb_config_writable(&s,1));assert(!full_mb_config_writable(&s,48));
    s.config.profile_override=PANEL_PROFILE_UNKNOWN;
    assert(!full_mb_make_write(&s,7,2300,false,&command));
    assert(full_mb_make_write(&s,8,5000,false,&command));
    s.now_ms=7001;assert(!full_mb_make_write(&s,8,5000,false,&command));
    setup();s.config.profile_override=PANEL_PROFILE_4KL;
    assert(!full_mb_make_write(&s,7,1200,false,&command));
    assert(!full_mb_make_write(&s,7,2300,false,&command));
    set(0x150,1,1900);assert(!full_mb_read_config(&s,4,false,&value));
    assert(!full_mb_make_write(&s,4,2,false,&command));
    setup();
    /* Timer wrap must choose the genuinely oldest consumed sample. */
    for(unsigned reg=0;reg<PANEL_REG_COUNT;reg++) {
        uint16_t raw;
        if(panel_fresh(&s,(uint16_t)reg) && panel_value(&s,(uint16_t)reg,&raw))set(reg,raw,UINT32_MAX-100U);
    }
    s.now_ms=20;set(0x42,3,19);set(0x50,2300,UINT32_MAX-200U);
    assert(full_mb_measurement(&s,&run,&received) && received==UINT32_MAX-200U);
    setup();
    direct_inverter_battery_t bms={0};
    set(0x88,523,1998);set(0x89,(uint16_t)-12,1998);set(0x8a,70,1998);set(0x41,3,1999);
    full_mb_battery(&s,&bms);assert(!bms.online && !bms.valid);
    set(0x41,2,2000);full_mb_battery(&s,&bms);
    assert(bms.online && bms.valid==7 && bms.values[0]==523 && bms.values[2]==70);
    set(0x41,0,2000);full_mb_battery(&s,&bms);assert(!bms.online && bms.values[2]==70);
    set(0x41,1,2000);full_mb_battery(&s,&bms);assert(!bms.online);
    set(0x88,521,2000);set(0x8a,65535,2000);set(0x91,2,2000);full_mb_battery(&s,&bms);
    assert(bms.online && bms.valid==19 && bms.values[0]==521 && bms.values[4]==2);
    s.context_generation++;panel_invalidate(&s,0x41);full_mb_battery(&s,&bms);assert(!bms.online && !bms.valid);
    /* Unavailable BMS words must never become thousands of volts or amps. */
    setup();set(0x41,2,2000);set(0x88,0x7fff,2000);
    full_mb_battery(&s,&bms);assert(!bms.online);
    set(0x88,0xffff,2000);full_mb_battery(&s,&bms);assert(!bms.online);
    set(0x88,523,2000);set(0x89,0x7fff,2000);set(0x8a,0x7fff,2000);set(0x8b,0x7fff,2000);
    full_mb_battery(&s,&bms);
    assert(bms.online && (bms.valid&1) && !(bms.valid&14));
    setup();s.config.profile_override=PANEL_PROFILE_UNKNOWN;s.detected_profile=PANEL_PROFILE_3KU;
    panel_invalidate(&s,0xa0);set(0x5e,200,1650);set(0x53,650,1900);
    set(0x330,32,1900);set(0x331,45,1900);
    assert(full_mb_measurement(&s,&run,&received));
    assert(received==1650 && run.ACInWatt==-450 && run.Ppv==1000);
    assert(run.BoostTemp==32 && run.InvertTemp==450);
    assert(!panel_setting_allowed(&s,0x4123,1) && !panel_setting_allowed(&s,0x4101,0));
    s.config.profile_override=PANEL_PROFILE_3KU;s.detected_profile=PANEL_PROFILE_UNKNOWN;
    assert(full_mb_make_write(&s,8,5000,false,&command) && command.address==0x4101 && command.value==0);
    set(0x41,2,2000);set(0x88,257,2000);set(0x89,(uint16_t)-1230,2000);set(0x8b,24,2000);
    full_mb_battery(&s,&bms);
    assert(bms.online && (bms.valid&0x0b)==0x0b && (int16_t)bms.values[1]==-123 && bms.values[3]==240);
    setup();s.config.profile_override=PANEL_PROFILE_3KU;set(0x5e,0,1900);
    set(0x81,0,1900);set(0x85,3,1900);
    assert(full_mb_measurement(&s,&run,&received));
    assert(run.BatDischgCurr==0 && run.BatDisChrWatt==3 && (run.SysStatus&8));
    set(0x85,(uint16_t)-1,1900);
    assert(full_mb_measurement(&s,&run,&received));
    assert(run.BatChgCurr==0 && run.BatChrWatt==1 && (run.SysStatus&4));
    const panel_profile_t profiles[]={PANEL_PROFILE_6KU,PANEL_PROFILE_6KU_PARALLEL,
        PANEL_PROFILE_12KU,PANEL_PROFILE_2KU,PANEL_PROFILE_3KU};
    const uint16_t identity[]={0x10,0x11,0x40,0x60,0x50};
    for(unsigned i=0;i<sizeof(profiles)/sizeof(profiles[0]);i++){
        setup();s.config.profile_override=profiles[i];s.detected_profile=profiles[i];
        set(0x38,0,1900);set(0x39,identity[i],1900);set(0x42,2,1999);
        const uint16_t switches[]={FULL_MB_POWER_SAVING,FULL_MB_AUTO_FREQUENCY,
            FULL_MB_BATTERY_OPEN_ALARM,FULL_MB_INVERTER_MUTE};
        const uint16_t writes[]={0x412a,0x412b,0x412d,0x4130};
        const uint16_t flags[]={1U<<4,1U<<5,1U<<7,1U<<11};
        for(unsigned sw=0;sw<sizeof(switches)/sizeof(switches[0]);sw++){
            set(0x351,flags[sw],1900);
            for(unsigned read=0;read<sizeof(switches)/sizeof(switches[0]);read++){
                assert(full_mb_read_config(&s,switches[read],true,&value) && value==(sw==read));
            }
            assert(full_mb_make_write(&s,switches[sw],1,true,&command));
            assert(command.address==writes[sw] && command.value==1);
            assert(!full_mb_make_write(&s,switches[sw],2,true,&command));
            assert(!full_mb_make_write(&s,switches[sw],1,false,&command));
        }
        set(0x351,0xffff,1900);
        assert(!full_mb_read_config(&s,FULL_MB_INVERTER_MUTE,true,&value));
        assert(full_mb_make_write(&s,FULL_MB_AC_POWER_INDEX,1,true,&command));
        assert(command.address==0x4123 && command.value==1);
        assert(!full_mb_make_write(&s,FULL_MB_AC_POWER_INDEX,2,true,&command));
        assert(!full_mb_make_write(&s,FULL_MB_AC_POWER_INDEX,1,false,&command));
        set(0x42,3,1999);
        assert(full_mb_make_write(&s,FULL_MB_AC_POWER_INDEX,2,true,&command));
        assert(command.address==0x4123 && command.value==2);
        set(0x150,2,1900);
        assert(full_mb_make_write(&s,40,25,true,&command) && command.address==0x4120 && command.value==25);
        assert(full_mb_make_write(&s,37,95,true,&command) && command.address==0x4121 && command.value==95);
        assert(full_mb_make_write(&s,36,40,true,&command) && command.address==0x4122 && command.value==40);
        assert(!full_mb_make_write(&s,40,4,true,&command));
        assert(full_mb_make_write(&s,40,0,true,&command) && command.value==0xfff1);
        set(0x350,7,1900);set(0x19e,25,1900);set(0x19f,40,1900);set(0x1a0,95,1900);
        assert(full_mb_read_config(&s,40,true,&value) && value==25);
        assert(full_mb_read_config(&s,36,true,&value) && value==40);
        assert(full_mb_read_config(&s,37,true,&value) && value==95);
        panel_invalidate(&s,0x19e);
        assert(!full_mb_read_config(&s,40,true,&value));
        set(0x350,3,1900);
        assert(full_mb_read_config(&s,40,true,&value) && value==0);
        set(0x350,0xffff,1900);
        assert(!full_mb_read_config(&s,36,true,&value));
        set(0x150,1,1900);
        assert(!full_mb_make_write(&s,36,40,true,&command));
        assert(full_mb_make_write(&s,36,0,true,&command) && command.value==0xfff1);
        if(profiles[i]==PANEL_PROFILE_3KU){
            set(0x17e,270,1900);set(0x17f,1,1900);set(0x180,2,1900);set(0x181,30,1900);
            assert(full_mb_read_config(&s,27,true,&value) && value==270);
            assert(full_mb_read_config(&s,28,true,&value) && value==60);
            assert(full_mb_read_config(&s,29,true,&value) && value==120);
            assert(full_mb_read_config(&s,30,true,&value) && value==30);
            assert(full_mb_make_write(&s,26,1,true,&command) && command.address==0x4112);
            assert(full_mb_make_write(&s,28,120,true,&command) && command.address==0x4114);
            assert(!full_mb_make_write(&s,28,65,true,&command));
            set(0x150,2,1900);
            assert(!full_mb_read_config(&s,28,true,&value));
            assert(!full_mb_make_write(&s,26,1,true,&command));
        }else{
            assert(!full_mb_make_write(&s,26,1,true,&command));
        }
        set(0x42,7,1999);assert(!full_mb_make_write(&s,FULL_MB_AC_POWER_INDEX,2,true,&command));
        set(0x42,3,1999);panel_invalidate(&s,0x39);
        assert(!full_mb_make_write(&s,FULL_MB_AC_POWER_INDEX,2,true,&command));
    }
    set(0x179,6000,1900);
    assert(full_mb_read_config(&s,1,true,&value) && value==600);
    puts("PASS full mapping: units, current direction, oldest timestamp, alarm/config conversion, SOC flags and 3KU equalization guards");
}
