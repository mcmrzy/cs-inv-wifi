#include "full_modbus_map.h"

void full_mb_battery(const panel_snapshot_t *s,direct_inverter_battery_t *out)
{
    if(out->context!=s->context_generation)
        *out=(direct_inverter_battery_t){.context=s->context_generation};
    bool was_online=out->online;
    out->online=false;
    uint16_t lock,voltage;
    const bool is_3ku=panel_is_3ku(s);
    /* V1.27 0041: 1=CAN lock, 2=serial lock. Remote ID lock alone is not evidence. */
    if(!panel_online(s) || !panel_fresh(s,0x41) || !panel_value(s,0x41,&lock) ||
       (lock!=2 && (is_3ku || lock!=1)) || !panel_fresh(s,0x88) || !panel_value(s,0x88,&voltage) ||
       (int16_t)voltage<=0 || voltage==0x7fffU)return;
    uint32_t stamp=panel_stamp(s,0x88);
    if(out->valid && !was_online && out->sample_ms==stamp)return;
    static const uint16_t regs[]={0x88,0x89,0x8a,0x8b,0x91,0x92};
    out->valid=0;out->sample_ms=stamp;out->online=true;
    for(unsigned i=0;i<sizeof(regs)/sizeof(regs[0]);i++){
        uint16_t raw;
        if(panel_fresh(s,regs[i]) && panel_value(s,regs[i],&raw) &&
           (i>=4 || (raw!=0x7fffU && raw!=0xffffU && raw!=0x8000U)) && (i!=2 || raw<=100)){
            if(is_3ku && i==1)raw=(uint16_t)((int16_t)raw/10);
            if(is_3ku && i==3){
                int16_t temp=(int16_t)raw;
                if(temp<-3276 || temp>3276)continue;
                raw=(uint16_t)(temp*10);
            }
            out->values[i]=raw;out->valid|=(uint8_t)(1U<<i);
        }
    }
}
#include "panel_catalog.h"
#include <limits.h>
#include <stddef.h>
#include <string.h>

const char *full_mb_registry_model(const panel_snapshot_t *s)
{
    if(!s || !panel_online(s) || !panel_fresh(s,0x38) || !panel_fresh(s,0x39))return NULL;
    uint16_t a,b;
    if(!panel_value(s,0x38,&a) || !panel_value(s,0x39,&b))return NULL;
    panel_profile_t profile=panel_detect_profile(a,b);
    if(s->config.profile_override!=PANEL_PROFILE_UNKNOWN &&
       !panel_profile_same_family(s->config.profile_override,profile))return NULL;
    switch(profile){
    case PANEL_PROFILE_6KU:case PANEL_PROFILE_6KU_PARALLEL:return "6KU";
    case PANEL_PROFILE_12KU:return "12KU";
    case PANEL_PROFILE_2KU:return "PWR2KH12H1";
    case PANEL_PROFILE_3KU:return "3KU";
    default:return NULL;
    }
}
/* V1.27 units and enums are independent of the original DSP protocol.
 * This module only converts semantically known fields, never write markers. */
static uint16_t raw_value(const panel_snapshot_t *s,uint16_t address)
{ uint16_t value=0; (void)panel_value(s,address,&value); return value; }

static bool fresh_value(const panel_snapshot_t *s,uint16_t address,uint16_t *value)
{ return panel_fresh(s,address) && panel_value(s,address,value); }

static bool require(const panel_snapshot_t *s,const uint16_t *addresses,size_t count,uint32_t *oldest)
{
    if(!s)return false;
    uint32_t age=0;
    for(size_t i=0;i<count;i++) {
        if(!panel_fresh(s,addresses[i]))return false;
        uint32_t next=s->now_ms-panel_stamp(s,addresses[i]);
        if(next>age)age=next;
    }
    if(oldest)*oldest=s->now_ms-age;
    return true;
}

static uint16_t magnitude(int16_t value)
{ return value<0 ? (uint16_t)(-(int32_t)value) : (uint16_t)value; }

