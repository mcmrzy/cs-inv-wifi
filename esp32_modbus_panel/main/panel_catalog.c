#include "panel_catalog.h"
#include <stdio.h>

uint16_t panel_battery_suggestion(const panel_snapshot_t *s, uint16_t address,
                                 uint16_t charge_deciamp, uint16_t charge_decivolt,
                                 uint16_t discharge_decivolt)
{
    uint16_t mode,state,other;
    if(!charge_deciamp || !discharge_decivolt || charge_decivolt<=discharge_decivolt ||
       !panel_fresh(s,0x150) || !panel_value(s,0x150,&mode) || mode!=4 ||
       !panel_fresh(s,0x42) || !panel_value(s,0x42,&state) || state!=2)return 0;
    uint16_t value=0;
    if(address==0x4106)value=charge_deciamp/100U; /* Round down to a supported 10 A step. */
    else if(address==0x4109 && panel_fresh(s,0x159) && panel_value(s,0x159,&other) &&
            other>discharge_decivolt && other<charge_decivolt)
        value=discharge_decivolt;
    else if(address==0x410a && panel_fresh(s,0x178) && panel_value(s,0x178,&other) &&
            other>=discharge_decivolt && other<=charge_decivolt)
        value=charge_decivolt;
    return value && panel_setting_allowed(s,address,value)?value:0;
}
/* V1.27 WF_UserSet writes are completion latches (03 -> FFFF).
 * WF_BAT 0177/0178 are charge voltage setpoints, not measured battery voltage.
 * Time setpoints use minutes/days on write but hours in WF_BAT readback. */
static const panel_setting_t settings[] = {
    {0x4100,0x02a0,"输出电压","V",1},
    {0x4101,0x02a1,"输出频率","Hz",1},
    {0x4102,0x0142,"输出优先级","",1},
    {0x4103,0x0140,"运行模式","",1},
    {0x4104,0x0143,"充电优先级","",1},
    {0x4105,0xffff,"市电充电电流","A",1},
    {0x4106,0xffff,"最大充电电流","A",1},
    {0x4107,0x0150,"电池类型","",1},
    {0x4108,0x0159,"电池低压告警点","V",10},
    {0x4109,0xffff,"电池关机点","V",10},
    {0x410a,0x0177,"恒压点","V",10},
    {0x410b,0x0178,"浮充点","V",10},
    {0x410c,0xffff,"电池转市电","V",10},
    {0x410d,0xffff,"市电转电池","V",10},
    {0x4110,0x0141,"并机模式","",1},
    {0x4111,0xffff,"并机序号","",1},
    {0x4112,0x0351,"均衡充电使能","",1},
    {0x4113,0x017e,"均衡电压","V",10},
    {0x4114,0x017f,"均衡充电时间","min",1},
    {0x4115,0x0180,"均衡延时时间","min",1},
    {0x4116,0x0181,"均衡间隔天数","day",1},
    {0x4117,0xffff,"立即均衡充电","",1},
    {0x4118,0x015c,"双输出截止电压","V",10},
    {0x4119,0x015d,"双输出时间","min",1},
    {0x411a,0x0350,"发电机使能","",1},
    {0x411b,0xffff,"发电机额定功率","kW",10},
    {0x411c,0xffff,"发电机最大功率","kW",10},
    {0x411d,0xffff,"消除发电机告警","",1},
    {0x411e,0xffff,"逆变器电池协议","",1},
    {0x411f,0xffff,"BMS地址","",1},
    {0x4120,0x019e,"低SOC关机","%",1},
    {0x4121,0x01a0,"转电池SOC","%",1},
    {0x4122,0x019f,"转市电SOC","%",1},
    {0x4123,0xffff,"开关机","",1},
    {0x4125,0x0350,"逆变器自动熄屏","",1},
    {0x4127,0x0351,"过温重启","",1},
    {0x4128,0x0351,"过载重启","",1},
    {0x4129,0x0351,"过载转旁路","",1},
    {0x412a,0x0351,"低功耗省电","",1},
    {0x412b,0x0351,"自动检测市电频率","",1},
    {0x412c,0x0351,"继电器后软启动","",1},
    {0x412d,0x0351,"电池开路警告","",1},
    {0x4130,0x0351,"逆变器蜂鸣静音","",1},
    {0x4131,0x0351,"自动返回首页","",1},
    {0x4132,0x0351,"主输入变化警告","",1},
    {0x414b,0xffff,"双输出截止SOC","%",1},
    {0x410e,0xffff,"市电低压点","V",1},
    {0x410f,0xffff,"市电高压点","V",1},
    {0x4124,0xffff,"充电功能","",1},
    {0x4142,0xffff,"最大放电电流","A",1},
    {0x4153,0xffff,"并机无电池运行","",1},
    {0x4166,0xffff,"电池未接检测","",1},
    {0x418d,0xffff,"低SOC告警","%",1},
    {0x41a6,0xffff,"温度降额策略","",1},
};

