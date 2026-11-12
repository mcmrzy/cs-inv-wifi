#include "panel_control.h"
#include "panel_catalog.h"

static void report(panel_snapshot_t *s,panel_write_state_t state,const panel_control_io_t *io)
{
    s->write_state=state;s->now_ms=io->now(io->context);
    if(io->publish)io->publish(io->context,s);
}

static bool actual_matches(const panel_snapshot_t *s,uint16_t reg,uint16_t requested,uint16_t actual)
{
    if(reg!=0x4123){
        uint16_t decoded;
        return panel_setting_decode_current(s,reg,actual,&decoded) && decoded==requested;
    }
    if(requested==2)return actual==(panel_is_3ku(s)?2:8);
    /* Standby may still leave the output off after a start request. */
    return requested==1 && actual>=3 && actual<=6;
}

bool panel_control_active(const panel_control_t *c)
{return c && c->stage!=PANEL_CONTROL_IDLE;}

static bool finish(panel_control_t *c,panel_snapshot_t *s,panel_write_state_t state,
                   const panel_control_io_t *io)
{
    c->stage=PANEL_CONTROL_IDLE;report(s,state,io);return true;
}

static panel_write_state_t completed(const panel_control_t *c)
{
    if(!c->processed)return PANEL_WRITE_UNCERTAIN;
    return c->actual_conflict?PANEL_WRITE_PROCESSED:PANEL_WRITE_PROTOCOL_ACCEPTED;
}

bool panel_control_cancel(panel_control_t *c,panel_snapshot_t *s,const panel_control_io_t *io)
{
    if(!panel_control_active(c) || !s || !io || !io->now)return false;
    return finish(c,s,c->stage==PANEL_CONTROL_WRITE?PANEL_WRITE_REJECTED:
        completed(c),io);
}

bool panel_control_begin(panel_control_t *c,panel_snapshot_t *s,uint16_t reg,
                         uint16_t value,const panel_control_io_t *io)
{
    if(!c || !s || !io || !io->now || !io->read || !io->write || panel_control_active(c))return false;
    c->marker_seen=false;c->actual_seen=false;c->actual_conflict=false;
    c->marker_raw=0;c->actual_raw=0;c->actual_reg=0xffff;
    s->now_ms=io->now(io->context);s->write_address=reg;s->write_value=value;s->exception=0;
    if(s->config.address<1 || s->config.address>247 || s->scanning ||
       !panel_setting_allowed(s,reg,value)){
        report(s,PANEL_WRITE_REJECTED,io);return false;
    }
    c->reg=reg;c->value=value;c->processed=false;c->stage=PANEL_CONTROL_WRITE;
    for(unsigned i=0;i<PANEL_SETTING_READ_SLOTS;i++)
        if(s->setting_reads[i].address==reg)s->setting_reads[i].state=PANEL_SETTING_UNREAD;
    c->context_generation=s->context_generation;
    uint32_t since=s->now_ms-c->last_write_ms;
    c->next_io_ms=s->now_ms+(c->have_write && since<PANEL_WRITE_GAP_MS?PANEL_WRITE_GAP_MS-since:0U);
    c->actual_reg=panel_setting_current_address(s,panel_setting_find(reg));
    if(reg==0x4123 && value!=0)c->actual_reg=0x42;
    report(s,PANEL_WRITE_QUEUED,io);return true;
}

bool panel_control_tick(panel_control_t *c,panel_snapshot_t *s,const panel_control_io_t *io)
{
    if(!panel_control_active(c) || !s || !io || !io->now || !io->read || !io->write)return false;
    s->now_ms=io->now(io->context);
    if(c->context_generation!=s->context_generation)return panel_control_cancel(c,s,io);
    if(c->stage!=PANEL_CONTROL_WRITE && s->now_ms-c->started_ms>=3000U)
        return finish(c,s,completed(c),io);
    if((int32_t)(s->now_ms-c->next_io_ms)<0)return false;
    if(c->stage==PANEL_CONTROL_WRITE){
        /* Schedule the 600 ms guard without sleeping; recheck current state
         * after the delay, then transmit exactly once. */
        if(!panel_setting_allowed(s,c->reg,c->value))return finish(c,s,PANEL_WRITE_REJECTED,io);
        report(s,PANEL_WRITE_SENDING,io);
        c->last_write_ms=s->now_ms;c->have_write=true;
        panel_io_result_t result=io->write(io->context,c->reg,c->value,&s->exception);
        if(result!=PANEL_IO_EXCEPTION){
            if(c->reg==0x4107)panel_invalidate(s,0x150);
            if(c->reg==0x4103)panel_invalidate(s,0x140);
        }
        if(result!=PANEL_IO_OK)
            return finish(c,s,result==PANEL_IO_EXCEPTION?PANEL_WRITE_REJECTED:PANEL_WRITE_UNCERTAIN,io);
        report(s,PANEL_WRITE_ACK,io);c->started_ms=s->now_ms;c->next_io_ms=s->now_ms;
        c->stage=PANEL_CONTROL_MARKER;return false;
    }
    uint16_t raw=0;uint8_t exception=0;
    uint16_t address=c->stage==PANEL_CONTROL_MARKER?c->reg:
        c->stage==PANEL_CONTROL_FLAGS?panel_setting_current_flags(c->reg):c->actual_reg;
    panel_io_result_t result=io->read(io->context,address,1,&raw,&exception);
    s->now_ms=io->now(io->context);c->next_io_ms=s->now_ms+100U;
    if(result==PANEL_IO_EXCEPTION){
        if(c->stage==PANEL_CONTROL_MARKER && c->actual_reg!=0xffff){
            c->stage=PANEL_CONTROL_ACTUAL;return false;
        }
        /* A read exception is not a rejected write value. */
        return finish(c,s,completed(c),io);
    }
    if(result!=PANEL_IO_OK){
        /* A silent confirmation register must not consume a 350 ms timeout
         * every 100 ms and starve otherwise healthy live-data reads. */
        if(c->actual_reg!=0xffff && !c->processed)
            c->stage=c->stage==PANEL_CONTROL_MARKER?PANEL_CONTROL_ACTUAL:PANEL_CONTROL_MARKER;
        c->next_io_ms=s->now_ms+500U;return false;
    }
    if(c->stage==PANEL_CONTROL_MARKER){
        c->marker_seen=true;c->marker_raw=raw;
        if(raw!=0xffff){
            if(c->actual_reg!=0xffff)c->stage=PANEL_CONTROL_ACTUAL;
            return false;
        }
        if(c->actual_reg==0xffff)return finish(c,s,PANEL_WRITE_PROTOCOL_ACCEPTED,io);
        c->processed=true;report(s,PANEL_WRITE_PROCESSED,io);
        c->stage=PANEL_CONTROL_ACTUAL;c->next_io_ms=s->now_ms;return false;
    }
    if(c->stage==PANEL_CONTROL_ACTUAL){
        c->actual_seen=true;c->actual_raw=raw;
        panel_store(s,c->actual_reg,raw,s->now_ms);
        if(panel_setting_current_flags(c->reg)!=0xffff){
            c->stage=PANEL_CONTROL_FLAGS;return false;
        }
    }else panel_store(s,address,raw,s->now_ms);
    if(actual_matches(s,c->reg,c->value,c->actual_raw))return finish(c,s,PANEL_WRITE_CONFIRMED,io);
    c->actual_conflict=true;
    report(s,c->processed?PANEL_WRITE_PROCESSED:PANEL_WRITE_ACK,io);
    c->stage=c->processed?PANEL_CONTROL_ACTUAL:PANEL_CONTROL_MARKER;
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