bool full_mb_status(const panel_snapshot_t *s,direct_dsp_status_t *out,uint32_t *received_ms)
{
    const uint16_t required[]={0x42,0x43,0x44,0x46,0x47,0x48,0x49};
    uint32_t stamp;
    if(!out || !received_ms || !require(s,required,sizeof(required)/sizeof(required[0]),&stamp))return false;
    uint16_t state=raw_value(s,0x42),terminal=raw_value(s,0x43),flow=raw_value(s,0x44);
    if(state>10)return false;
    direct_dsp_status_t next={0};
    next.mode=(uint8_t)state; /* Diagnostics retain the documented Modbus state. */
    uint16_t bits=0;
    if((terminal&3U)==2U)bits|=0x0001; /* GridValid */
    if(((terminal>>2)&3U)==2U)bits|=0x0002; /* GenValid */
    if(((terminal>>6)&3U)==1U)bits|=0x0008; /* LoadValid, not overload/short */
    if(((terminal>>4)&3U)==2U)bits|=0x0010; /* PV1/PV2 aggregate validity */
    if(flow&(1U<<1))bits|=0x0080; /* Grid -> load bypass */
    if(flow&(1U<<5))bits|=0x4000; /* Rectifier -> battery charging */
    next.inverter_status[0]=bits;

    /* Original UI assets use DSP alarm indices. Translate only exact meanings.
     * Reserved bit31 signals at least one unmapped Modbus alarm; the complete
     * source words and explanations remain available on the Modbus alarm page. */
    static const int8_t fault1[16]={-1,11,4,-1,-1,14,-1,16,-1,8,2,-1,-1,-1,-1,-1};
    uint16_t first=raw_value(s,0x46),second=raw_value(s,0x47);
    bool unmapped_fault=second!=0;
    for(unsigned bit=0;bit<16;bit++)if(first&(1U<<bit)) {
        if(fault1[bit]>=0)next.fault|=1UL<<fault1[bit];
        else unmapped_fault=true;
    }
    if(unmapped_fault)next.fault|=1UL<<31;
    uint16_t w1=raw_value(s,0x48),w2=raw_value(s,0x49);
    const uint16_t mapped1=(1U<<0)|(1U<<1)|(1U<<2)|(1U<<5)|(1U<<6);
    const uint16_t mapped2=panel_is_3ku(s)?0:(1U<<8)|(1U<<9);
    if(w1&((1U<<1)|(1U<<2)))next.warning[1]|=1U; /* legacy battery low */
    if(w1&(1U<<0))next.warning[2]|=1U<<14; /* legacy battery disconnected */
    if(w1&(1U<<5))next.warning[2]|=1U<<3; /* legacy over charged */
    if(w1&(1U<<6))next.warning[2]|=1U; /* legacy BMS communication lost */
    if(w2&mapped2&(1U<<8))next.warning[0]|=1U<<1; /* generator voltage low */
    if(w2&mapped2&(1U<<9))next.warning[0]|=1U<<6; /* generator voltage high */
    if((w1&~mapped1) || (w2&~mapped2))next.warning[1]|=1U<<15;
    *out=next;*received_ms=stamp;return true;
}

static const uint16_t measurement_regs[]={0x42,0x43,0x44,0x46,0x47,0x48,0x49,
    0x50,0x51,0x52,0x53,0x58,0x59,0x5a,0x5b,0x5c,0x5d,
    0x80,0x81,0x84,0x85,0x96,0x97,0x98,0x99,0x9a,0x9b,0xa0,0x154,0x330,0x331};
static const uint16_t nonnegative_regs[]={0x50,0x52,0x58,0x5a,0x5d,0x80,0x84,0x96,0x97,0x98,0x99,0x9a,0x9b,0x154};

static bool home_fresh(const panel_snapshot_t *s,uint16_t address,const uint32_t *after)
{
    uint32_t stamp=panel_stamp(s,address);
    return !s->scanning && s->now_ms-stamp<FULL_MB_MEASUREMENT_FRESH_MS &&
        (!after || (int32_t)(stamp-*after)>0);
}