size_t panel_setting_count(void) { return sizeof settings / sizeof settings[0]; }
const char *panel_setting_group_name(unsigned group)
{
    static const char *const names[]={"全部参数","输出与模式","充电与电池","均衡充电",
        "发电机","BMS通信","系统功能","并机设置","双路输出"};
    return group<PANEL_SETTING_GROUPS?names[group]:"";
}
unsigned panel_setting_group_for(uint16_t a)
{
    if(a<=0x4104 || a==0x410e || a==0x410f)return 1;
    if(a<=0x410d || (a>=0x4120 && a<=0x4122) || a==0x4124 || a==0x4142 ||
       a==0x4166 || a==0x418d)return 2;
    if(a==0x4110 || a==0x4111 || a==0x4153)return 7;
    if(a<=0x4117)return 3;
    if(a==0x4118 || a==0x4119 || a==0x414b)return 8;
    if(a<=0x411d)return 4;
    if(a<=0x411f)return PANEL_SETTING_GROUP_BMS;
    return 6;
}
unsigned panel_setting_group_count(unsigned group)
{
    unsigned n=0;
    if(group>=PANEL_SETTING_GROUPS)return 0;
    for(unsigned i=0;i<panel_setting_count();i++){
        uint16_t address=settings[i].address;
        if(!group || panel_setting_group_for(address)==group)n++;
    }
    return n;
}
unsigned panel_setting_group_index(unsigned group,unsigned position)
{
    if(group<PANEL_SETTING_GROUPS)for(unsigned i=0;i<panel_setting_count();i++){
        uint16_t address=settings[i].address;
        if((!group || panel_setting_group_for(address)==group) && position--==0)return i;
    }
    return (unsigned)panel_setting_count();
}
unsigned panel_setting_group_position(unsigned group,unsigned index)
{
    for(unsigned p=0;p<panel_setting_group_count(group);p++)
        if(panel_setting_group_index(group,p)==index)return p;
    return 0;
}
static bool profile_conflict(const panel_snapshot_t *s)
{
    return (s->profile_locked || panel_is_3ku(s)) &&
           s->config.profile_override!=PANEL_PROFILE_UNKNOWN && s->detected_profile!=PANEL_PROFILE_UNKNOWN &&
           s->detected_profile!=s->config.profile_override &&
           panel_fresh(s,0x38) && panel_fresh(s,0x39);
}

static bool unsupported_profile(const panel_snapshot_t *s)
{
    return (s->config.profile_override!=PANEL_PROFILE_UNKNOWN &&
            !panel_profile_supported(s->config.profile_override)) ||
           (s->detected_profile!=PANEL_PROFILE_UNKNOWN && !panel_profile_supported(s->detected_profile));
}

static bool output_state_3ku(uint16_t address,uint16_t state)
{
    if(address==0x4100)return state==2 || state==3 || state==5;
    if(address==0x4101)return state==2 || state==3;
    return true;
}

static bool fixed_3ku(uint16_t address)
{
    return address==0x4110 || address==0x4111 || address==0x411b || address==0x411c ||
           address==0x411d || address==0x411f;
}

