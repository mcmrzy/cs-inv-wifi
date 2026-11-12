#include "full_modbus_map.h"
#include "panel_catalog.h"
#include <assert.h>
#include <stdio.h>
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
    set(0x39,0x11,s.now_ms);assert(!full_mb_registry_model(&s)); /* Unregistered profile. */
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
    assert(full_mb_measurement(&s,&run,&received) && run.ACInWatt==-1000 && run.ACInVA<0);

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
    puts("PASS full mapping: units, current direction, oldest timestamp, semantic alarm/config conversion, guards");
}