void full_mb_home(const panel_snapshot_t *s,const uint32_t *after,panel_home_readings_t *out)
{
    memset(out,0,sizeof(*out));
    if(!s)return;
    const bool three=panel_is_3ku(s);
    const uint16_t volts[]={0x50,0x96,0x80,0x58};
    uint16_t raw,current;
    for(unsigned i=0;i<PANEL_HOME_COUNT;i++){
        if(!panel_value(s,volts[i],&raw) || raw>INT16_MAX)continue;
        out->meter[i].decivolts=raw;
        out->meter[i].available=PANEL_HOME_VOLTAGE;
        if(home_fresh(s,volts[i],after))out->meter[i].fresh=PANEL_HOME_VOLTAGE;
    }
    panel_home_meter_t *grid=&out->meter[PANEL_HOME_GRID],*pv=&out->meter[PANEL_HOME_SOLAR];
    panel_home_meter_t *battery=&out->meter[PANEL_HOME_BATTERY],*load=&out->meter[PANEL_HOME_LOAD];
    bool fresh;
    uint32_t watts;
    if(panel_grid_watts(s,&grid->watts,&fresh)){
        grid->available|=PANEL_HOME_POWER;
        if(fresh && home_fresh(s,0x53,after) && (!three || home_fresh(s,0x5e,after)))
            grid->fresh|=PANEL_HOME_POWER;
    }
    if(panel_pv_watts(s,&watts,&fresh)){
        pv->watts=(int32_t)watts;pv->available|=PANEL_HOME_POWER;
        if(fresh && (three?(home_fresh(s,0x98,after) && home_fresh(s,0x9b,after)):
                            home_fresh(s,0xa0,after)))pv->fresh|=PANEL_HOME_POWER;
    }
    if(panel_value(s,0x5b,&raw)){
        load->watts=(int16_t)raw;load->available|=PANEL_HOME_POWER;
        if(home_fresh(s,0x5b,after))load->fresh|=PANEL_HOME_POWER;
    }
    if(panel_value(s,0x85,&raw)){
        battery->watts=magnitude((int16_t)raw);battery->available|=PANEL_HOME_POWER;
        if(home_fresh(s,0x85,after))battery->fresh|=PANEL_HOME_POWER;
        if(panel_value(s,0x81,&current) && panel_stamp(s,0x85)==panel_stamp(s,0x81)){
            int16_t signed_current=(int16_t)current;
            int16_t direction=three && !signed_current?(int16_t)raw:signed_current;
            out->battery_direction=(direction>0)-(direction<0);
        }
    }
    if(panel_value(s,0x84,&raw) && raw<=1000){
        out->battery_soc=(uint8_t)(raw/10U);battery->available|=PANEL_HOME_SOC;
        if(home_fresh(s,0x84,after))battery->fresh|=PANEL_HOME_SOC;
    }
}

static full_mb_data_state_t measurement_check(const panel_snapshot_t *s,uint16_t *problem,uint32_t *stamp)
{
    uint16_t raw;
    uint32_t oldest_age=0;
    if(problem)*problem=0x42;
    for(unsigned i=0;i<sizeof(measurement_regs)/sizeof(measurement_regs[0]);i++){
        uint16_t reg=measurement_regs[i];
        if(reg==0xa0 && panel_is_3ku(s))reg=0x5e;
        if(problem)*problem=reg;
        if(!panel_value(s,reg,&raw))return FULL_MB_DATA_MISSING;
        uint32_t age=s->now_ms-panel_stamp(s,reg);
        if(!panel_fresh(s,reg) || age>FULL_MB_MEASUREMENT_FRESH_MS)
            return FULL_MB_DATA_STALE;
        if(age>oldest_age)oldest_age=age;
    }
    for(unsigned i=0;i<sizeof(nonnegative_regs)/sizeof(nonnegative_regs[0]);i++){
        if(problem)*problem=nonnegative_regs[i];
        if((int16_t)raw_value(s,nonnegative_regs[i])<0)return FULL_MB_DATA_INVALID;
    }
    if(raw_value(s,0x42)>10){if(problem)*problem=0x42;return FULL_MB_DATA_INVALID;}
    if(raw_value(s,0x84)>1000){if(problem)*problem=0x84;return FULL_MB_DATA_INVALID;}
    if(stamp)*stamp=s->now_ms-oldest_age;
    if(problem)*problem=0;
    return FULL_MB_DATA_READY;
}