panel_setting_access_t panel_setting_access(const panel_snapshot_t *s,uint16_t address)
{
    uint16_t state=0,next;
    if(!s || !panel_online(s))return PANEL_ACCESS_OFFLINE;
    if(!panel_value(s,0x42,&state) || state<2 || state>8 || state==7)
        return PANEL_ACCESS_STATE;
    if(!panel_setting_find(address) || unsupported_profile(s))return PANEL_ACCESS_UNSUPPORTED;
    if(panel_is_3ku(s)){
        if(profile_conflict(s))return PANEL_ACCESS_PROFILE_CONFLICT;
        if(s->config.profile_override!=PANEL_PROFILE_3KU)return PANEL_ACCESS_MODEL;
        if(fixed_3ku(address))return PANEL_ACCESS_READ_ONLY;
        if(!output_state_3ku(address,state))return PANEL_ACCESS_STATE;
    }
    if(panel_setting_allowed(s,address,0) || panel_setting_step(s,address,0,1,&next))return PANEL_ACCESS_EDITABLE;
    if(profile_conflict(s))return PANEL_ACCESS_PROFILE_CONFLICT;
    if(s->config.profile_override==PANEL_PROFILE_UNKNOWN)return PANEL_ACCESS_MODEL;
    if((address==0x4110 || address==0x4111 || address==0x4153) &&
       s->config.profile_override==PANEL_PROFILE_12KU && state!=2)return PANEL_ACCESS_STANDBY;
    bool timed_equalization=panel_is_3ku(s) && address>=0x4114 && address<=0x4116;
    bool battery=(address>=0x4107 && address<=0x410d) || address==0x4112 || address==0x4113 ||
        timed_equalization || address==0x4117 || address==0x4118;
    if(battery && !panel_fresh(s,0x150))return PANEL_ACCESS_BATTERY;
    if(address==0x410e || address==0x410f){
        uint16_t mode;
        if(!panel_fresh(s,0x140) || !panel_value(s,0x140,&mode))return PANEL_ACCESS_MODE;
        if(address==0x410f && mode==1)return PANEL_ACCESS_MODE_FIXED;
    }
    uint16_t type;
    if(battery && panel_value(s,0x150,&type)){
        if(address>=0x4108 && address<=0x410d && type==3 && panel_is_3ku(s))return PANEL_ACCESS_DEFINITION;
        if(address>=0x4108 && address<=0x410b && type<=1)return PANEL_ACCESS_FIXED;
        if((address==0x4112 || address==0x4113 || timed_equalization || address==0x4117) &&
           type>1)return PANEL_ACCESS_LEAD_ONLY;
    }
    return PANEL_ACCESS_UNSUPPORTED;
}
const char *panel_setting_access_reason(panel_setting_access_t access)
{
    static const char *const reasons[]={"","逆变器无有效应答","当前运行状态禁止设置",
        "请先确认实际机型","缺少最新电池类型数据","并机设置仅可在待机时修改",
        "此电池类型使用固定参数","均衡充电仅适用于铅酸电池",
        "此机型电池定义尚未确认","此机型不支持这一设置","逆变器机型与选定配置不符",
        "此机型仅可读取","缺少最新运行模式数据","当前模式使用固定值"};
    return reasons[access];
}
const char *panel_setting_restriction(const panel_snapshot_t *s,uint16_t address)
{
    panel_setting_access_t access=panel_setting_access(s,address);
    uint16_t state;
    if(access==PANEL_ACCESS_STATE && panel_is_3ku(s) &&
       panel_value(s,0x42,&state) && state>=2 && state<=6){
        if(address==0x4101)return "仅可在待机或市电模式修改";
        if(address==0x4100)return "仅可在待机、市电或电池模式修改";
    }
    return panel_setting_access_reason(access);
}
bool panel_setting_decode_readback(uint16_t address,uint16_t raw,uint16_t *value)
{
    if(!value || raw==0xffff)return false;
    int bit=-1;
    switch(address){
    case 0x4100:{
        /* WF_INV names 02A0 as the voltage setpoint but mislabels its unit
         * as Hz. Accept only the enumerated voltages in 0.1V representation;
         * an unknown representation must never confirm a write. */
        static const uint16_t volts10[]={2080,2200,2300,2400,1000,1100,1200,1270};
        for(unsigned i=0;i<sizeof(volts10)/sizeof(volts10[0]);i++)
            if(raw==volts10[i]){*value=i%4;return true;}
        return false;
    }
    case 0x4114:case 0x4119:
        if(raw>15)return false; /* WF_BAT hours -> WF_UserSet minutes. */
        *value=raw*60U;return true;
    case 0x4116:
        if(raw<24 || raw>2160 || raw%24)return false;
        *value=raw/24U;return true;
    case 0x4101:
        if(raw!=5000 && raw!=6000)return false;
        *value=raw==6000;return true;
    case 0x4112:case 0x411a:bit=8;break;
    case 0x4125:case 0x412d:bit=7;break;
    case 0x4127:bit=1;break;case 0x4128:bit=2;break;
    case 0x4129:bit=3;break;case 0x412a:bit=4;break;
    case 0x412b:bit=5;break;case 0x412c:bit=6;break;
    case 0x4130:bit=11;break;case 0x4131:bit=13;break;
    case 0x4132:bit=14;break;
    default:break;
    }
    *value=bit>=0?(raw>>bit)&1U:raw;return true;
}
const panel_setting_t *panel_setting(size_t index)
{
    return index < panel_setting_count() ? &settings[index] : NULL;
}

