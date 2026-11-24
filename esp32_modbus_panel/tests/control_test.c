#include "panel_control.h"
#include "panel_catalog.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint32_t now, write_time[3];
    unsigned writes, reads, published;
    panel_io_result_t result;
    bool marker, match, override_actual,read_error,marker_error,marker_exception;
    bool override_flags,actual_exception;
    uint16_t written, actual,flags;
} fake_t;
static uint32_t now(void *p) { return ((fake_t *)p)->now; }
static void sleep_ms(void *p,uint32_t n) { ((fake_t *)p)->now += n; }
static panel_io_result_t read_reg(void *p,uint16_t a,uint16_t n,uint16_t *v,uint8_t *e)
{
    fake_t *f=p; (void)n; (void)e; f->reads++;
    if(f->read_error || (a>=0x4100 && f->marker_error))return PANEL_IO_ERROR;
    if(a>=0x4100 && f->marker_exception){*e=2;return PANEL_IO_EXCEPTION;}
    if(a<0x4100 && f->actual_exception){*e=2;return PANEL_IO_EXCEPTION;}
    if(a==0x350 && f->override_flags){*v=f->flags;return PANEL_IO_OK;}
    *v = a>=0x4100 ? (f->marker ? 0xffff : f->written) :
         (f->override_actual ? f->actual : f->match ? f->written : 2);
    return PANEL_IO_OK;
}
static panel_io_result_t write_reg(void *p,uint16_t a,uint16_t v,uint8_t *e)
{
    fake_t *f=p; (void)a; f->write_time[f->writes++]=f->now; f->written=v;
    if(f->result==PANEL_IO_EXCEPTION) *e=3;
    return f->result;
}
static void publish(void *p,const panel_snapshot_t *s)
{ ((fake_t *)p)->published++; (void)s; }
static void setup(panel_snapshot_t *s,fake_t *f)
{
    memset(s,0,sizeof(*s)); memset(f,0,sizeof(*f));
    f->now=100; f->marker=true; f->match=true;
    s->now_ms=100; s->config.address=1;
    assert(panel_store(s,0x42,3,100));
}
static void incremental(void)
{
    panel_snapshot_t s;fake_t f;panel_control_t c={0};
    panel_control_io_t io={&f,now,sleep_ms,read_reg,write_reg,publish};
    uint16_t converted;
    assert(panel_setting_decode_readback(0x4114,2,&converted) && converted==120);
    assert(panel_setting_decode_readback(0x4119,3,&converted) && converted==180);
    assert(panel_setting_decode_readback(0x4116,48,&converted) && converted==2);
    assert(!panel_setting_decode_readback(0x4116,25,&converted));
    /* Quiet/context cancellation distinguishes an unsent request from a
     * transmitted write, and cannot resume it after the transition. */
    setup(&s,&f);assert(panel_control_begin(&c,&s,0x4103,1,&io));
    assert(panel_control_cancel(&c,&s,&io) && !f.writes && s.write_state==PANEL_WRITE_REJECTED);
    assert(!panel_control_tick(&c,&s,&io) && !f.writes);
    setup(&s,&f);c=(panel_control_t){0};assert(panel_control_begin(&c,&s,0x4103,1,&io));
    panel_control_tick(&c,&s,&io);assert(f.writes==1);
    s.context_generation++;assert(panel_control_tick(&c,&s,&io));
    assert(s.write_state==PANEL_WRITE_UNCERTAIN && f.writes==1 && !f.reads);
    setup(&s,&f);c=(panel_control_t){0};assert(panel_control_begin(&c,&s,0x4103,1,&io));
    s.context_generation++;assert(panel_control_tick(&c,&s,&io));
    assert(s.write_state==PANEL_WRITE_REJECTED && !f.writes);
    c=(panel_control_t){0};
    setup(&s,&f);f.marker=false;f.match=false;
    assert(panel_control_begin(&c,&s,0x4103,1,&io));
    assert(f.writes==0 && f.reads==0 && s.write_state==PANEL_WRITE_QUEUED);
    assert(!panel_control_begin(&c,&s,0x4103,2,&io));
    assert(c.value==1 && s.write_value==1);
    unsigned core_samples=0,alarm_samples=0,finished=0;
    uint32_t started=f.now;
    while(panel_control_active(&c)){
        unsigned before=f.reads+f.writes;uint32_t before_ms=f.now;
        finished+=panel_control_tick(&c,&s,&io);
        assert(f.reads+f.writes-before<=1 && f.now==before_ms);
        /* Same owner loop remains free to sample live data and alarms while
         * the inverter has not yet returned its completion marker. */
        panel_store(&s,0x50,2300,f.now);panel_store(&s,0x42,3,f.now);core_samples++;
        panel_store(&s,0x46,0,f.now);alarm_samples++;
        f.now+=100;
    }
    assert(finished==1 && f.writes==1 && f.reads==1 && f.now-started>=500 && core_samples>=6 && alarm_samples>=6);
    assert(s.write_state==PANEL_WRITE_PROCESSED && c.readback_valid && c.readback_value==2 && f.now-panel_stamp(&s,0x50)<=100);
    unsigned before=f.reads+f.writes;assert(!panel_control_tick(&c,&s,&io));assert(before==f.reads+f.writes);
    /* A marker never substitutes for actual adoption. Sampling continues
     * while an independent readback refuses the target value. */
    setup(&s,&f);c=(panel_control_t){0};f.match=false;finished=0;
    assert(panel_control_begin(&c,&s,0x4103,1,&io));
    while(panel_control_active(&c)){
        finished+=panel_control_tick(&c,&s,&io);
        panel_store(&s,0x50,2300,f.now);panel_store(&s,0x46,0,f.now);f.now+=100;
    }
    assert(finished==1 && f.writes==1 && s.write_state==PANEL_WRITE_PROCESSED);
    /* A delayed write must be re-admitted after polling changes run state. */
    setup(&s,&f);c=(panel_control_t){.have_write=true,.last_write_ms=100};
    assert(panel_control_begin(&c,&s,0x4103,1,&io));
    for(f.now=100;f.now<700;f.now+=100){assert(!panel_control_tick(&c,&s,&io));assert(!f.writes);}
    panel_store(&s,0x42,7,f.now);
    assert(panel_control_tick(&c,&s,&io) && !f.writes && s.write_state==PANEL_WRITE_REJECTED);
    /* Tick deadlines and the gap remain correct when uptime wraps. */
    setup(&s,&f);f.now=UINT32_MAX-49U;panel_store(&s,0x42,3,f.now);
    c=(panel_control_t){.have_write=true,.last_write_ms=f.now};
    assert(panel_control_begin(&c,&s,0x4103,1,&io));
    f.now+=599U;assert(!panel_control_tick(&c,&s,&io) && !f.writes);
    f.now++;assert(!panel_control_tick(&c,&s,&io) && f.writes==1);
    assert(!panel_control_tick(&c,&s,&io) && f.reads==0);
    f.now+=500;
    assert(panel_control_tick(&c,&s,&io) && f.reads==1 && s.write_state==PANEL_WRITE_CONFIRMED);
    /* Unavailable actual data causes one completion-marker read. */
    setup(&s,&f);c=(panel_control_t){0};f.read_error=true;
    assert(panel_control_begin(&c,&s,0x4103,1,&io));
    assert(!panel_control_tick(&c,&s,&io) && f.writes==1);
    f.now+=499;assert(!panel_control_tick(&c,&s,&io) && f.reads==0);
    f.now++;assert(!panel_control_tick(&c,&s,&io) && f.reads==1);
    assert(panel_control_tick(&c,&s,&io) && f.reads==2 && s.write_state==PANEL_WRITE_UNCERTAIN);
    puts("PASS incremental control: one transaction/tick, sampling during marker/adoption waits, single completion/write, delayed admission and uptime wrap");
}