full_mb_data_state_t full_mb_measurement_check(const panel_snapshot_t *s,uint16_t *problem)
{
    return measurement_check(s,problem,NULL);
}

static uint16_t measurement_status(const panel_snapshot_t *s,const direct_dsp_status_t *status,int direction)
{
    uint16_t state=raw_value(s,0x42),flow=raw_value(s,0x44),bits=status->inverter_status[0];
    uint16_t flags=(state==2)?1U:0U;
    if(state==7 || status->fault)flags|=2U;
    if(direction<0)flags|=4U;else if(direction>0)flags|=8U;
    if((flow&(1U<<4)) && (flow&(1U<<5)))flags|=0x10;
    if((flow&(1U<<0)) && (flow&(1U<<5)))flags|=0x20;
    if((flow&(1U<<2)) && (flow&(1U<<5)))flags|=0x40;
    if(flow&(1U<<1))flags|=0x80;
    if(bits&8U)flags|=0x100;
    if(bits&0x30U)flags|=0x200;
    if(bits&1U)flags|=0x400;
    if(bits&2U)flags|=0x800;
    return flags;
}

bool full_mb_measurement(const panel_snapshot_t *s,RunParamDef *out,uint32_t *received_ms)
{
    /* Legacy telemetry has no per-field validity flags. Its published sensor
     * fields must therefore have actual fresh evidence, rather than zero-filled
     * unknown temperatures or bus voltage. Reserved legacy fields stay zero. */
    uint32_t stamp,status_stamp;direct_dsp_status_t status;
    if(!out || !received_ms || measurement_check(s,NULL,&stamp)!=FULL_MB_DATA_READY ||
       !full_mb_status(s,&status,&status_stamp))return false;
    int32_t grid;
    uint32_t pv;
    bool fresh;
    if(!panel_grid_watts(s,&grid,&fresh) || grid<INT16_MIN || grid>INT16_MAX ||
       !panel_pv_watts(s,&pv,&fresh) || pv>UINT16_MAX)return false;
    RunParamDef next={0};
    next.GridVolt=raw_value(s,0x50)/10U;
    next.GridFreq=raw_value(s,0x52)/10U;
    next.ACInWatt=(int16_t)grid;
    next.ACChrWatt=next.ACDisChrWatt=next.ACInWatt;
    next.ACChrCurr=(int16_t)raw_value(s,0x51);
    /* Simplify has no grid VA register. Compute apparent power from the same
     * fresh voltage/current RMS readings, retaining grid power direction. */
    uint32_t va=(uint32_t)raw_value(s,0x50)*magnitude(next.ACChrCurr)/1000U;
    if(va>INT16_MAX)va=INT16_MAX;
    next.ACInVA=(int16_t)(next.ACInWatt<0 ? -(int32_t)va : (int32_t)va);
    next.ACChrVA=next.ACDisChrVA=next.ACInVA;
    next.ACOutputVolt=raw_value(s,0x58)/10U;
    next.OutputCurr=(int16_t)raw_value(s,0x59);
    next.ACOutputFreq=raw_value(s,0x5a)/10U;
    next.OutputWatt=(int16_t)raw_value(s,0x5b);
    next.OutputVA=(int16_t)raw_value(s,0x5c);
    next.LoadPercent=raw_value(s,0x5d);
    next.BatVolt=raw_value(s,0x80);next.BatterySOC=raw_value(s,0x84)/10U;
    int16_t current=(int16_t)raw_value(s,0x81);
    uint16_t power=magnitude((int16_t)raw_value(s,0x85));
    /* Share direction with CHG. 3KU signed watts retain small loads when
     * the 0.1A current word rounds to zero; no invented current is sent. */
    int direction=panel_battery_direction(s);
    if(direction>0){next.BatDischgCurr=magnitude(current);next.BatDisChrWatt=power;}
    else if(direction<0){next.BatChgCurr=magnitude(current);next.BatChrWatt=power;}
    next.BusVolt=raw_value(s,0x154);
    next.Vpv1=raw_value(s,0x96)/10U;next.Buck1Curr=raw_value(s,0x97);next.Ppv1=raw_value(s,0x98);
    next.Vpv2=raw_value(s,0x99)/10U;next.Buck2Curr=raw_value(s,0x9a);next.Ppv2=raw_value(s,0x9b);
    next.Ppv=(uint16_t)pv;
    const bool is_3ku=panel_is_3ku(s);
    int16_t inverter_temp=(int16_t)raw_value(s,0x331);
    if(is_3ku && (inverter_temp<-3276 || inverter_temp>3276))return false;
    next.BoostTemp=(int16_t)raw_value(s,0x330)/(is_3ku?1:10);
    next.InvertTemp=is_3ku?inverter_temp*10:inverter_temp;
    next.Warning=status.warning[1];next.BmsWarning=status.warning[2]&0x1fffU;
    next.BatOverCharge=(raw_value(s,0x48)>>5)&1U;next.FaultValue=status.fault;
    next.SysStatus=measurement_status(s,&status,direction);
    if(panel_fresh(s,0x9c))next.Epv_today=raw_value(s,0x9c);
    if(panel_fresh(s,0x9d))next.Epv_total=raw_value(s,0x9d);
    *out=next;*received_ms=stamp;return true;
}

