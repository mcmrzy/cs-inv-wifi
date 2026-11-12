#include "panel_settings_query.h"
#include "panel_catalog.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef struct {unsigned reads;uint16_t regs[16];uint32_t now;bool fallback,flags_error;uint16_t flags;} fake_t;
static uint32_t now(void *p){return ((fake_t *)p)->now;}
static panel_io_result_t read_reg(void *p,uint16_t reg,uint16_t count,uint16_t *value,uint8_t *exception){
    fake_t *f=p;assert(count==1);f->regs[f->reads++]=reg;f->now+=20;
    if(reg==0x4105)*value=20;
    else if(reg==0x4109)*value=0xffff;
    else if(reg==0x177)*value=552;
    else if(reg==0x411f){*exception=2;return PANEL_IO_EXCEPTION;}
    else if(reg==0x411e){*exception=3;return PANEL_IO_EXCEPTION;}
    else if(reg==0x410c)return PANEL_IO_ERROR;
    else if(reg==0x2a0)*value=2; /* Unknown representation: ask R/W address. */
    else if(reg==0x4100)*value=2;
    else if(reg==0x15d)*value=0xffff; /* 3KU's independent unlimited setting. */
    else if(reg==0x350){if(f->flags_error){*exception=2;return PANEL_IO_EXCEPTION;}*value=f->flags;}
    else if(reg==0x19e)*value=20;
    else if(reg==0x4120)*value=0xffff;
    else assert(0);
    return PANEL_IO_OK;
}
int main(void){
    fake_t f={.now=100};panel_settings_query_t q;
    panel_snapshot_t s={.now_ms=100,.context_generation=1,.config={1,9600,0,PANEL_PROFILE_6KU}};
    panel_store(&s,0x42,3,100);
    panel_control_io_t io={.context=&f,.now=now,.read=read_reg};
    uint16_t addresses[]={0x4105,0x4109,0x410a,0x411f,0x410c};
    panel_settings_query_start(&q,&s,addresses,5);
    for(unsigned i=0;i<5;i++)assert(panel_settings_query_tick(&q,&s,&io));
    assert(f.reads==5 && f.regs[0]==0x4105 && f.regs[1]==0x4109 && f.regs[2]==0x177);
    const panel_setting_read_t *r=panel_setting_read_find(&s,0x4105);
    assert(r && r->state==PANEL_SETTING_VALUE && r->value==20 && !r->independent);
    assert(panel_setting_read_find(&s,0x4109)->state==PANEL_SETTING_EMPTY);
    assert(panel_setting_read_find(&s,0x410a)->state==PANEL_SETTING_VALUE);
    assert(panel_setting_read_find(&s,0x411f)->state==PANEL_SETTING_UNSUPPORTED);
    assert(panel_setting_read_find(&s,0x410c)->state==PANEL_SETTING_TIMEOUT);
    assert(!panel_settings_query_tick(&q,&s,&io));
    addresses[0]=0x4100;panel_settings_query_start(&q,&s,addresses,1);
    assert(panel_settings_query_tick(&q,&s,&io) && q.fallback);
    assert(panel_settings_query_tick(&q,&s,&io) && !q.fallback);
    r=panel_setting_read_find(&s,0x4100);assert(r->value==2 && !r->independent);
    assert(f.regs[5]==0x2a0 && f.regs[6]==0x4100);
    panel_invalidate(&s,0x42);assert(!panel_settings_query_tick(&q,&s,&io));
    panel_store(&s,0x42,3,f.now);assert(panel_settings_query_tick(&q,&s,&io));
    assert(f.regs[7]==0x2a0); /* Recovery refreshes immediately, without waiting for the round deadline. */
    s.command_busy=true;assert(!panel_settings_query_tick(&q,&s,&io));s.command_busy=false;
    panel_config_t config=s.config;panel_device_context_reset(&s,&config);
    assert(!panel_setting_read_find(&s,0x4100));
    assert(!panel_settings_query_tick(&q,&s,&io));
    assert(panel_setting_read_find(&s,0x4100)->state==PANEL_SETTING_UNREAD);
    panel_store(&s,0x42,3,f.now);addresses[0]=0x411e;panel_settings_query_start(&q,&s,addresses,1);
    assert(panel_settings_query_tick(&q,&s,&io));
    assert(panel_setting_read_find(&s,0x411e)->state==PANEL_SETTING_INVALID);
    s.config.profile_override=PANEL_PROFILE_3KU;
    addresses[0]=0x4119;panel_settings_query_start(&q,&s,addresses,1);
    assert(panel_settings_query_tick(&q,&s,&io));
    r=panel_setting_read_find(&s,0x4119);
    assert(r && r->independent && r->state==PANEL_SETTING_VALUE && r->value==0xfff2);
    /* A visible SOC percentage is only current when its enable switch agrees. */
    f.reads=0;f.flags=4;addresses[0]=0x4120;panel_settings_query_start(&q,&s,addresses,1);
    assert(panel_settings_query_tick(&q,&s,&io));
    assert(panel_settings_query_tick(&q,&s,&io));
    r=panel_setting_read_find(&s,0x4120);
    assert(f.regs[0]==0x350 && f.regs[1]==0x19e && r->independent && r->value==20);
    f.reads=0;f.flags=0;panel_settings_query_start(&q,&s,addresses,1);
    assert(panel_settings_query_tick(&q,&s,&io) && panel_settings_query_tick(&q,&s,&io));
    assert(panel_setting_read_find(&s,0x4120)->value==0xfff1);
    f.reads=0;f.flags_error=true;panel_settings_query_start(&q,&s,addresses,1);
    assert(panel_settings_query_tick(&q,&s,&io) && panel_settings_query_tick(&q,&s,&io));
    r=panel_setting_read_find(&s,0x4120);
    assert(!r->independent && r->state==PANEL_SETTING_EMPTY);
    puts("PASS setting queries: visible-page reads, independent/RW fallback, FFFF/timeout/exception, refresh and context reset; no writes");
}
