#include "panel_control.h"
#include "panel_catalog.h"

enum { SETTING_SETTLE_MS=500U, SETTING_CONFIRM_TIMEOUT_MS=3000U, OUTPUT_VOLTAGE_REG=0x58, POWER_TIMEOUT_MS=12000U, POWER_STABLE_MS=1000U,
       POWER_STABLE_SAMPLES=3, OUTPUT_OFF_MAX=50, OUTPUT_ON_MIN=2000, OUTPUT_ON_MAX=2600 };

static void report(panel_snapshot_t *s,panel_write_state_t state,const panel_control_io_t *io)
{
    s->write_state=state;s->now_ms=io->now(io->context);
    if(io->publish)io->publish(io->context,s);
}

bool panel_control_active(const panel_control_t *c)
{return c && c->stage!=PANEL_CONTROL_IDLE;}

static bool finish(panel_control_t *c,panel_snapshot_t *s,panel_write_state_t state,
                   const panel_control_io_t *io)
{
    if(c->context_generation==s->context_generation && !panel_power_changes_output(c->reg,c->value) &&
       state!=PANEL_WRITE_REJECTED){
        for(unsigned i=0;i<PANEL_SETTING_READ_SLOTS;i++){
            panel_setting_read_t *read=&s->setting_reads[i];
            if(read->address!=c->reg)continue;
            *read=(panel_setting_read_t){.address=c->reg,.value=c->readback_value,
                .stamp=io->now(io->context),.independent=c->readback_valid,
                .state=c->readback_valid?PANEL_SETTING_VALUE:
                    state==PANEL_WRITE_PROTOCOL_ACCEPTED?PANEL_SETTING_EMPTY:PANEL_SETTING_TIMEOUT};
            break;
        }
    }
    c->stage=PANEL_CONTROL_IDLE;report(s,state,io);return true;
}

static bool output_matches(uint16_t decivolts,bool on)
{
    return on?decivolts>=OUTPUT_ON_MIN && decivolts<=OUTPUT_ON_MAX:decivolts<=OUTPUT_OFF_MAX;
}

int panel_power_output_state(const panel_snapshot_t *s)
{
    uint16_t voltage;
    if(!panel_fresh(s,OUTPUT_VOLTAGE_REG) || !panel_value(s,OUTPUT_VOLTAGE_REG,&voltage))return -1;
    if(output_matches(voltage,true))return 1;
    return output_matches(voltage,false)?0:-1;
}

static bool power_output_confirmed(panel_control_t *c,const panel_snapshot_t *s)
{
    uint16_t voltage;
    if(!c->output_started_opposite || !panel_fresh(s,OUTPUT_VOLTAGE_REG) ||
       !panel_value(s,OUTPUT_VOLTAGE_REG,&voltage))return false;
    uint32_t stamp=panel_stamp(s,OUTPUT_VOLTAGE_REG);
    if((int32_t)(stamp-c->output_last_ms)<=0)return false;
    if(stamp-c->output_last_ms>PANEL_FRESH_MS)c->output_samples=0;
    c->output_last_ms=stamp;
    if(!output_matches(voltage,c->value==1)){c->output_samples=0;return false;}
    if(!c->output_samples)c->output_first_ms=stamp;
    if(c->output_samples<POWER_STABLE_SAMPLES)c->output_samples++;
    return c->output_samples>=POWER_STABLE_SAMPLES && stamp-c->output_first_ms>=POWER_STABLE_MS;
}

bool panel_control_cancel(panel_control_t *c,panel_snapshot_t *s,const panel_control_io_t *io)
{
    if(!panel_control_active(c) || !s || !io || !io->now)return false;
    return finish(c,s,c->stage==PANEL_CONTROL_WRITE?PANEL_WRITE_REJECTED:
        PANEL_WRITE_UNCERTAIN,io);
}

bool panel_control_begin(panel_control_t *c,panel_snapshot_t *s,uint16_t reg,
                         uint16_t value,const panel_control_io_t *io)
{
    if(!c || !s || !io || !io->now || !io->read || !io->write || panel_control_active(c))return false;
    c->require_opposite_output=false;
    c->marker_seen=false;c->actual_seen=false;c->readback_valid=false;c->readback_value=0;
    c->marker_raw=0;c->actual_raw=0;c->actual_reg=0xffff;
    s->now_ms=io->now(io->context);s->write_address=reg;s->write_value=value;s->exception=0;
    if(s->config.address<1 || s->config.address>247 || s->scanning ||
       !panel_setting_allowed(s,reg,value)){
        report(s,PANEL_WRITE_REJECTED,io);return false;
    }
    c->reg=reg;c->value=value;c->stage=PANEL_CONTROL_WRITE;
    c->context_generation=s->context_generation;
    uint32_t since=s->now_ms-c->last_write_ms;
    c->next_io_ms=s->now_ms+(c->have_write && since<PANEL_WRITE_GAP_MS?PANEL_WRITE_GAP_MS-since:0U);
    c->actual_reg=panel_setting_current_address(s,panel_setting_find(reg));
    report(s,PANEL_WRITE_QUEUED,io);return true;
}