static bool telemetry_reg(const panel_snapshot_t *s,uint16_t address,uint16_t *value)
{
    return fresh_value(s,address,value) && s->now_ms-panel_stamp(s,address)<=FULL_MB_MEASUREMENT_FRESH_MS;
}

bool full_mb_partial_telemetry(const panel_snapshot_t *s,RunParamDef *out,bool words[RUN_PARAM_WORDS])
{
    if(!s || !out || !words || s->scanning || !panel_online(s))return false;
    memset(out,0,sizeof(*out));
    memset(words,0,RUN_PARAM_WORDS*sizeof(*words));
    #define MARK(field) words[offsetof(RunParamDef,field)/sizeof(uint16_t)]=true
    #define READ_FIELD(field,address,scale,type) do { \
        uint16_t raw; \
        if(telemetry_reg(s,address,&raw)){out->field=(type)raw/(scale);MARK(field);} \
    } while(0)

    uint16_t raw,voltage;
    int32_t grid;
    uint32_t pv;
    bool fresh;
    const bool three=panel_is_3ku(s);
    if(telemetry_reg(s,0x53,&raw) && (!three || telemetry_reg(s,0x5e,&raw)) &&
       panel_grid_watts(s,&grid,&fresh) && fresh && grid>=INT16_MIN && grid<=INT16_MAX){
        out->ACInWatt=(int16_t)grid;MARK(ACInWatt);
        if(telemetry_reg(s,0x50,&voltage) && telemetry_reg(s,0x51,&raw)){
            uint32_t va=(uint32_t)voltage*magnitude((int16_t)raw)/1000U;
            if(va>INT16_MAX)va=INT16_MAX;
            out->ACInVA=grid<0 ? -(int16_t)va : (int16_t)va;MARK(ACInVA);
        }
    }
    READ_FIELD(GridVolt,0x50,10U,uint16_t);
    READ_FIELD(GridFreq,0x52,10U,uint16_t);
    READ_FIELD(ACChrCurr,0x51,1,int16_t);
    READ_FIELD(ACOutputVolt,0x58,10U,uint16_t);
    READ_FIELD(OutputCurr,0x59,1,int16_t);
    READ_FIELD(ACOutputFreq,0x5a,10U,uint16_t);
    READ_FIELD(OutputWatt,0x5b,1,int16_t);
    READ_FIELD(OutputVA,0x5c,1,int16_t);
    READ_FIELD(LoadPercent,0x5d,1U,uint16_t);
    READ_FIELD(BatVolt,0x80,1U,uint16_t);
    if(telemetry_reg(s,0x84,&raw) && raw<=1000){out->BatterySOC=raw/10U;MARK(BatterySOC);}
    uint16_t current;
    int direction=0;
    bool battery_fresh=false;
    if(telemetry_reg(s,0x81,&current) && telemetry_reg(s,0x85,&raw) &&
       panel_stamp(s,0x81)==panel_stamp(s,0x85)){
        direction=panel_battery_direction(s);
        if(direction<0){out->BatChgCurr=magnitude((int16_t)current);out->BatChrWatt=magnitude((int16_t)raw);}
        if(direction>0){out->BatDischgCurr=magnitude((int16_t)current);out->BatDisChrWatt=magnitude((int16_t)raw);}
        battery_fresh=direction || raw==0;
        if(battery_fresh){MARK(BatChgCurr);MARK(BatDischgCurr);MARK(BatChrWatt);MARK(BatDisChrWatt);}
    }
    direct_dsp_status_t status;
    uint32_t received;
    if(full_mb_status(s,&status,&received) && s->now_ms-received<=FULL_MB_MEASUREMENT_FRESH_MS){
        out->FaultValue=status.fault;MARK(FaultValue);words[offsetof(RunParamDef,FaultValue)/2+1]=true;
        out->Warning=status.warning[1];MARK(Warning);
        out->BmsWarning=status.warning[2]&0x1fffU;MARK(BmsWarning);
        out->BatOverCharge=(raw_value(s,0x48)>>5)&1U;MARK(BatOverCharge);
        if(battery_fresh){out->SysStatus=measurement_status(s,&status,direction);MARK(SysStatus);}
    }
    READ_FIELD(Vpv1,0x96,10U,uint16_t);
    READ_FIELD(Buck1Curr,0x97,1U,uint16_t);
    READ_FIELD(Ppv1,0x98,1U,uint16_t);
    READ_FIELD(Vpv2,0x99,10U,uint16_t);
    READ_FIELD(Buck2Curr,0x9a,1U,uint16_t);
    READ_FIELD(Ppv2,0x9b,1U,uint16_t);
    if((three ? (telemetry_reg(s,0x98,&raw) && telemetry_reg(s,0x9b,&raw)) : telemetry_reg(s,0xa0,&raw)) &&
       panel_pv_watts(s,&pv,&fresh) && fresh && pv<=UINT16_MAX){out->Ppv=(uint16_t)pv;MARK(Ppv);}
    READ_FIELD(BusVolt,0x154,1U,uint16_t);
    READ_FIELD(BoostTemp,0x330,(three?1:10),int16_t);
    if(telemetry_reg(s,0x331,&raw)){
        int32_t temperature=(int16_t)raw*(three?10:1);
        if(temperature>=INT16_MIN && temperature<=INT16_MAX){out->InvertTemp=(int16_t)temperature;MARK(InvertTemp);}
    }
    if(telemetry_reg(s,0x9c,&raw)){
        out->Epv_today=raw;MARK(Epv_today);words[offsetof(RunParamDef,Epv_today)/2+1]=true;
    }
    if(telemetry_reg(s,0x9d,&raw)){
        out->Epv_total=raw;MARK(Epv_total);words[offsetof(RunParamDef,Epv_total)/2+1]=true;
    }
    #undef READ_FIELD
    #undef MARK
    for(unsigned i=0;i<RUN_PARAM_WORDS;i++)if(words[i])return true;
    return false;
}

