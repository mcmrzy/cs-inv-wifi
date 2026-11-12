#include "panel_alarm.h"
#include "panel_modbus.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define ALARM_LEGACY_MAGIC 0x4d424101U
#define ALARM_MAGIC 0x4d424102U
static int slot(uint16_t code)
{
    unsigned reg=code>>8,bit=code&255U;
    if(reg>=0x46 && reg<=0x49 && bit<16)return (int)((reg-0x46)*16+bit);
    if(code>=PANEL_ALARM_COMM && code<=PANEL_ALARM_UNKNOWN)return (int)(64+code-PANEL_ALARM_COMM);
    if(reg>=0x56 && reg<=0x59 && bit<16)return (int)(PANEL_ALARM_LEGACY_SLOTS+(reg-0x56)*16+bit);
    return -1;
}
static bool system_code(unsigned index){return index>=64 && index<PANEL_ALARM_LEGACY_SLOTS;}
static bool fault_code(unsigned index){return index<32 || index==64 ||
    (index>=PANEL_ALARM_LEGACY_SLOTS && index<PANEL_ALARM_LEGACY_SLOTS+32);}

static bool event_valid(const panel_alarm_event_t *e)
{
    int index=slot(e->code);
    if(index<0 || !e->serial || (e->flags&~0x83U))return false;
    return e->source==(system_code(index)?2:0) && ((e->flags&2U)!=0)==fault_code(index);
}
void panel_alarm_init(panel_alarm_store_t *s)
{memset(s,0,sizeof(*s));s->magic=ALARM_MAGIC;s->next_serial=1;panel_alarm_seal(s);}
void panel_alarm_seal(panel_alarm_store_t *s)
{s->crc=panel_mb_crc((const uint8_t *)s,offsetof(panel_alarm_store_t,crc));}
bool panel_alarm_validate(const panel_alarm_store_t *s)
{
    if(!s || s->magic!=ALARM_MAGIC || !s->next_serial || s->head>=PANEL_ALARM_DEPTH ||
       s->count>PANEL_ALARM_DEPTH || ((s->known|s->known_3ku)&~15U) ||
       (s->sys_known&~7U) || (s->sys_active&~7U) ||
       s->crc!=panel_mb_crc((const uint8_t *)s,offsetof(panel_alarm_store_t,crc)))return false;
    for(unsigned i=0;i<s->count;i++)if(!event_valid(&s->events[(s->head+i)%PANEL_ALARM_DEPTH]))return false;
    return true;
}
bool panel_alarm_restore(panel_alarm_store_t *s,size_t bytes)
{
    if(bytes==sizeof(*s))return panel_alarm_validate(s);
    const size_t prefix=offsetof(panel_alarm_store_t,legacy_end);
    uint16_t old_crc;
    if(bytes!=prefix+sizeof(old_crc) || s->magic!=ALARM_LEGACY_MAGIC || s->known_3ku)return false;
    memcpy(&old_crc,(uint8_t *)s+prefix,sizeof(old_crc));
    if(old_crc!=panel_mb_crc((const uint8_t *)s,prefix))return false;
    memset((uint8_t *)s+prefix,0,sizeof(*s)-prefix);
    s->magic=ALARM_MAGIC;
    panel_alarm_seal(s);
    return panel_alarm_validate(s);
}