static void power_voltage(void)
{
    enum { OUTPUT_VOLTAGE=0x58 };
    panel_snapshot_t s;fake_t f;panel_control_t c;
    panel_control_io_t io={&f,now,sleep_ms,read_reg,write_reg,publish};
    for(unsigned action=1;action<=2;action++){
        uint16_t initial=action==1?30:2310;
        uint16_t target=action==1?2310:30;
        for(unsigned response=PANEL_IO_OK;response<=PANEL_IO_ERROR;response+=2){
            setup(&s,&f);c=(panel_control_t){0};f.result=(panel_io_result_t)response;
            s.config.profile_override=PANEL_PROFILE_3KU;
            f.now=UINT32_MAX-1000U;
            panel_store(&s,0x42,action==1?2:3,f.now);
            panel_store(&s,OUTPUT_VOLTAGE,initial,f.now);
            assert(panel_control_begin(&c,&s,0x4123,action,&io));
            assert(!panel_control_tick(&c,&s,&io) && f.writes==1);

            f.now+=4100U; /* Slower than the old protocol deadline. */
            assert(!panel_control_tick(&c,&s,&io));
            panel_store(&s,OUTPUT_VOLTAGE,target,f.now);
            assert(!panel_control_tick(&c,&s,&io));
            for(unsigned i=0;i<6;i++){
                f.now+=200U;
                assert(!panel_control_tick(&c,&s,&io)); /* One cached reading stays one sample. */
            }
            panel_store(&s,OUTPUT_VOLTAGE,0xffff,f.now);
            assert(!panel_control_tick(&c,&s,&io));
            f.now+=500U;panel_store(&s,OUTPUT_VOLTAGE,target,f.now);
            assert(!panel_control_tick(&c,&s,&io));
            f.now+=500U;panel_store(&s,OUTPUT_VOLTAGE,target,f.now);
            assert(!panel_control_tick(&c,&s,&io));
            f.now+=500U;panel_store(&s,OUTPUT_VOLTAGE,target,f.now);
            assert(panel_control_tick(&c,&s,&io) && s.write_state==PANEL_WRITE_CONFIRMED);
            assert(f.writes==1 && !f.reads);
        }
    }
    for(unsigned scenario=0;scenario<5;scenario++){
        setup(&s,&f);c=(panel_control_t){0};
        f.override_actual=true;f.actual=5; /* Running status and receipt alone are insufficient. */
        f.now=6000;panel_store(&s,0x42,2,f.now);
        if(scenario!=4)
            panel_store(&s,OUTPUT_VOLTAGE,scenario==1?2300:0,scenario==2?100:f.now);
        assert(panel_control_begin(&c,&s,0x4123,1,&io));
        assert(!panel_control_tick(&c,&s,&io));
        for(unsigned i=1;i<12;i++){
            f.now+=1000;
            if(scenario!=3){
                uint16_t voltage=scenario==0?(i%2?1900:2300):2300;
                panel_store(&s,OUTPUT_VOLTAGE,voltage,f.now);
            }
            assert(!panel_control_tick(&c,&s,&io));
        }
        f.now+=1000;
        assert(panel_control_tick(&c,&s,&io) && s.write_state==PANEL_WRITE_UNCERTAIN);
        assert(f.writes==1 && !f.reads);
    }

    setup(&s,&f);c=(panel_control_t){0};f.result=PANEL_IO_EXCEPTION;
    panel_store(&s,OUTPUT_VOLTAGE,2300,f.now);
    assert(panel_control_begin(&c,&s,0x4123,2,&io));
    assert(panel_control_tick(&c,&s,&io) && s.write_state==PANEL_WRITE_REJECTED && s.exception==3);

    setup(&s,&f);c=(panel_control_t){0};f.result=PANEL_IO_ERROR;
    panel_store(&s,OUTPUT_VOLTAGE,2300,f.now);
    assert(panel_control_begin(&c,&s,0x4123,2,&io));
    assert(!panel_control_tick(&c,&s,&io));
    s.context_generation++;
    assert(panel_control_tick(&c,&s,&io) && s.write_state==PANEL_WRITE_UNCERTAIN && f.writes==1);

    setup(&s,&f);c=(panel_control_t){0};
    panel_control_write(&c,&s,0x4123,0,&io);
    assert(f.writes==1 && f.reads==1 && s.write_state==PANEL_WRITE_PROTOCOL_ACCEPTED);
    setup(&s,&f);c=(panel_control_t){0};
    panel_store(&s,OUTPUT_VOLTAGE,2300,f.now);
    assert(panel_control_begin(&c,&s,0x4123,2,&io));
    c.require_opposite_output=true;
    panel_store(&s,OUTPUT_VOLTAGE,0,f.now);
    assert(panel_control_tick(&c,&s,&io) && s.write_state==PANEL_WRITE_REJECTED);
    assert(f.writes==0);
    puts("PASS power: voltage transition plus three fresh samples, missing ACK, timeout, rejection, context change, no retransmission");
}