bool panel_setting_decode_current(const panel_snapshot_t *s,uint16_t address,uint16_t raw,uint16_t *value)
{
    if(!s || !value)return false;
    uint16_t flags_reg=panel_setting_current_flags(address);
    if(flags_reg!=0xffff){
        uint16_t flags;
        if(!panel_fresh(s,flags_reg) || !panel_value(s,flags_reg,&flags))return false;
        unsigned bit=address==0x4120?2U:address==0x4121?1U:0U;
        if(!(flags&(1U<<bit))){*value=0xfff1;return true;}
        unsigned low=address==0x4120?5U:10U;
        unsigned high=address==0x4120?50U:address==0x4121?100U:90U;
        if(raw<low || raw>high)return false;
        *value=raw;return true;
    }
    if(panel_is_3ku(s)){
        switch(address){
        case 0x4102:case 0x4103: if(raw>2)return false;break;
        case 0x4104: if(raw>2)return false;break; /* WF_WorkMode lists only 0–2. */
        case 0x4107: if(raw>4)return false;break;
        case 0x4106:
            if(raw==200){*value=0;return true;}
            if(raw<1000 || raw>10000 || raw%1000U)return false;
            *value=raw/1000U;return true;
        case 0x4115:
            if(raw<1 || raw>15)return false;
            *value=raw*60U;return true;
        case 0x4116:
            if(raw<1 || raw>90)return false;
            *value=raw;return true;
        case 0x4119:
            /* The 3KU table disagrees on minutes versus hours for finite values. */
            if(raw!=0 && raw!=0xffff)return false;
            *value=raw==0?0xfff1:0xfff2;return true;
        case 0x411a:
            if(raw==0xffff)return false;
            *value=(raw&(1U<<8))==0;return true;
        case 0x411e:
            if(raw!=0 && (raw<2 || raw>5))return false;
            *value=raw;return true;
        default:break;
        }
    }
    if(address==0x4100){
        if(!panel_profile_supported(s->config.profile_override) || raw<2000)return false;
    }
    return panel_setting_decode_readback(address,raw,value);
}
const panel_setting_t *panel_setting_find(uint16_t address)
{
    for (size_t i = 0; i < panel_setting_count(); ++i)
        if (settings[i].address == address) return &settings[i];
    return NULL;
}