bool panel_control_tick(panel_control_t *c,panel_snapshot_t *s,const panel_control_io_t *io)
{
    if(!panel_control_active(c) || !s || !io || !io->now || !io->read || !io->write)return false;
    s->now_ms=io->now(io->context);
    if(c->context_generation!=s->context_generation)return panel_control_cancel(c,s,io);
    if(c->stage==PANEL_CONTROL_POWER_OUTPUT){
        if(s->now_ms-c->started_ms>=POWER_TIMEOUT_MS)return finish(c,s,PANEL_WRITE_UNCERTAIN,io);
        return power_output_confirmed(c,s) && finish(c,s,PANEL_WRITE_CONFIRMED,io);
    }
    if(c->stage!=PANEL_CONTROL_WRITE && s->now_ms-c->started_ms>=SETTING_CONFIRM_TIMEOUT_MS)
        return finish(c,s,PANEL_WRITE_UNCERTAIN,io);
    if((int32_t)(s->now_ms-c->next_io_ms)<0)return false;
    if(c->stage==PANEL_CONTROL_WRITE){
        /* Schedule the 600 ms guard without sleeping; recheck current state
         * after the delay, then transmit exactly once. */
        if(!panel_setting_allowed(s,c->reg,c->value))return finish(c,s,PANEL_WRITE_REJECTED,io);
        bool check_output=panel_power_changes_output(c->reg,c->value);
        if(check_output){
            c->output_started_opposite=panel_power_output_state(s)==(c->value==1?0:1);
            if(c->require_opposite_output && !c->output_started_opposite)
                return finish(c,s,PANEL_WRITE_REJECTED,io);
        }
        report(s,PANEL_WRITE_SENDING,io);
        c->last_write_ms=s->now_ms;c->have_write=true;
        panel_io_result_t result=io->write(io->context,c->reg,c->value,&s->exception);
        if(result!=PANEL_IO_EXCEPTION){
            if(c->reg==0x4107)panel_invalidate(s,0x150);
            if(c->reg==0x4103)panel_invalidate(s,0x140);
        }
        if(check_output && result!=PANEL_IO_EXCEPTION){
            c->started_ms=c->output_last_ms=io->now(io->context);
            c->output_samples=0;c->stage=PANEL_CONTROL_POWER_OUTPUT;
            if(result==PANEL_IO_OK)report(s,PANEL_WRITE_ACK,io);
            return false;
        }
        if(result!=PANEL_IO_OK)
            return finish(c,s,result==PANEL_IO_EXCEPTION?PANEL_WRITE_REJECTED:PANEL_WRITE_UNCERTAIN,io);
        report(s,PANEL_WRITE_ACK,io);c->started_ms=s->now_ms;
        c->next_io_ms=s->now_ms+SETTING_SETTLE_MS;
        c->stage=c->actual_reg==0xffff?PANEL_CONTROL_MARKER:PANEL_CONTROL_ACTUAL;
        return false;
    }
    uint16_t raw=0;uint8_t exception=0;
    uint16_t flags=panel_setting_current_flags(c->reg);
    uint16_t address=c->stage==PANEL_CONTROL_MARKER?c->reg:
        c->stage==PANEL_CONTROL_FLAGS?flags:c->actual_reg;
    panel_io_result_t result=io->read(io->context,address,1,&raw,&exception);
    s->now_ms=io->now(io->context);
    if(c->stage==PANEL_CONTROL_MARKER){
        c->marker_seen=result==PANEL_IO_OK;c->marker_raw=raw;
        return finish(c,s,c->marker_seen && raw==0xffff?
            PANEL_WRITE_PROTOCOL_ACCEPTED:PANEL_WRITE_UNCERTAIN,io);
    }
    if(result==PANEL_IO_OK){
        panel_store(s,address,raw,s->now_ms);
        if(c->stage==PANEL_CONTROL_ACTUAL){
            c->actual_seen=true;c->actual_raw=raw;
            if(flags!=0xffff){c->stage=PANEL_CONTROL_FLAGS;return false;}
        }
        c->readback_valid=panel_setting_decode_current(s,c->reg,c->actual_raw,&c->readback_value);
        if(c->readback_valid)
            return finish(c,s,c->readback_value==c->value?PANEL_WRITE_CONFIRMED:PANEL_WRITE_PROCESSED,io);
    }
    /* Some inverters only expose the command's completion receipt. */
    c->stage=PANEL_CONTROL_MARKER;
    return false;
}

void panel_control_write(panel_control_t *c,panel_snapshot_t *s,uint16_t reg,
                         uint16_t value,const panel_control_io_t *io)
{
    if(!io || !io->sleep || !panel_control_begin(c,s,reg,value,io))return;
    while(panel_control_active(c)){
        panel_control_tick(c,s,io);
        if(panel_control_active(c))io->sleep(io->context,10);
    }
}