static bool enqueue(panel_alarm_store_t *s,uint16_t code,bool active,uint32_t ms,uint32_t epoch,bool reserve)
{
    if(s->count>=(reserve?PANEL_ALARM_DEPTH:PANEL_ALARM_DEPTH-1))return false;
    int index=slot(code);if(index<0)return false;
    panel_alarm_event_t *e=&s->events[(s->head+s->count)%PANEL_ALARM_DEPTH];
    *e=(panel_alarm_event_t){.serial=s->next_serial++,.epoch=epoch,.uptime_ms=ms,.code=code,
        .source=(uint8_t)(system_code(index)?2:0),.flags=(uint8_t)((active?1:0)|(fault_code(index)?2:0))};
    if(!s->next_serial)s->next_serial=1;
    s->count++;return true;
}
static bool mark_loss(panel_alarm_store_t *s,uint32_t ms)
{
    if(s->sys_active&2U)return false;
    if(!enqueue(s,PANEL_ALARM_LOSS,true,ms,0,true))return false;
    s->sys_known|=2U;s->sys_active|=2U;s->loss_count++;return true;
}
static bool system_state(panel_alarm_store_t *s,unsigned bit,bool active,uint32_t ms)
{
    uint8_t mask=(uint8_t)(1U<<bit);bool known=(s->sys_known&mask)!=0;
    if(known && ((s->sys_active&mask)!=0)==active)return false;
    if(active || known){
        if(!enqueue(s,(uint16_t)(PANEL_ALARM_COMM+bit),active,ms,0,bit==1))return mark_loss(s,ms);
    }
    s->sys_known|=mask;
    if(active)s->sys_active|=mask;else s->sys_active&=(uint8_t)~mask;
    return true;
}
bool panel_alarm_observe(panel_alarm_store_t *s,const panel_alarm_sample_t *sample,bool grace)
{
    if(!s || !sample || sample->paused)return false;
    bool changed=false;
    uint8_t *known=sample->is_3ku?&s->known_3ku:&s->known;
    unsigned code_base=sample->is_3ku?0x4600U+PANEL_ALARM_3KU_OFFSET:0x4600U;
    for(unsigned word=0;word<4;word++)if(sample->fresh&(1U<<word)){
        uint16_t before=sample->is_3ku?s->words_3ku[word]:s->words[word],after=sample->words[word];
        if(!(*known&(1U<<word))){*known|=(uint8_t)(1U<<word);changed=true;}
        /* Newly active bits first, then recoveries. Every bit has its own key;
         * changes to one bit must not clear another still-active alarm. */
        for(unsigned phase=0;phase<2;phase++)for(unsigned bit=0;bit<16;bit++){
            uint16_t mask=(uint16_t)(1U<<bit);
            bool active=(after&mask)!=0;
            if(((before^after)&mask)==0 || active!=(phase==0))continue;
            if(enqueue(s,(uint16_t)(code_base+(word<<8)+bit),active,sample->now_ms,0,false)){
                before^=mask;
                changed=true;
            }else changed=mark_loss(s,sample->now_ms)||changed;
        }
        if(sample->is_3ku)s->words_3ku[word]=before;else s->words[word]=before;
    }
    if(grace || sample->reachable)changed=system_state(s,0,!sample->reachable,sample->now_ms)||changed;
    if(grace || sample->fresh==15)changed=system_state(s,2,sample->fresh!=15,sample->now_ms)||changed;
    if((s->sys_active&2U) && s->count<PANEL_ALARM_DEPTH/2 && sample->fresh==15)
        changed=system_state(s,1,false,sample->now_ms)||changed;
    return changed;
}
const panel_alarm_event_t *panel_alarm_front(const panel_alarm_store_t *s)
{return s && s->count?&s->events[s->head]:NULL;}
bool panel_alarm_pop(panel_alarm_store_t *s,uint32_t serial)
{
    if(!s || !s->count || s->events[s->head].serial!=serial)return false;
    memset(&s->events[s->head],0,sizeof(s->events[s->head]));
    s->head=(uint16_t)((s->head+1)%PANEL_ALARM_DEPTH);s->count--;return true;
}
bool panel_alarm_assign_time(panel_alarm_store_t *s,uint32_t epoch,uint32_t ms,uint32_t boot_serial)
{
    if(!s || !s->count || epoch<1577836800U)return false;
    panel_alarm_event_t *e=&s->events[s->head];if(e->epoch)return false;
    uint32_t stamp=epoch;
    if(e->serial>=boot_serial){
        uint32_t age=(ms-e->uptime_ms)/1000U;
        if(age<epoch-1577836800U)stamp-=age;
    }else e->flags|=0x80U; /* Original UTC unknown; do not claim exact occurrence time. */
    int index=slot(e->code);if(index<0)return false;
    bool is_3ku=(unsigned)index>=PANEL_ALARM_LEGACY_SLOTS;
    uint32_t previous=is_3ku?s->last_epoch_3ku[index-PANEL_ALARM_LEGACY_SLOTS]:s->last_epoch[index];
    if(stamp<=previous)stamp=previous+1U;
    e->epoch=stamp;
    if(is_3ku)s->last_epoch_3ku[index-PANEL_ALARM_LEGACY_SLOTS]=stamp;else s->last_epoch[index]=stamp;
    return true;
}
bool panel_alarm_resync(panel_alarm_store_t *s,uint32_t ms)
{
    if(!s)return false;
    bool changed=false;
    for(unsigned index=0;index<PANEL_ALARM_SLOTS;index++){
        bool is_3ku=index>=PANEL_ALARM_LEGACY_SLOTS;
        unsigned offset=is_3ku?index-PANEL_ALARM_LEGACY_SLOTS:index;
        uint32_t epoch=is_3ku?s->last_epoch_3ku[offset]:s->last_epoch[offset];
        if(!epoch)continue;
        unsigned word=offset/16,bit=offset%16;
        bool system=system_code(index);
        uint16_t code=(uint16_t)(system?PANEL_ALARM_COMM+index-64:
            0x4600U+(word<<8)+bit+(is_3ku?PANEL_ALARM_3KU_OFFSET:0));
        bool active=system?(s->sys_active&(1U<<(index-64)))!=0:
            ((is_3ku?s->words_3ku[word]:s->words[word])&(1U<<bit))!=0;
        bool pending=false;
        for(unsigned i=0;i<s->count;i++)if(s->events[(s->head+i)%PANEL_ALARM_DEPTH].code==code){pending=true;break;}
        if(pending)continue;
        if(enqueue(s,code,active,ms,epoch,false))changed=true;
        else changed=mark_loss(s,ms)||changed;
    }
    return changed;
}
int panel_alarm_json(const panel_alarm_event_t *e,char *out,size_t capacity)
{
    if(!e || !out || !capacity || !event_valid(e) || !e->epoch)return -1;
    int n=snprintf(out,capacity,"{\"t\":%"PRIu32",\"v\":1,\"data\":{\"source\":%u,\"code\":%u,\"level\":%u,\"state\":%u}}",
        e->epoch,e->source,e->code,(e->flags&2U)?2:1,e->flags&1U);
    return n>0 && (size_t)n<capacity?n:-1;
}
