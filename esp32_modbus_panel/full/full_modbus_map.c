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
#include <string.h>

const char *full_mb_registry_model(const panel_snapshot_t *s)
{
    if(!s || !panel_online(s) || !panel_fresh(s,0x38) || !panel_fresh(s,0x39))return NULL;
    uint16_t a,b;
    if(!panel_value(s,0x38,&a) || !panel_value(s,0x39,&b))return NULL;
    panel_profile_t profile=panel_detect_profile(a,b);
    if(s->config.profile_override!=PANEL_PROFILE_UNKNOWN && s->config.profile_override!=profile)return NULL;
    switch(profile){
    case PANEL_PROFILE_6KU:return "6KU";
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
    uint16_t state=raw_value(s,0x42),flow=raw_value(s,0x44),bits=status.inverter_status[0];
    if(state==2)next.SysStatus|=1U;
    if(state==7 || status.fault)next.SysStatus|=2U;
    if(direction<0)next.SysStatus|=4U;else if(direction>0)next.SysStatus|=8U;
    if((flow&(1U<<4)) && (flow&(1U<<5)))next.SysStatus|=0x10;
    if((flow&(1U<<0)) && (flow&(1U<<5)))next.SysStatus|=0x20;
    if((flow&(1U<<2)) && (flow&(1U<<5)))next.SysStatus|=0x40;
    if(flow&(1U<<1))next.SysStatus|=0x80;
    if(bits&8U)next.SysStatus|=0x100;
    if(bits&0x30U)next.SysStatus|=0x200;
    if(bits&1U)next.SysStatus|=0x400;
    if(bits&2U)next.SysStatus|=0x800;
    if(panel_fresh(s,0x9c))next.Epv_today=raw_value(s,0x9c);
    if(panel_fresh(s,0x9d))next.Epv_total=raw_value(s,0x9d);
    *out=next;*received_ms=stamp;return true;
}

bool full_mb_read_config(const panel_snapshot_t *s,uint16_t index,bool wire,uint16_t *value)
{
    if(!s || !value)return false;
    uint16_t raw,next,reg;
    switch(index) {
    case 0:reg=0x142;break;case 2:reg=0x140;break;case 13:reg=0x143;break;
    case 4:case 23:reg=0x150;break;
    case 8:reg=0x2a1;break;
    case 5:case 6:case 16:case 17:case 26:reg=0x351;break;
    default:return false;
    }
    if(!fresh_value(s,reg,&raw) || raw==0xffff)return false;
    switch(index) {
    case 0: {
        static const uint16_t values[]={2,4,3};
        if(raw>=3)return false;
        next=values[raw];break;
    }
    case 2:if(raw>2)return false;next=raw+1;break;
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
    case 8:if(raw!=5000 && raw!=6000)return false;next=raw;break;
    case 5:next=(raw>>2)&1U;break;
    case 6:next=(raw>>1)&1U;break;
    case 16:next=(raw>>14)&1U;break;
    case 17:next=(raw>>3)&1U;break;
    case 26:next=(raw>>8)&1U;break;
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
    case 36:return 0x4122;case 37:return 0x4121;case 40:return 0x4120;
    default:return 0xffff;
    }
}

bool full_mb_make_write(const panel_snapshot_t *s,uint16_t index,uint16_t value,bool wire,panel_command_t *command)
{
    if(!s || !command)return false;
    uint16_t reg=write_register(index),raw=value;
    if(reg==0xffff)return false;
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
