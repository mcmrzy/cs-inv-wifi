#include "panel_types.h"
#include <string.h>

void panel_result_record(panel_snapshot_t *s,uint32_t sequence,panel_write_state_t state,uint8_t exception)
{
    if(!s)return;
    panel_result_t *r=&s->results[s->result_next%PANEL_RESULT_SLOTS];
    *r=(panel_result_t){.sequence=sequence,.state=state,.exception=exception,
        .scan_found=s->scan_found,.valid=true};
    s->result_next=(s->result_next+1U)%PANEL_RESULT_SLOTS;
}
const panel_result_t *panel_result_find(const panel_snapshot_t *s,uint32_t sequence)
{
    if(s)for(unsigned i=0;i<PANEL_RESULT_SLOTS;i++)
        if(s->results[i].valid && s->results[i].sequence==sequence)return &s->results[i];
    return NULL;
}
void panel_device_context_reset(panel_snapshot_t *s,const panel_config_t *config)
{
    if(!s || !config)return;
    memset(s->valid,0,sizeof(s->valid));memset(s->values,0,sizeof(s->values));
    memset(s->updated_ms,0,sizeof(s->updated_ms));
    memset(s->setting_reads,0,sizeof(s->setting_reads));
    s->config=*config;s->detected_profile=PANEL_PROFILE_UNKNOWN;
    s->scan_found=0;s->ota_valid=0;s->context_generation++;
}
const panel_setting_read_t *panel_setting_read_find(const panel_snapshot_t *s,uint16_t address)
{
    if(s)for(unsigned i=0;i<PANEL_SETTING_READ_SLOTS;i++)
        if(s->setting_reads[i].address==address)return &s->setting_reads[i];
    return NULL;
}

unsigned panel_slot(uint16_t a)
{
    if(a<=0xa0)return a;
    if(a>=0x140 && a<=0x15f)return 161U+a-0x140;
    if(a>=0x176 && a<=0x181)return 193U+a-0x176;
    if(a>=0x210 && a<=0x227)return 205U+a-0x210;
    if(a>=0x2a0 && a<=0x2bf)return 229U+a-0x2a0;
    if(a>=0x330 && a<=0x336)return 261U+a-0x330;
    if(a>=0x350 && a<=0x351)return 268U+a-0x350;
    if(a==0x190)return 270U;
    if(a>=0x19e && a<=0x1a0)return 271U+a-0x19e;
    return PANEL_CACHE_SLOTS;
}

uint32_t panel_stamp(const panel_snapshot_t *s,uint16_t address)
{ return s && panel_slot(address)<PANEL_CACHE_SLOTS ? s->updated_ms[panel_slot(address)] : 0; }

bool panel_store(panel_snapshot_t *s,uint16_t address,uint16_t value,uint32_t stamp)
{
    unsigned slot=panel_slot(address);
    if(!s || slot>=PANEL_CACHE_SLOTS)return false;
    s->values[slot]=value;s->updated_ms[slot]=stamp;s->valid[slot]=true;
    return true;
}

void panel_invalidate(panel_snapshot_t *s,uint16_t address)
{ if(s && panel_slot(address)<PANEL_CACHE_SLOTS)s->valid[panel_slot(address)]=false; }

bool panel_value(const panel_snapshot_t *s, uint16_t address, uint16_t *value)
{
    unsigned slot=panel_slot(address);
    if (!s || !value || slot >= PANEL_CACHE_SLOTS || !s->valid[slot]) return false;
    *value = s->values[slot];
    return true;
}

bool panel_fresh(const panel_snapshot_t *s, uint16_t address)
{
    unsigned slot=panel_slot(address);
    return s && slot < PANEL_CACHE_SLOTS && s->valid[slot] &&
           s->now_ms - s->updated_ms[slot] <= PANEL_FRESH_MS;
}

bool panel_grid_watts(const panel_snapshot_t *s,int32_t *value,bool *fresh)
{
    uint16_t grid,import;
    if(!panel_value(s,0x53,&grid))return false;
    *fresh=panel_fresh(s,0x53);
    if(panel_is_3ku(s)){
        if(!panel_value(s,0x5e,&import))return false;
        *fresh=*fresh && panel_fresh(s,0x5e);
        *value=(int32_t)import-grid;
    }else *value=(int16_t)grid;
    return true;
}

bool panel_pv_watts(const panel_snapshot_t *s,uint32_t *value,bool *fresh)
{
    uint16_t pv1,pv2;
    if(panel_is_3ku(s)){
        if(!panel_value(s,0x98,&pv1) || !panel_value(s,0x9b,&pv2))return false;
        *fresh=panel_fresh(s,0x98) && panel_fresh(s,0x9b);
        *value=(uint32_t)pv1+pv2;
    }else{
        if(!panel_value(s,0xa0,&pv1))return false;
        *fresh=panel_fresh(s,0xa0);
        *value=pv1;
    }
    return true;
}

int panel_battery_direction(const panel_snapshot_t *s)
{
    uint16_t raw=0;
    if(!panel_fresh(s,0x81) || !panel_value(s,0x81,&raw))return 0;
    int16_t value=(int16_t)raw;
    if(!value && panel_is_3ku(s) && panel_fresh(s,0x85) &&
       panel_stamp(s,0x85)==panel_stamp(s,0x81) && panel_value(s,0x85,&raw))
        value=(int16_t)raw;
    return (value>0)-(value<0);
}

bool panel_online(const panel_snapshot_t *s)
{
    uint16_t state;
    return panel_fresh(s,0x42) && panel_value(s,0x42,&state) && state<=10;
}
bool panel_config_valid(const panel_config_t *config)
{
    static const uint32_t bauds[]={1200,2400,4800,9600,19200,38400,57600,115200};
    if(!config || !config->address || config->address>247 || config->parity>2 ||
       (config->profile_override!=PANEL_PROFILE_UNKNOWN &&
        !panel_profile_supported(config->profile_override)))return false;
    for(unsigned i=0;i<sizeof(bauds)/sizeof(bauds[0]);i++)
        if(config->baud==bauds[i])return true;
    return false;
}