uint16_t panel_setting_current_address(const panel_snapshot_t *s,const panel_setting_t *setting)
{
    if(!s || !setting)return 0xffff;
    if(panel_is_3ku(s)){
        if(setting->address==0x4106)return 0x0179;
        if(setting->address==0x411e)return 0x0190;
    }
    /* WF_BAT's generic 0..4 enum cannot identify PWR4KL's 0..8 setting. */
    if(setting->address==0x4107 && (s->config.profile_override==PANEL_PROFILE_4KL ||
                                  s->detected_profile==PANEL_PROFILE_4KL))return 0xffff;
    return setting->read_address;
}

uint16_t panel_setting_current_flags(uint16_t address)
{
    return address>=0x4120 && address<=0x4122?0x0350:0xffff;
}

const char *panel_profile_name(panel_profile_t profile)
{
    static const char *const names[] = {
        "未知/自动", "6KU", "不支持", "不支持", "12KU", "2KU", "不支持", "3KU"
    };
    return (unsigned)profile < PANEL_PROFILE_COUNT ? names[profile] : names[0];
}

static panel_profile_t profile_code(uint16_t code)
{
    switch (code) {
    case 0x10: return PANEL_PROFILE_6KU;
    case 0x11: return PANEL_PROFILE_6KU_PARALLEL;
    case 0x20: return PANEL_PROFILE_5KU;
    case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45:
        return PANEL_PROFILE_12KU;
    case 0x60: return PANEL_PROFILE_2KU;
    case 0x50: return PANEL_PROFILE_3KU;
    case 0x70: return PANEL_PROFILE_4KL;
    default: return PANEL_PROFILE_UNKNOWN;
    }
}

panel_profile_t panel_detect_profile(uint16_t id38, uint16_t id39)
{
    /* Table identifies 0039; revision history moves the code into 0038. Only
     * unambiguous known, full-word codes are hints. Never enable writes here. */
    panel_profile_t a = profile_code(id38), b = profile_code(id39);
    if (a && b && a != b) return PANEL_PROFILE_UNKNOWN;
    if (a && (id39 == 0 || b == a)) return a;
    if (b && (id38 == 0 || id38 == 1 || a == b)) return b;
    return PANEL_PROFILE_UNKNOWN;
}

static bool between(uint16_t value, unsigned low, unsigned high)
{
    return value >= low && value <= high;
}

static bool voltage_allowed(panel_profile_t profile, uint16_t battery,
                            uint16_t address, uint16_t value)
{
    if (battery > 4 ||
        (profile==PANEL_PROFILE_3KU && battery==3 && address!=0x4118)) return false;
    unsigned factor = profile == PANEL_PROFILE_2KU ? 1U :
                      profile == PANEL_PROFILE_3KU ? 2U : 4U;
    unsigned low = 0, high = 0;
    bool lead = battery <= 1, custom = battery == 4;
    switch (address) {
    case 0x4108:
        if (lead) return false; /* Fixed points are not editable. */
        low = custom ? 105 : 103;
        high = custom ? 135 : 125;
        break;
    case 0x4109:
        if (lead) return false;
        low = 100; high = 120;
        break;
    case 0x410a:
        if (lead) return false;
        low = 120; high = 150;
        break;
    case 0x410b:
        if (lead) return false;
        low = custom ? 120 : 125;
        high = 150;
        /* 6KU single differs from 6KU parallel/12KU. */
        if (profile == PANEL_PROFILE_6KU && !custom) high = 145;
        if (profile == PANEL_PROFILE_12KU && !custom) low = 120;
        break;
    case 0x410c:
        low = lead ? 110 : 100;
        high = lead ? 130 : 125;
        break;
    case 0x410d:
        low = lead ? 120 : 115;
        high = 145;
        break;
    case 0x4118:
        low = 110; high = 150;
        break;
    default: return false;
    }
    return between(value, low * factor, high * factor);
}