int main(void)
{
    static panel_snapshot_t s; fake_t f; panel_control_t c={0};
    panel_control_io_t io={&f,now,sleep_ms,read_reg,write_reg,publish};
    /* Settings use 03 after 10: CV/float read the BAT setpoints, not live
     * battery voltage. An ACK or a completion marker cannot hide a mismatch. */
    const uint16_t voltage_regs[]={0x410a,0x410b};
    for(unsigned i=0;i<2;i++){
        setup(&s,&f);c=(panel_control_t){0};
        s.config.profile_override=PANEL_PROFILE_6KU;panel_store(&s,0x150,4,f.now);
        f.override_actual=true;f.actual=552;
        panel_control_write(&c,&s,voltage_regs[i],552,&io);
        assert(f.writes==1 && f.reads==1 && s.write_state==PANEL_WRITE_CONFIRMED);
        assert(!c.marker_seen && c.actual_seen && c.actual_raw==552);
        assert(panel_stamp(&s,0x177+i)==f.now);
        setup(&s,&f);c=(panel_control_t){0};
        s.config.profile_override=PANEL_PROFILE_6KU;panel_store(&s,0x150,4,f.now);
        f.override_actual=true;f.actual=540;
        s.setting_reads[0]=(panel_setting_read_t){voltage_regs[i],550,s.now_ms,PANEL_SETTING_VALUE,true};
        panel_control_write(&c,&s,voltage_regs[i],552,&io);
        assert(f.writes==1 && s.write_state==PANEL_WRITE_PROCESSED);
        uint16_t actual;assert(panel_value(&s,0x177+i,&actual) && actual==540);
        assert(f.reads==1 && s.setting_reads[0].value==540 && s.setting_reads[0].independent);
    }
    setup(&s,&f);
    panel_control_write(&c,&s,0x4103,1,&io);
    assert(f.writes==1 && s.write_state==PANEL_WRITE_CONFIRMED);
    uint16_t actual;assert(panel_value(&s,0x140,&actual) && actual==1);
    panel_control_write(&c,&s,0x4103,0,&io);
    assert(f.writes==2 && f.write_time[1]-f.write_time[0]>=600);

    setup(&s,&f); c=(panel_control_t){0}; f.result=PANEL_IO_ERROR;
    panel_control_write(&c,&s,0x4103,1,&io);
    assert(f.writes==1 && f.reads==0 && s.write_state==PANEL_WRITE_UNCERTAIN);
    setup(&s,&f); c=(panel_control_t){0}; f.result=PANEL_IO_EXCEPTION;
    panel_control_write(&c,&s,0x4103,1,&io);
    assert(f.writes==1 && f.reads==0 && s.write_state==PANEL_WRITE_REJECTED && s.exception==3);

    setup(&s,&f); c=(panel_control_t){0}; f.marker=false;
    panel_control_write(&c,&s,0x4103,1,&io);
    assert(f.writes==1 && s.write_state==PANEL_WRITE_CONFIRMED && f.reads==1);
    setup(&s,&f);c=(panel_control_t){0};f.marker_error=true;
    panel_control_write(&c,&s,0x4103,1,&io);
    assert(f.writes==1 && s.write_state==PANEL_WRITE_CONFIRMED && c.actual_seen);
    setup(&s,&f);c=(panel_control_t){0};f.marker_exception=true;
    panel_control_write(&c,&s,0x4103,1,&io);
    assert(f.writes==1 && s.write_state==PANEL_WRITE_CONFIRMED && !s.exception && c.actual_seen);
    setup(&s,&f); c=(panel_control_t){0}; f.match=false;
    panel_control_write(&c,&s,0x4103,1,&io);
    assert(f.writes==1 && s.write_state==PANEL_WRITE_PROCESSED); /* marker alone != adopted */

    setup(&s,&f); c=(panel_control_t){0}; s.updated_ms[panel_slot(0x42)]=0; f.now=6000;
    panel_control_write(&c,&s,0x4103,1,&io);
    assert(f.writes==0 && s.write_state==PANEL_WRITE_REJECTED);
    setup(&s,&f); c=(panel_control_t){0};
    panel_control_write(&c,&s,0x4100,2,&io); /* Unknown model */
    assert(f.writes==0 && s.write_state==PANEL_WRITE_REJECTED);
    setup(&s,&f); c=(panel_control_t){0};s.config.profile_override=PANEL_PROFILE_6KU;
    assert(panel_store(&s,0x2a0,2200,f.now));
    f.override_actual=true;f.actual=2300;
    panel_control_write(&c,&s,0x4100,2,&io);
    assert(f.writes==1 && f.reads==1 && s.write_state==PANEL_WRITE_CONFIRMED);
    uint16_t previous;
    assert(panel_value(&s,0x2a0,&previous) && previous==2300);
    setup(&s,&f);c=(panel_control_t){0};s.config.profile_override=PANEL_PROFILE_6KU;
    f.override_actual=true;f.actual=2200;
    panel_control_write(&c,&s,0x4100,2,&io);
    assert(f.writes==1 && s.write_state==PANEL_WRITE_PROCESSED);
    assert(panel_value(&s,0x2a0,&previous) && previous==2200); /* Never replace mismatch with the request. */
    setup(&s,&f);c=(panel_control_t){0};s.config.profile_override=PANEL_PROFILE_6KU;
    panel_control_write(&c,&s,0x4105,20,&io);
    assert(f.writes==1 && f.reads==1 && s.write_state==PANEL_WRITE_PROTOCOL_ACCEPTED);
    setup(&s,&f);c=(panel_control_t){0};s.config.profile_override=PANEL_PROFILE_4KL;
    f.override_actual=true;f.actual=1200;
    panel_control_write(&c,&s,0x4100,2,&io);
    assert(f.writes==0 && s.write_state==PANEL_WRITE_REJECTED);
    setup(&s,&f);c=(panel_control_t){0};s.config.profile_override=PANEL_PROFILE_6KU;
    f.override_actual=true;f.actual=1200;
    panel_control_write(&c,&s,0x4100,2,&io);
    assert(f.writes==1 && s.write_state==PANEL_WRITE_PROTOCOL_ACCEPTED && !c.readback_valid); /* Invalid independent encoding. */
    setup(&s,&f); c=(panel_control_t){0};s.config.profile_override=PANEL_PROFILE_6KU;f.marker=false;
    panel_control_write(&c,&s,0x4100,2,&io);
    assert(f.writes==1 && s.write_state==PANEL_WRITE_UNCERTAIN);
    setup(&s,&f);c=(panel_control_t){0};f.override_actual=true;f.actual=6000;
    panel_control_write(&c,&s,0x4101,1,&io);
    assert(s.write_state==PANEL_WRITE_CONFIRMED && panel_stamp(&s,0x2a1)==f.now);
    setup(&s,&f);c=(panel_control_t){0};f.override_actual=true;f.actual=0xa004;
    panel_control_write(&c,&s,0x4128,1,&io);
    assert(s.write_state==PANEL_WRITE_CONFIRMED); /* unrelated flag bits must not break confirmation */
    setup(&s,&f);c=(panel_control_t){0};f.override_actual=true;f.actual=0xa000;
    panel_control_write(&c,&s,0x4128,1,&io);
    assert(s.write_state==PANEL_WRITE_PROCESSED); /* marker cannot overrule the actual disabled flag */
    setup(&s,&f);c=(panel_control_t){0};s.config.profile_override=PANEL_PROFILE_3KU;
    f.override_actual=true;f.actual=6000;
    panel_control_write(&c,&s,0x4106,6,&io);
    assert(f.writes==1 && f.written==6 && c.actual_reg==0x179 && s.write_state==PANEL_WRITE_CONFIRMED);
    setup(&s,&f);c=(panel_control_t){0};s.config.profile_override=PANEL_PROFILE_3KU;
    panel_store(&s,0x150,2,f.now);f.result=PANEL_IO_ERROR;
    panel_control_write(&c,&s,0x4107,0,&io);
    assert(f.writes==1 && s.write_state==PANEL_WRITE_UNCERTAIN && !panel_fresh(&s,0x150));
    assert(!panel_setting_allowed(&s,0x4108,220));
    incremental();
    power_voltage();
    /* SOC thresholds need both the stored percentage and the enable switch. */
    const panel_profile_t profiles[]={PANEL_PROFILE_2KU,PANEL_PROFILE_3KU,PANEL_PROFILE_6KU,PANEL_PROFILE_6KU_PARALLEL,PANEL_PROFILE_12KU};
    const uint16_t soc_settings[]={0x4120,0x4121,0x4122};
    const uint16_t soc_regs[]={0x19e,0x1a0,0x19f};
    const uint16_t soc_flags[]={4,2,1};
    for(unsigned p=0;p<sizeof(profiles)/sizeof(profiles[0]);p++)for(unsigned i=0;i<3;i++){
        setup(&s,&f);c=(panel_control_t){0};s.config.profile_override=profiles[p];
        panel_store(&s,0x150,2,f.now);f.override_actual=true;f.actual=20;
        f.override_flags=true;f.flags=soc_flags[i];
        panel_control_write(&c,&s,soc_settings[i],20,&io);
        assert(f.writes==1 && c.actual_reg==soc_regs[i] && s.write_state==PANEL_WRITE_CONFIRMED);
        assert(panel_fresh(&s,0x350) && panel_fresh(&s,soc_regs[i]));
        setup(&s,&f);c=(panel_control_t){0};s.config.profile_override=profiles[p];
        panel_store(&s,0x150,2,f.now);f.override_actual=true;f.actual=20;
        f.override_flags=true;f.flags=0;
        panel_control_write(&c,&s,soc_settings[i],20,&io);
        assert(f.writes==1 && s.write_state==PANEL_WRITE_PROCESSED);
        setup(&s,&f);c=(panel_control_t){0};s.config.profile_override=profiles[p];
        f.override_actual=true;f.actual=20;f.override_flags=true;f.flags=0;
        panel_control_write(&c,&s,soc_settings[i],0xfff1,&io);
        assert(f.writes==1 && s.write_state==PANEL_WRITE_CONFIRMED);
    }
    /* A documented completion receipt remains meaningful on older devices
     * that do not implement the independent configuration register. */
    setup(&s,&f);c=(panel_control_t){0};f.actual_exception=true;
    panel_control_write(&c,&s,0x4103,1,&io);
    assert(f.writes==1 && s.write_state==PANEL_WRITE_PROTOCOL_ACCEPTED);
    puts("PASS control: ACK, marker, actual frequency/bit readback, no write retry, 600ms, stale/profile guards");
}