static uint16_t system_setting_register(uint16_t index)
{
    switch(index) {
    case FULL_MB_POWER_SAVING:return 0x412a;
    case FULL_MB_AUTO_FREQUENCY:return 0x412b;
    case FULL_MB_BATTERY_OPEN_ALARM:return 0x412d;
    case FULL_MB_INVERTER_MUTE:return 0x4130;
    default:return 0;
    }
}

static uint16_t soc_setting_register(uint16_t index)
{
    switch(index) {
    case 36:return 0x4122; /* Back to utility. */
    case 37:return 0x4121; /* Back to battery. */
    case 40:return 0x4120; /* Low SOC shutdown. */
    default:return 0;
    }
}

static bool modbus_lead_battery(const panel_snapshot_t *s)
{
    uint16_t battery;
    return fresh_value(s,0x150,&battery) && battery<=1;
}

bool full_mb_read_config(const panel_snapshot_t *s,uint16_t index,bool wire,uint16_t *value)
{
    if(!s || !value)return false;
    uint16_t raw,next,reg;
    uint16_t system=system_setting_register(index);
    if(system){
        return wire && full_mb_registry_model(s) && fresh_value(s,0x351,&raw) &&
               panel_setting_decode_current(s,system,raw,value);
    }
    uint16_t soc=soc_setting_register(index);
    if(soc){
        const panel_setting_t *setting=panel_setting_find(soc);
        raw=0xffff;
        /* The enabled flags are authoritative for OFF. A stale threshold
         * must not turn into a numeric setting when its flag is set. */
        (void)fresh_value(s,setting->read_address,&raw);
        if(!panel_setting_decode_current(s,soc,raw,&next))return false;
        *value=wire && next==0xfff1?0:next;
        return true;
    }
    if(index>=27 && index<=30 && (!panel_is_3ku(s) || !modbus_lead_battery(s)))return false;
    switch(index) {
    case 0:reg=0x142;break;case 2:reg=0x140;break;case 13:reg=0x143;break;
    case 1:if(!panel_is_3ku(s))return false;reg=0x179;break;
    case 4:case 23:reg=0x150;break;
    case 7:reg=0x2a0;break;case 8:reg=0x2a1;break;
    case 5:case 6:case 16:case 17:case 26:reg=0x351;break;
    case 27:case 28:case 29:case 30:reg=(uint16_t)(0x17e + index - 27);break;
    default:return false;
    }
    if(!fresh_value(s,reg,&raw) || raw==0xffff)return false;
    switch(index) {
    case 0: {
        static const uint16_t values[]={2,4,3};
        if(raw>=3)return false;
        next=values[raw];break;
    }
    case 1: {
        uint16_t decoded;
        if(!panel_setting_decode_current(s,0x4106,raw,&decoded))return false;
        next=decoded?decoded*10U:2U;
        if(wire)next*=10U;
        break;
    }
    case 2:if(raw>2 || (raw==2 && panel_is_3ku(s)))return false;next=raw+1;break;
    case 13: {
        static const uint16_t native[]={3,4,1},collector[]={2,3,1};
        if(raw>2)return false;
        next=wire?collector[raw]:native[raw];break;
    }
    case 4:
        if(s->config.profile_override==PANEL_PROFILE_4KL || s->detected_profile==PANEL_PROFILE_4KL || raw>1)return false;
        next=raw+1;break; /* AGM/Flooded only; lithium chemistry != managed/custom. */
    case 23:
        if(panel_is_3ku(s) || s->config.profile_override==PANEL_PROFILE_4KL ||
           s->detected_profile==PANEL_PROFILE_4KL || (raw!=2 && raw!=3))return false;
        next=raw==2 ? 1 : 0;break;
    case 7: {
        uint16_t decoded;
        if(!panel_setting_decode_current(s,0x4100,raw,&decoded))return false;
        next=raw;break;
    }
    case 8:if(raw!=5000 && raw!=6000)return false;next=raw;break;
    case 5:next=(raw>>2)&1U;break;
    case 6:next=(raw>>1)&1U;break;
    case 16:next=(raw>>14)&1U;break;
    case 17:next=(raw>>3)&1U;break;
    case 26:next=(raw>>8)&1U;break;
    case 27:
        if(raw<240 || raw>300)return false;
        next=raw;break;
    case 28:case 29:case 30:
        if(raw==0 || !panel_setting_decode_current(s,(uint16_t)(0x4113+index-27),raw,&next))
            return false;
        break;
    default:return false;
    }
    *value=next;return true;
}