bool panel_setting_allowed(const panel_snapshot_t *s, uint16_t address, uint16_t value)
{
    uint16_t state;
    if (!s || !panel_setting_find(address) || value == 0xffff || unsupported_profile(s) ||
        !panel_online(s) || !panel_fresh(s,0x42) || !panel_value(s,0x42,&state) ||
        state < 2 || state > 8 || state == 7) return false;
    panel_profile_t profile = s->config.profile_override;
    bool known = panel_profile_supported(profile) && !profile_conflict(s);
    if(panel_is_3ku(s)){
        if(!known || profile!=PANEL_PROFILE_3KU || fixed_3ku(address) ||
           !output_state_3ku(address,state))return false;
        uint16_t battery;
        switch(address){
        case 0x4102:return value<=2;
        case 0x4104:return value<=3;
        case 0x4105:return between(value,2,40); /* Common range of the documented 40A/60A variants. */
        case 0x4106:return value<=10;
        case 0x4114:case 0x4115:case 0x4116:
            if(!panel_fresh(s,0x150) || !panel_value(s,0x150,&battery) || battery>1)return false;
            if(address==0x4116)return between(value,1,90);
            return between(value,5,900) && value%5U==0;
        case 0x4119:return between(value,1,894) || value==0xfff1 || value==0xfff2;
        case 0x411e:return value==0 || between(value,2,5);
        case 0x4120:case 0x4121:case 0x4122:
            if(value==0xfff1)return true;
            if(!panel_fresh(s,0x150) || !panel_value(s,0x150,&battery) || battery!=2)return false;
            break;
        case 0x4123:return value==0 || (value==1 && state==2) || (value==2 && between(state,3,6));
        case 0x4124:return value<=1;
        case 0x414b:return false;
        case 0x41a6:return value<=1;
        default:break;
        }
    }
    switch (address) {
    case 0x4125:case 0x4127:case 0x4128:case 0x4129:case 0x412a:case 0x412b:
    case 0x412c:case 0x412d:case 0x4130:case 0x4131:case 0x4132:return value<=1;
    case 0x4114:return value==0 || (between(value,5,900) && value%5==0);
    case 0x4115:return between(value,5,900) && value%5==0;
    case 0x4116:return between(value,1,90);
    case 0x414b:return between(value,5,90) || value==0xfff1;
    case 0x4101: return value <= 1;
    case 0x4103: case 0x4104: case 0x4123: return value <= 2;
    case 0x411e: return value <= 6;
    case 0x4119: return between(value,5,900) || value == 0xfff1 || value == 0xfff2;
    case 0x4120: return between(value,5,50) || value == 0xfff1;
    case 0x4121: return between(value,10,100) || value == 0xfff1;
    case 0x4122: return between(value,10,90) || value == 0xfff1;
    case 0x411f:
        return value <= 15 || value == 0xfff1 ||
               (known && profile != PANEL_PROFILE_12KU && value == 0xfff2);
    default: break;
    }
    if (!known) return false;
    switch (address) {
    case 0x410e:case 0x410f:{
        uint16_t mode;
        if(!panel_fresh(s,0x140) || !panel_value(s,0x140,&mode) ||
           mode>2 || (profile==PANEL_PROFILE_3KU && mode==2))return false;
        if(address==0x410e)return mode==1?between(value,170,200):between(value,90,154);
        return mode!=1 && between(value,264,280);
    }
    case 0x4142:{
        /* V1.27 raw maximum per model; each step adds 5 A from 10 A. */
        static const uint8_t limits[PANEL_PROFILE_COUNT]={
            [PANEL_PROFILE_6KU]=24,[PANEL_PROFILE_12KU]=42,[PANEL_PROFILE_2KU]=28,
        };
        return profile!=PANEL_PROFILE_3KU && (value<=limits[profile] || value==0xfff1);
    }
    case 0x4153:return state==2 && profile==PANEL_PROFILE_12KU && value<=1;
    case 0x4166:return profile==PANEL_PROFILE_2KU && value<=1;
    case 0x418d:return profile==PANEL_PROFILE_2KU &&
        (between(value,5,55) || value==0x8001);
    case 0x4110:return state==2 && profile==PANEL_PROFILE_12KU && value<=4;
    case 0x4111:return state==2 && profile==PANEL_PROFILE_12KU && (value==0xfff1 || value<=5);
    case 0x411a:case 0x411d:return value<=1;
    case 0x411b:return value<=120;
    case 0x411c:return between(value,5,120);
    case 0x4100: return value <= 3 && (profile != PANEL_PROFILE_12KU || value != 0);
    case 0x4102: return value <= 3;
    case 0x4105: {
        unsigned limit = profile == PANEL_PROFILE_12KU ? 160 : profile == PANEL_PROFILE_2KU ? 100 : 120;
        return value == 2 || (value >= 10 && value <= limit && value % 10U == 0);
    }
    case 0x4106: {
        unsigned limit = profile == PANEL_PROFILE_12KU ? 16 : profile == PANEL_PROFILE_2KU ? 10 : 12;
        return value <= limit && (value != 0 || profile != PANEL_PROFILE_12KU);
    }
    case 0x4107:
        return panel_fresh(s,0x150) && value <= 4;
    default: break;
    }
    uint16_t battery;
    if (!panel_fresh(s,0x150) || !panel_value(s,0x150,&battery)) return false;
    if (address == 0x4113) {
        unsigned factor=profile==PANEL_PROFILE_2KU?1U:profile==PANEL_PROFILE_3KU?2U:4U;
        return battery<=1 && between(value,120*factor,150*factor);
    }
    if (address == 0x4112 || address==0x4117)
        return value == 0 || (value == 1 && battery <= 1);
    return voltage_allowed(profile,battery,address,value);
}

