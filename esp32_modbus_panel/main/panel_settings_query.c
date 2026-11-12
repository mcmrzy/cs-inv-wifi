#include "panel_settings_query.h"
#include "panel_catalog.h"
#include <string.h>

void panel_settings_query_start(panel_settings_query_t *q,panel_snapshot_t *s,const uint16_t *addresses,unsigned count)
{
    if(count>PANEL_SETTING_READ_SLOTS)count=PANEL_SETTING_READ_SLOTS;
    memset(q,0,sizeof(*q));memset(s->setting_reads,0,sizeof(s->setting_reads));
    q->context_generation=s->context_generation;
    for(unsigned i=0;addresses && i<count;i++)if(panel_setting_find(addresses[i])){
        unsigned n=q->count++;q->addresses[n]=addresses[i];
        s->setting_reads[n].address=addresses[i];
    }
}

bool panel_settings_query_tick(panel_settings_query_t *q,panel_snapshot_t *s,const panel_control_io_t *io)
{
    if(!q || !s || !io || !io->read || !io->now || !q->count || s->scanning || s->command_busy)return false;
    s->now_ms=io->now(io->context);
    if(q->context_generation!=s->context_generation){
        uint16_t addresses[PANEL_SETTING_READ_SLOTS];memcpy(addresses,q->addresses,sizeof(addresses));
        panel_settings_query_start(q,s,addresses,q->count);
    }
    if(!panel_online(s)){q->was_online=false;return false;}
    if(!q->was_online){q->cursor=0;q->fallback=false;q->flags_read=false;q->next_round=0;q->was_online=true;}
    if(q->cursor>=q->count){
        if((int32_t)(s->now_ms-q->next_round)<0)return false;
        q->cursor=0;q->fallback=false;q->flags_read=false;
    }
    unsigned slot=q->cursor;
    const panel_setting_t *setting=panel_setting_find(q->addresses[slot]);
    uint16_t independent=panel_setting_current_address(s,setting);
    uint16_t reg=q->fallback || independent==0xffff?setting->address:independent;
    bool actual=reg!=setting->address;
    uint16_t raw=0,value=0;uint8_t exception=0;
    uint16_t flags=panel_setting_current_flags(setting->address);
    if(actual && flags!=0xffff && !q->flags_read){
        panel_io_result_t result=io->read(io->context,flags,1,&raw,&exception);
        s->now_ms=io->now(io->context);q->last_reg=flags;q->last_raw=raw;
        if(result==PANEL_IO_OK){panel_store(s,flags,raw,s->now_ms);q->flags_read=true;}
        else{panel_invalidate(s,flags);q->fallback=true;}
        return true;
    }
    panel_io_result_t result=io->read(io->context,reg,1,&raw,&exception);
    s->now_ms=io->now(io->context);q->last_reg=reg;q->last_raw=raw;
    panel_setting_read_t *r=&s->setting_reads[slot];
    r->address=setting->address;r->stamp=s->now_ms;r->independent=actual;
    if(actual && result==PANEL_IO_OK)panel_store(s,reg,raw,s->now_ms);
    if(actual && (result!=PANEL_IO_OK || !panel_setting_decode_current(s,setting->address,raw,&value))){
        /* An older device may expose only its R/W setting address. Ask it,
         * rather than declaring current-value support absent without a read. */
        q->fallback=true;r->state=PANEL_SETTING_UNREAD;return true;
    }
    if(result==PANEL_IO_OK){
        r->value=actual?value:raw;
        r->state=!actual && raw==0xffff?PANEL_SETTING_EMPTY:PANEL_SETTING_VALUE;
    }else if(result==PANEL_IO_EXCEPTION)
        r->state=exception==1 || exception==2?PANEL_SETTING_UNSUPPORTED:PANEL_SETTING_INVALID;
    else r->state=PANEL_SETTING_TIMEOUT;
    q->fallback=false;q->flags_read=false;q->cursor++;
    if(q->cursor>=q->count)q->next_round=s->now_ms+3000U;
    return true;
}