static uint16_t write_register(uint16_t index)
{
    switch(index) {
    case 0:return 0x4102;case 1:return 0x4106;case 2:return 0x4103;
    case 4:return 0x4107;case 7:return 0x4100;case 8:return 0x4101;
    case 5:return 0x4128;case 6:return 0x4127;
    case 16:return 0x4132;case 17:return 0x4129;
    case 10:return 0x4105;case 11:return 0x410c;case 12:return 0x410d;
    case 13:return 0x4104;case 26:return 0x4112;
    case 27:return 0x4113;case 28:return 0x4114;case 29:return 0x4115;
    case 30:return 0x4116;case 31:return 0x4117;
    case 36:case 37:case 40:return soc_setting_register(index);
    default:return 0xffff;
    }
}

bool full_mb_make_write(const panel_snapshot_t *s,uint16_t index,uint16_t value,bool wire,panel_command_t *command)
{
    if(!s || !command)return false;
    uint16_t reg=write_register(index),raw=value;
    uint16_t system=system_setting_register(index);
    if(system){
        if(!wire || !panel_profile_supported(panel_effective_profile(s)) || !full_mb_registry_model(s))
            return false;
        reg=system;
    }
    if(index==FULL_MB_AC_POWER_INDEX){
        uint16_t state;
        if(!wire || !panel_profile_supported(panel_effective_profile(s)) ||
           !full_mb_registry_model(s) || !fresh_value(s,0x42,&state))return false;
        bool start=value==1 && (state==2 || (state==8 && !panel_is_3ku(s)));
        bool stop=value==2 && state>=3 && state<=6;
        if(!start && !stop)return false;
        reg=0x4123;
    }
    if(reg==0xffff)return false;
    if(wire && soc_setting_register(index)){
        uint16_t battery;
        if(!panel_profile_supported(panel_effective_profile(s)) || !full_mb_registry_model(s))return false;
        if(value==0)raw=0xfff1;
        else if(!fresh_value(s,0x150,&battery) || (battery!=2 && (battery!=3 || panel_is_3ku(s))))
            return false;
    }
    if(wire && index>=26 && index<=30){
        if(panel_effective_profile(s)!=PANEL_PROFILE_3KU || !full_mb_registry_model(s) ||
           !modbus_lead_battery(s))return false;
        /* The 3KU exposes holding and timeout settings as whole hours. */
        if((index==28 || index==29) && (value<60 || value>900 || value%60U))return false;
    }
    if(wire && (index==1 || index==10)) {
        if(value%10U)return false;
        value/=10U;raw=value;
    }
    switch(index) {
    case 0:
        if(value==2)raw=0;else if(value==4)raw=1;else if(value==3)raw=2;else return false;
        break;
    case 1:
        if(value==2)raw=0;else if(value>=10 && value<=160 && value%10U==0)raw=value/10U;else return false;
        break;
    case 2:if(value<1 || value>3)return false;raw=value-1U;break;
    case 4:
        if(s->config.profile_override==PANEL_PROFILE_4KL || (value!=1 && value!=2))return false;
        raw=value-1U;break;
    case 7: {
        static const uint16_t voltages[]={2080,2200,2300,2400};
        bool found=false;
        for(unsigned i=0;i<4;i++)if(value==voltages[i]){raw=(uint16_t)i;found=true;break;}
        if(!found)return false;
        break;
    }
    case 8:if(value==5000)raw=0;else if(value==6000)raw=1;else return false;break;
    case 13:
        if(wire) {
            if(value==1)raw=2;else if(value==2)raw=0;else if(value==3)raw=1;else return false;
        } else {
            if(value==1)raw=2;else if(value==3)raw=0;else if(value==4)raw=1;else return false;
        }
        break;
    default:break;
    }
    if(!panel_setting_allowed(s,reg,raw))return false;
    memset(command,0,sizeof(*command));command->type=PANEL_CMD_WRITE;
    command->address=reg;command->value=raw;return true;
}

bool full_mb_config_writable(const panel_snapshot_t *s,uint16_t index)
{
    uint16_t reg=write_register(index),candidate=0;
    if(reg==0xffff || (index==4 && s && s->config.profile_override==PANEL_PROFILE_4KL))return false;
    return panel_setting_allowed(s,reg,0) || panel_setting_step(s,reg,0,1,&candidate);
}