bool panel_setting_step(const panel_snapshot_t *s, uint16_t address, uint16_t current,
                        int direction, uint16_t *next)
{
    if (!next || !direction || !panel_setting_find(address)) return false;
    /* Jump directly to OFF/AUTO/FULL codes after the ordinary range. */
    const int32_t maximum=900;
    const uint16_t off=address==0x418d?0x8001:0xfff1;
    bool valid = panel_setting_allowed(s,address,current);
    if (direction < 0) {
        if ((!valid || current > 0xfff2) && panel_setting_allowed(s,address,0xfff2)) {
            *next = 0xfff2;
            return true;
        }
        if ((!valid || current > off) && panel_setting_allowed(s,address,off)) {
            *next = off;
            return true;
        }
    }
    int32_t value = valid ? (int32_t)current + (direction > 0 ? 1 : -1) :
                           direction > 0 ? 0 : maximum;
    if (direction < 0 && value > maximum) value = maximum;
    for (; value >= 0 && value <= maximum; value += direction > 0 ? 1 : -1) {
        if (panel_setting_allowed(s,address,(uint16_t)value)) {
            *next = (uint16_t)value;
            return true;
        }
    }
    if (direction > 0) {
        if ((!valid || current < off) && panel_setting_allowed(s,address,off)) {
            *next = off;
            return true;
        }
        if ((!valid || current < 0xfff2) && panel_setting_allowed(s,address,0xfff2)) {
            *next = 0xfff2;
            return true;
        }
    }
    return false;
}

void panel_setting_format_profile(panel_profile_t profile,uint16_t address,
                                  uint16_t raw,char *out,size_t size)
{
    if (!out || !size) return;
    const panel_setting_t *setting = panel_setting_find(address);
    if (!setting || raw == 0xffff) { snprintf(out,size,"--"); return; }
    if(address==0x418d && raw==0x8001){
        snprintf(out,size,"关闭");return;
    }
    if (raw == 0xfff1 && (address == 0x411f || address == 0x4119 ||
        (address >= 0x4120 && address <= 0x4122) || address==0x4142 || address==0x414b || address==0x4111)) {
        snprintf(out,size,"%s",address == 0x411f || address==0x4111 ? "自动" : "关闭");
        return;
    }
    if (raw == 0xfff2 && (address == 0x411f || address == 0x4119)) {
        snprintf(out,size,"%s",address == 0x411f ? "循环" : "不限时间");
        return;
    }
    if (address == 0x4100 && raw <= 3) {
        static const unsigned standard[] = {208,220,230,240};
        if (!panel_profile_supported(profile))
            snprintf(out,size,"机型待确认 (%u)",raw);
        else snprintf(out,size,"%u V",standard[raw]);
    } else if (address == 0x4101 && raw <= 1) snprintf(out,size,"%u Hz",raw ? 60 : 50);
    else if (address == 0x4102 && raw <= 3) {
        static const char *const names[] = {"GPB","PGB","PBG","MKS"};
        static const char *const three_ku_names[] = {"GRD","PV","PBG","MKS"};
        snprintf(out,size,"%s",profile==PANEL_PROFILE_3KU?three_ku_names[raw]:names[raw]);
    } else if (address == 0x4103 && raw <= 2) {
        static const char *const names[] = {"APP","UPS","GEN"};
        snprintf(out,size,"%s",names[raw]);
    } else if (address == 0x4104 && (raw <= 2 || (raw==3 && profile==PANEL_PROFILE_3KU))) {
        static const char *const names[] = {"PNG","OPV","PVF","GRD"};
        snprintf(out,size,"%s",names[raw]);
    } else if (address == 0x4106 && raw <= 16) snprintf(out,size,"%u A",raw ? raw * 10U : 2U);
    else if (address == 0x4123 && raw <= 2) {
        static const char *const names[] = {"停用远程开关机","开启逆变器","关闭逆变器"};
        snprintf(out,size,"%s",names[raw]);
    } else if (address == 0x4107) {
        static const char *const standard[]={"铅酸","注水铅酸","三元锂","磷酸铁锂","自定义"};
        if(profile==PANEL_PROFILE_3KU && raw==2)snprintf(out,size,"锂电池");
        else if(profile==PANEL_PROFILE_3KU && raw==3)snprintf(out,size,"FEL (3)");
        else if(raw<5)snprintf(out,size,"%s",standard[raw]);
        else snprintf(out,size,"未知类型 %u",raw);
    } else if(address==0x411e && raw<=6){
        static const char *const names[]={"关闭","CVTE","古瑞瓦特 GRO","日月元 VOL","派能 PYL","铁塔 IRF","沛城 PAC"};
        snprintf(out,size,"%s",profile==PANEL_PROFILE_3KU && raw==5?"IRON":names[raw]);
    } else if((address==0x4124 || (address==0x411a && profile==PANEL_PROFILE_3KU)) && raw<=1){
        snprintf(out,size,"%s",raw?"关闭":"开启");
    } else if(address==0x4142 && raw<=42){
        snprintf(out,size,"%u A",10U+5U*raw);
    } else if(address==0x41a6 && raw<=1){
        snprintf(out,size,"%s",raw?"充电优先":"标准");
    } else if ((address==0x4112 || address==0x4117 || address==0x411a || address==0x411d ||
                address==0x4125 || (address>=0x4127 && address<=0x412d) ||
                address==0x4130 || address==0x4131 || address==0x4132 || address==0x4153 ||
                address==0x4166) && raw<=1)
        snprintf(out,size,"%s",raw?"开启":"关闭");
    else if ((address >= 0x4100 && address <= 0x4104) || address == 0x4106 ||
             address == 0x4112 || address == 0x4123) snprintf(out,size,"code %u",raw);
    else if (setting->divisor == 10) snprintf(out,size,"%u.%u %s",raw / 10U,raw % 10U,setting->unit);
    else snprintf(out,size,"%u%s%s",raw,*setting->unit ? " " : "",setting->unit);
}
void panel_setting_format(const panel_snapshot_t *s,uint16_t address,
                          uint16_t raw,char *out,size_t size)
{panel_setting_format_profile(panel_is_3ku(s)?PANEL_PROFILE_3KU:
                              s?s->config.profile_override:PANEL_PROFILE_UNKNOWN,address,raw,out,size);}
