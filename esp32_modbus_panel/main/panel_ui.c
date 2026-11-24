#include "panel_ui.h"
#include "panel_key_labels.h"
#include "panel_catalog.h"
#include "direct_key_codec.h"
#include "direct_gfx.h"
#include "panel_locale.h"
#include "panel_theme.h"
#include <stdio.h>
#include <string.h>

static const char *const menus[]={"运行参数","故障告警","参数设置","开关机","通信配置","通信诊断"};
static const uint32_t bauds[]={1200,2400,4800,9600,19200,38400,57600,115200};
static const char *const states[]={"上电","初始化","待机","市电","光伏","电池","发电机","故障","关机","测试","升级"};
static const char *const detail_titles[]={
    "市电与负载","电池与充电","光伏输入","电池状态","温度监测","发电机","电量与输出","当前运行设置"
};
enum {SETTINGS_PAGE_ROWS=5};

const char *panel_detail_group_label(unsigned group,bool english)
{
    static const char *const en[]={"Grid & load","Battery","Solar","Battery status",
        "Temperatures","Generator","Energy","Modes"};
    return group<PANEL_DETAIL_PAGES?(english?en[group]:detail_titles[group]):"";
}

static const char *run_state(const panel_snapshot_t *s)
{
    uint16_t value;
    return panel_value(s,0x42,&value) && value<sizeof(states)/sizeof(states[0]) ? states[value]:"未知";
}

static const char *result_state(panel_write_state_t state)
{
    switch(state){
    case PANEL_WRITE_QUEUED:return "命令排队中";
    case PANEL_WRITE_SENDING:return "正在发送，请等待";
    case PANEL_WRITE_ACK:return "已应答，等待处理";
    case PANEL_WRITE_PROTOCOL_ACCEPTED:return "设备已受理";
    case PANEL_WRITE_PROCESSED:return "设置未生效";
    case PANEL_WRITE_CONFIRMED:return "设置已生效";
    case PANEL_WRITE_UNCERTAIN:return "设置结果未确认";
    case PANEL_WRITE_REJECTED:return "命令失败，查看诊断";
    default:return "";
    }
}

void panel_ui_init(panel_ui_t *u)
{
    if(u)memset(u,0,sizeof(*u));
}

void panel_ui_sync(panel_ui_t *u,const panel_snapshot_t *s)
{
    if(!u || !s)return;
    const panel_result_t *receipt=u->pending?panel_result_find(s,u->pending_sequence):NULL;
    bool own_config=u->pending && u->pending_type==PANEL_CMD_CONFIG && receipt && receipt->state!=PANEL_WRITE_REJECTED;
    if(u->context_seen && u->context_generation!=s->context_generation && !own_config){
        bool uncertain=u->pending && u->pending_type==PANEL_CMD_WRITE;
        if(u->model_setup){u->page=PANEL_PAGE_SETTINGS;u->selected=u->model_return_selected;}
        u->pending=false;u->editing=false;u->review=false;u->blocked=false;u->model_setup=false;
        u->have_result=uncertain;u->last_result=PANEL_WRITE_UNCERTAIN;u->result_exception=0;u->result_readback_valid=false;
        u->edit_original_valid=false;
        u->had_link=false;u->link_lost=false;u->show_recovery=false;
        snprintf(u->message,sizeof(u->message),"通信配置已变更，请重新打开参数");
        u->context_generation=s->context_generation;return;
    }
    u->context_seen=true;u->context_generation=s->context_generation;
    if(u->show_recovery && s->now_ms-u->recovered_ms>=3000U)u->show_recovery=false;
    if(!s->scanning){
        bool online=panel_online(s);
        if(online){
            u->had_link=true;
            if(u->link_lost){
                u->link_lost=false;u->show_recovery=true;u->recovered_ms=s->now_ms;
                if(!strcmp(u->message,"通信中断，本次修改已取消"))u->message[0]=0;
            }
            if(u->blocked && u->page==PANEL_PAGE_SETTINGS){
                const panel_setting_t *setting=panel_setting(u->selected);
                if(setting && panel_setting_access(s,setting->address)==PANEL_ACCESS_EDITABLE){
                    u->blocked=false;
                    u->message[0]=0;
                }
            }
        }else if(u->had_link){
            u->link_lost=true;u->show_recovery=false;
        }
        if(!online && !u->pending && (u->editing || u->review) &&
           (u->page==PANEL_PAGE_SETTINGS || u->page==PANEL_PAGE_POWER)){
            u->editing=false;u->review=false;u->edit_original_valid=false;
            snprintf(u->message,sizeof(u->message),"通信中断，本次修改已取消");
        }
    }
    /* A bounded completion history may have been consumed while this page was
     * covered by another screen. Lost receipts never mean success or retry. */
    if(u->pending && !receipt && !s->command_busy && s->now_ms-u->pending_since_ms>1000U){
        u->pending=false;u->editing=false;u->review=false;
        if(u->pending_type==PANEL_CMD_WRITE){
            u->have_result=true;u->last_result=PANEL_WRITE_UNCERTAIN;u->result_exception=0;u->result_readback_valid=false;
        }else snprintf(u->message,sizeof(u->message),"未取得操作回执，请核对当前配置");
        return;
    }
    if(own_config && u->context_generation!=s->context_generation){
        u->had_link=false;u->link_lost=false;u->show_recovery=false;
    }
    if(u->pending && receipt){
        u->pending=false;u->review=false;u->editing=false;u->message[0]=0;
        panel_write_state_t state=receipt->state;
        if(u->pending_type==PANEL_CMD_CONFIG)
            snprintf(u->message,sizeof(u->message),"%s",state==PANEL_WRITE_REJECTED?"配置保存失败，查看诊断":"本机通信配置已保存");
        else if(u->pending_type==PANEL_CMD_SCAN || u->pending_type==PANEL_CMD_SCAN_CANCEL){
            uint8_t found=receipt->scan_found;
            if(receipt->state==PANEL_WRITE_REJECTED)snprintf(u->message,sizeof(u->message),"搜索中断，请重试");
            else if(found)snprintf(u->message,sizeof(u->message),"搜索结束，找到地址%u",found);
            else if(u->pending_type==PANEL_CMD_SCAN_CANCEL)snprintf(u->message,sizeof(u->message),"已停止搜索");
            else snprintf(u->message,sizeof(u->message),"搜索结束，未找到从机");
        }else{
            u->have_result=true;u->last_result=state;
            u->result_exception=receipt->exception;
            u->result_readback=receipt->readback_value;
            u->result_readback_valid=receipt->readback_valid;
            snprintf(u->message,sizeof(u->message),"%s",result_state(state));
        }
        if(u->pending_type==PANEL_CMD_CONFIG && u->model_setup && state!=PANEL_WRITE_REJECTED){
            u->model_setup=false;u->page=PANEL_PAGE_SETTINGS;
            u->selected=u->model_return_selected;u->blocked=false;
            snprintf(u->message,sizeof(u->message),"机型已保存，请重新选择参数");
        }
    }
}
void panel_ui_submitted(panel_ui_t *u,uint32_t sequence,panel_command_type_t type)
{
    if(u && u->pending){
        u->pending_sequence=sequence;
        u->pending_type=type;
    }
}

void panel_ui_submit_failed(panel_ui_t *u,panel_command_type_t type)
{
    if(!u)return;
    /* The scan may have just completed, or the bus may still be busy.
     * Keep its original sequence so the result can still be displayed. */
    if(type==PANEL_CMD_SCAN_CANCEL)return;
    u->pending=false;u->review=false;u->editing=false;u->have_result=false;
    snprintf(u->message,sizeof(u->message),"请求未提交，请重试");
}

static bool send_command(panel_ui_t *u,const panel_snapshot_t *s,panel_command_t *c)
{
    u->pending=true;u->have_result=false;u->pending_sequence=s->command_sequence+1U;
    u->pending_since_ms=s->now_ms;u->context_seen=true;u->context_generation=s->context_generation;
    u->pending_type=c->type;
    if(c->type==PANEL_CMD_WRITE){u->result_address=c->address;u->result_value=c->value;u->result_profile=panel_effective_profile(s);}
    u->editing=false;u->review=false;u->message[0]=0;
    return true;
}

static unsigned move(unsigned selected,unsigned count,int direction)
{
    if(!count)return 0;
    return direction>0?(selected+1U)%count:(selected?selected-1U:count-1U);
}

static unsigned step_page(unsigned selected,unsigned count,int direction)
{
    if(selected>=count)selected=count-1;
    return direction>0?(selected+1<count?selected+1:selected):(selected?selected-1:0);
}

static unsigned move_grid(unsigned selected,unsigned count,unsigned columns,uint8_t key)
{
    unsigned column=selected%columns;
    if(key==DIRECT_KEY_LEFT && column>0)return selected-1;
    if(key==DIRECT_KEY_UP && selected>=columns)return selected-columns;
    if(key==DIRECT_KEY_DOWN && selected+columns<count)return selected+columns;
    if(key==DIRECT_KEY_RIGHT && column+1<columns && selected+1<count)return selected+1;
    return selected;
}

static uint16_t power_request(unsigned selected)
{
    return selected==0?1:selected==1?2:0;
}

static unsigned move_page(unsigned selected,unsigned count,unsigned rows,int direction)
{
    unsigned page=move(selected/rows,(count+rows-1)/rows,direction);
    unsigned next=page*rows+selected%rows;
    return next<count?next:count-1;
}

static unsigned alarm_pages(const panel_snapshot_t *s)
{
    unsigned count=0;
    for(unsigned reg=0x46;reg<=0x49;reg++){
        uint16_t bits=0;panel_value(s,(uint16_t)reg,&bits);
        while(bits){count+=bits&1U;bits>>=1;}
    }
    return count?(count+4U)/5U:1U;
}

static bool current_setting(const panel_snapshot_t *s,const panel_setting_t *setting,uint16_t *value)
{
    uint16_t address=panel_setting_current_address(s,setting);
    if(address==0xffff)return false;
    uint16_t raw;
    return panel_value(s,address,&raw) &&
           panel_setting_decode_current(s,setting->address,raw,value);
}

static bool displayed_setting(const panel_snapshot_t *s,const panel_setting_t *setting,uint16_t *value,bool *verified,bool *stale)
{
    *verified=true;*stale=!panel_online(s);
    const panel_setting_read_t *r=setting?panel_setting_read_find(s,setting->address):NULL;
    if(r && r->state!=PANEL_SETTING_UNREAD){
        if(r->state!=PANEL_SETTING_VALUE)return false;
        *value=r->value;*verified=r->independent;return true;
    }
    if(!current_setting(s,setting,value))return false;
    *stale|=!panel_fresh(s,panel_setting_current_address(s,setting));
    return true;
}

static const char *setting_read_status(const panel_snapshot_t *s,uint16_t address,bool short_text)
{
    if(!panel_online(s))return short_text?"已断联":"通信已断开";
    const panel_setting_read_t *r=panel_setting_read_find(s,address);
    if(!r || r->state==PANEL_SETTING_UNREAD)return "正在读取";
    if(r->state==PANEL_SETTING_EMPTY)return short_text?"未返回":"设备未返回当前值";
    if(r->state==PANEL_SETTING_UNSUPPORTED)return short_text?"不支持":"设备不支持读取";
    if(r->state==PANEL_SETTING_TIMEOUT)return "读取超时";
    return "设备读取失败";
}
static void setting_read_flags(panel_view_t *v,const panel_snapshot_t *s,uint16_t address,unsigned row)
{
    v->unread_rows|=(uint8_t)(1U<<row);
    const panel_setting_read_t *r=panel_setting_read_find(s,address);
    if(r && r->state>=PANEL_SETTING_TIMEOUT)v->read_error_rows|=(uint8_t)(1U<<row);
}

static bool setting_uses_fast_step(const panel_setting_t *setting)
{
    uint16_t address=setting->address;
    return setting->divisor==10 || address==0x4105 || address==0x4106 ||
           address==0x410e || address==0x410f || address==0x4142 ||
           (address>=0x4114 && address<=0x4116) ||
           (address>=0x4120 && address<=0x4122) || address==0x414b || address==0x418d;
}

static const char *setting_help(uint16_t address)
{
    switch(address){
    case 0x4104:return "设置充电来源与优先级";
    case 0x4105:return "限制市电充电电流";
    case 0x4106:return "限制光伏和市电的总充电电流";
    case 0x410a:return "恒压充电阶段的目标电压";
    case 0x410b:return "充电完成后的维持电压";
    case 0x4112:return "均衡充电仅适用于铅酸电池";
    case 0x4117:return "立即执行一次铅酸均衡充电";
    case 0x4120:return "电池电量低于此值时关机";
    case 0x4121:return "电量回升到此值时转电池供电";
    case 0x4122:return "电量降到此值时转市电供电";
    case 0x4124:return "关闭后停止电池充电";
    case 0x4100:return "设置交流输出的额定电压";
    case 0x4101:return "设置交流输出的额定频率";
    case 0x4102:return "选择负载优先使用的电源";
    case 0x4103:return "选择逆变器接受的输入电压范围";
    case 0x4107:return "按电池类型选择充电参数";
    case 0x4108:return "电池电压低于此值时告警";
    case 0x4109:return "电池电压低于此值时关机";
    case 0x410c:return "电池电压降到此值时转市电";
    case 0x410d:return "电池电压回升到此值时转电池";
    case 0x410e:return "市电电压低于此值时退出市电";
    case 0x410f:return "市电电压高于此值时退出市电";
    case 0x4110:return "选择单机或并机相位";
    case 0x4111:return "为并机中的逆变器设置编号";
    case 0x4113:return "设置均衡充电的目标电压";
    case 0x4114:return "设置均衡充电持续时间";
    case 0x4115:return "设置均衡充电最长时间";
    case 0x4116:return "设置均衡充电间隔天数";
    case 0x4118:return "电压降到此值时关闭输出 2";
    case 0x4119:return "设置输出 2 的供电时间";
    case 0x411a:return "控制是否允许接入发电机";
    case 0x411b:return "按发电机铭牌设置额定功率";
    case 0x411c:return "设置发电机输入功率上限";
    case 0x411d:return "消除当前的发电机告警";
    case 0x411e:return "选择逆变器与电池的通信协议";
    case 0x411f:return "设置逆变器通信使用的电池地址";
    case 0x4123:return "远程开启或关闭逆变器";
    case 0x4125:return "控制逆变器屏幕自动关闭";
    case 0x4127:return "温度恢复后可自动重启";
    case 0x4128:return "过载恢复后可自动重启";
    case 0x4129:return "过载时允许切换到市电旁路";
    case 0x412a:return "低负载时启用省电模式";
    case 0x412b:return "自动检测市电输入频率";
    case 0x412c:return "继电器接通后启用软启动";
    case 0x412d:return "电池开路时发出告警";
    case 0x4130:return "控制逆变器蜂鸣器是否静音";
    case 0x4131:return "允许逆变器界面自动返回首页";
    case 0x4132:return "主输入电源变化时发出告警";
    case 0x4142:return "限制电池放电的最大电流";
    case 0x414b:return "电量降到此值时关闭输出 2";
    case 0x4153:return "允许并机系统在无电池时运行";
    case 0x4166:return "检测逆变器是否接入电池";
    case 0x418d:return "电池电量低于此值时告警";
    case 0x41a6:return "选择高温时的功率限制策略";
    default:return "";
    }
}

static const char *setting_edit_help(const panel_snapshot_t *s,uint16_t address,uint16_t draft)
{
    if(address==0x4102){
        switch(draft){
        case 0:return "负载优先使用市电";
        case 1:return panel_is_3ku(s)?"负载优先使用光伏":"光伏优先，再用市电，最后电池";
        case 2:return "光伏优先，再用电池，最后市电";
        case 3:if(!panel_is_3ku(s))return "发电机优先，不向市电回馈";break;
        }
    }
    return setting_help(address);
}

static uint16_t battery_suggestion(const panel_ui_t *u,const panel_snapshot_t *s,uint16_t reg)
{
    if(!u->battery_limits_page)return 0;
    return panel_battery_suggestion(s,reg,u->battery_charge_deciamp,
                                    u->battery_charge_decivolt,u->battery_discharge_decivolt);
}

static bool begin_setting(panel_ui_t *u,const panel_snapshot_t *s)
{
    const panel_setting_t *setting=panel_setting(u->selected);
    if(!setting)return false;
    uint16_t draft=0;
    bool verified,stale;
    if(!displayed_setting(s,setting,&draft,&verified,&stale) || stale || !panel_setting_allowed(s,setting->address,draft)){
        draft=0;
        if(!panel_setting_allowed(s,setting->address,draft) &&
           !panel_setting_step(s,setting->address,draft,1,&draft)){
            u->blocked=true;u->have_result=false;
            snprintf(u->message,sizeof(u->message),"%s",panel_setting_restriction(s,setting->address));return false;
        }
        /* Require an explicit adjustment when there is no current value. */
        draft=0xffff;
    }
    uint16_t suggestion=battery_suggestion(u,s,setting->address);
    u->battery_draft=suggestion && suggestion!=draft;
    if(u->battery_draft)draft=suggestion;
    u->edit_original_valid=displayed_setting(s,setting,&u->edit_original,&verified,&stale) &&
        verified && !stale;
    u->context_seen=true;u->context_generation=s->context_generation;
    u->blocked=false;u->have_result=false;u->draft=draft;u->editing=true;u->review=false;return false;
}

static bool setting_can_configure_model(const panel_snapshot_t *s,
                                        panel_setting_access_t access)
{
    return access==PANEL_ACCESS_MODEL && !s->profile_locked;
}

static bool setting_waits_for_state(panel_setting_access_t access)
{
    return access==PANEL_ACCESS_OFFLINE || access==PANEL_ACCESS_STATE ||
           access==PANEL_ACCESS_BATTERY || access==PANEL_ACCESS_STANDBY ||
           access==PANEL_ACCESS_MODE || access==PANEL_ACCESS_MODE_FIXED;
}

bool panel_ui_can_exit(const panel_ui_t *u,const panel_snapshot_t *s,panel_page_t root)
{
    return u && s && u->page==root && !u->editing && !u->review &&
           !u->pending && !u->blocked && !u->have_result && !u->model_setup &&
           !(u->page==PANEL_PAGE_DETAILS && u->detail_from_index);
}


static void comm_step(panel_ui_t *u,int direction)
{
    panel_config_t *c=&u->comm_draft;
    if(u->selected==0){
        if(direction>0 && c->address<247)c->address++;
        if(direction<0 && c->address>1)c->address--;
    }else if(u->selected==1){
        unsigned index=0;
        while(index+1<sizeof(bauds)/sizeof(bauds[0]) && bauds[index]!=c->baud)index++;
        c->baud=bauds[move(index,sizeof(bauds)/sizeof(bauds[0]),direction)];
    }else if(u->selected==2)c->parity=(uint8_t)move(c->parity,3,direction);
    else if(u->selected==3){
        static const panel_profile_t profiles[]={PANEL_PROFILE_UNKNOWN,PANEL_PROFILE_2KU,
            PANEL_PROFILE_3KU,PANEL_PROFILE_6KU,PANEL_PROFILE_6KU_PARALLEL,PANEL_PROFILE_12KU};
        const unsigned count=sizeof(profiles)/sizeof(profiles[0]);
        unsigned index=0;
        for(unsigned i=1;i<count;i++)if(profiles[i]==c->profile_override){index=i;break;}
        c->profile_override=profiles[move(index,count,direction)];
    }
}

static void adjust_setting(panel_ui_t *u,const panel_snapshot_t *s,int direction,bool fast)
{
    const panel_setting_t *setting=panel_setting(u->selected);
    if(!setting)return;
    unsigned steps=fast && setting_uses_fast_step(setting)?10U:1U;
    uint16_t next;
    bool moved=false;
    while(steps-- && panel_setting_step(s,setting->address,u->draft,direction,&next)){
        u->draft=next;moved=true;u->battery_draft=false;
    }
    if(!moved){
        const char *restriction=panel_setting_restriction(s,setting->address);
        snprintf(u->message,sizeof(u->message),"%s",restriction[0]?restriction:"已到此机型可设置的边界");
    }
}

static bool commit_setting(panel_ui_t *u,const panel_snapshot_t *s,panel_command_t *out)
{
    const panel_setting_t *setting=panel_setting(u->selected);
    bool power=u->page==PANEL_PAGE_POWER;
    if(!power && !setting){u->review=false;u->editing=false;return false;}
    uint16_t reg=power?0x4123:setting->address;
    const char *reason=NULL;
    uint16_t actual=0;
    bool current_valid=!power && panel_fresh(s,panel_setting_current_address(s,setting)) &&
        current_setting(s,setting,&actual);
    const panel_setting_read_t *cached=power?NULL:panel_setting_read_find(s,reg);
    if(!current_valid && cached && cached->state==PANEL_SETTING_VALUE && cached->independent){
        actual=cached->value;current_valid=true;
    }
    if(!panel_setting_allowed(s,reg,u->draft))reason="状态已改变，设置未发送";
    else if(!power && u->battery_draft && battery_suggestion(u,s,reg)!=u->draft)
        reason="电池参数已变化，请重新进入";
    else if(!power && u->edit_original_valid && !current_valid)
        reason="当前值已失效，请重新编辑";
    else if(current_valid && u->edit_original_valid && actual!=u->edit_original && actual!=u->draft)
        reason="当前值已变化，请重新编辑";
    else if(current_valid && actual==u->draft)reason="数值未改变，无需发送";
    if(reason){
        u->review=false;u->editing=false;u->have_result=false;
        snprintf(u->message,sizeof(u->message),"%s",reason);return false;
    }
    out->type=PANEL_CMD_WRITE;out->address=reg;out->value=u->draft;
    return send_command(u,s,out);
}

bool panel_ui_key(panel_ui_t *u,uint8_t key,const panel_snapshot_t *s,panel_command_t *out)
{
    if(!u || !s || !out || key==DIRECT_KEY_NONE || key>DIRECT_KEY_RIGHT)return false;
    memset(out,0,sizeof(*out));
    panel_ui_sync(u,s);
    if(u->pending){
        if(u->pending_type==PANEL_CMD_SCAN && s->scanning && key==DIRECT_KEY_BACK){
            out->type=PANEL_CMD_SCAN_CANCEL;return true;
        }
        return false;
    }
    if(u->have_result){
        u->have_result=false;u->message[0]=0;
        if(key==DIRECT_KEY_ENTER || key==DIRECT_KEY_BACK)return false;
    }
    if(s->command_busy && key==DIRECT_KEY_ENTER &&
       (u->editing || u->review || u->page==PANEL_PAGE_SETTINGS ||
        u->page==PANEL_PAGE_POWER || u->page==PANEL_PAGE_COMM)){
        snprintf(u->message,sizeof(u->message),"其他操作正在进行，请稍后再试");return false;
    }
    u->message[0]=0;
    u->show_recovery=false;
    const int direction=(key==DIRECT_KEY_DOWN || key==DIRECT_KEY_RIGHT)?1:-1;
    if(u->review){
        if(key==DIRECT_KEY_BACK){u->review=false;return false;}
        if(key!=DIRECT_KEY_ENTER)return false;
        if(u->page==PANEL_PAGE_COMM){
            if(u->selected==4)out->type=PANEL_CMD_SCAN;
            else {out->type=PANEL_CMD_CONFIG;out->config=u->comm_draft;}
            return send_command(u,s,out);
        }
        return commit_setting(u,s,out);
    }
    if(u->editing){
        if(key==DIRECT_KEY_BACK){
            u->editing=false;
            if(u->model_setup){u->model_setup=false;u->page=PANEL_PAGE_SETTINGS;u->selected=u->model_return_selected;u->blocked=true;}
            return false;
        }
        if(key==DIRECT_KEY_ENTER){
            if(u->model_setup && u->comm_draft.profile_override==PANEL_PROFILE_UNKNOWN){
                snprintf(u->message,sizeof(u->message),"请按铭牌选择实际机型");return false;
            }
            if(u->page!=PANEL_PAGE_COMM && u->draft==0xffff){
                snprintf(u->message,sizeof(u->message),"请先用方向键选择拟设值");return false;
            }
            if(u->page==PANEL_PAGE_SETTINGS)return commit_setting(u,s,out);
            u->review=true;return false;
        }
        if(u->page==PANEL_PAGE_COMM)comm_step(u,-direction);
        else adjust_setting(u,s,-direction,key==DIRECT_KEY_UP || key==DIRECT_KEY_DOWN);
        return false;
    }
    if(key==DIRECT_KEY_BACK){
        if(u->model_setup){
            u->model_setup=false;u->page=PANEL_PAGE_SETTINGS;u->selected=u->model_return_selected;u->blocked=true;
            return false;
        }
        if(u->blocked){u->blocked=false;return false;}
        if(u->page==PANEL_PAGE_SETTINGS){u->page=PANEL_PAGE_SETTING_GROUPS;u->selected=u->setting_group;return false;}
        if(u->page==PANEL_PAGE_DETAILS && u->detail_from_index){
            u->page=PANEL_PAGE_DETAIL_GROUPS;u->selected=u->detail;u->detail_from_index=false;return false;
        }
        if(u->page==PANEL_PAGE_DETAIL_GROUPS && u->detail_index_from_page){
            u->page=PANEL_PAGE_DETAILS;u->detail=u->detail_origin;u->detail_index_from_page=false;return false;
        }
        u->page=u->page==PANEL_PAGE_MENU?PANEL_PAGE_HOME:PANEL_PAGE_MENU;
        u->selected=0;return false;
    }
    switch(u->page){
    case PANEL_PAGE_HOME:
        if(key==DIRECT_KEY_ENTER){u->page=PANEL_PAGE_MENU;u->selected=0;}
        else if(key==DIRECT_KEY_LEFT || key==DIRECT_KEY_RIGHT){u->page=PANEL_PAGE_DETAILS;u->detail=0;}
        break;
    case PANEL_PAGE_MENU:
        if(key==DIRECT_KEY_ENTER){
            static const panel_page_t pages[]={PANEL_PAGE_DETAIL_GROUPS,PANEL_PAGE_ALARMS,PANEL_PAGE_SETTING_GROUPS,PANEL_PAGE_POWER,PANEL_PAGE_COMM,PANEL_PAGE_DIAG};
            if(u->selected<6)u->page=pages[u->selected];
            u->selected=0;
        }else u->selected=(uint16_t)move(u->selected,6,direction);
        break;
    case PANEL_PAGE_DETAILS:
        if(key==DIRECT_KEY_ENTER){
            if(!u->detail_from_index){
                u->detail_index_from_page=true;
                u->detail_origin=u->detail;
            }
            u->detail_from_index=false;
            u->selected=u->detail;u->page=PANEL_PAGE_DETAIL_GROUPS;
        }else
            u->detail=(uint8_t)step_page(u->detail,PANEL_DETAIL_PAGES,direction);
        break;
    case PANEL_PAGE_DETAIL_GROUPS:
        if(key==DIRECT_KEY_ENTER){
            u->detail=(uint8_t)(u->selected%PANEL_DETAIL_PAGES);
            u->page=PANEL_PAGE_DETAILS;u->detail_from_index=true;
        }else
            u->selected=(uint16_t)move_grid(u->selected,PANEL_DETAIL_PAGES,PANEL_DETAIL_COLUMNS,key);
        break;
    case PANEL_PAGE_ALARMS:
        if(key!=DIRECT_KEY_ENTER)u->selected=(uint16_t)step_page(u->selected,alarm_pages(s),direction);
        break;
    case PANEL_PAGE_SETTINGS:{
        if(key==DIRECT_KEY_ENTER){
            const panel_setting_t *setting=panel_setting(u->selected);
            if(u->blocked && setting){
                panel_setting_access_t access=panel_setting_access(s,setting->address);
                if(setting_can_configure_model(s,access)){
                    u->model_setup=true;u->model_return_selected=u->selected;
                    u->page=PANEL_PAGE_COMM;u->selected=3;u->comm_draft=s->config;
                    u->editing=true;u->blocked=false;return false;
                }
                return false;
            }
            return begin_setting(u,s);
        }
        u->blocked=false;u->have_result=false;
        unsigned count=panel_setting_group_count(u->setting_group);
        unsigned position=panel_setting_group_position(u->setting_group,u->selected);
        if(!count)break;
        if(key==DIRECT_KEY_LEFT || key==DIRECT_KEY_RIGHT)
            position=move_page(position,count,SETTINGS_PAGE_ROWS,direction);
        else position=move(position,count,direction);
        u->selected=(uint16_t)panel_setting_group_index(u->setting_group,position);
        break;
    }
    case PANEL_PAGE_SETTING_GROUPS:
        if(key==DIRECT_KEY_ENTER && u->selected<PANEL_SETTING_GROUPS){
            u->setting_group=(uint8_t)u->selected;
            u->selected=(uint16_t)panel_setting_group_index(u->setting_group,0);
            u->page=PANEL_PAGE_SETTINGS;
        }else
            u->selected=(uint16_t)move_grid(u->selected,PANEL_SETTING_GROUPS,PANEL_SETTING_GROUP_COLUMNS,key);
        break;
    case PANEL_PAGE_POWER:
        if(key==DIRECT_KEY_ENTER){
            u->draft=power_request(u->selected);
            if(panel_setting_allowed(s,0x4123,u->draft))u->review=true;
            else {
                const char *reason=panel_setting_restriction(s,0x4123);
                snprintf(u->message,sizeof(u->message),"%s",*reason?reason:"当前运行状态禁止设置");
            }
        }else if(key==DIRECT_KEY_UP || key==DIRECT_KEY_DOWN)
            u->selected=(uint16_t)move(u->selected,3,direction);
        break;
    case PANEL_PAGE_COMM:
        if(key==DIRECT_KEY_ENTER){
            if(u->selected==4){u->review=true;return false;}
            if(u->selected==3 && s->profile_locked){
                snprintf(u->message,sizeof(u->message),"机型已由出厂锁定");return false;
            }
            u->comm_draft=s->config;u->editing=true;
        }else u->selected=(uint16_t)move(u->selected,5,direction);
        break;
    case PANEL_PAGE_DIAG:
        if(key!=DIRECT_KEY_ENTER)u->detail=(uint8_t)step_page(u->detail,2,direction);
        break;
    }
    return false;
}

static void number(const panel_snapshot_t *s,uint16_t address,unsigned divisor,bool signed_value,char *out,size_t size)
{
    uint16_t raw;
    if(!panel_value(s,address,&raw)){snprintf(out,size,"--");return;}
    int32_t value=signed_value?(int16_t)raw:raw;
    uint32_t magnitude=(uint32_t)(value<0?-value:value);
    const char *stale=panel_fresh(s,address)?"":"*";
    if(divisor==10)snprintf(out,size,"%s%lu.%lu%s",value<0?"-":"",(unsigned long)(magnitude/10),(unsigned long)(magnitude%10),stale);
    else if(divisor==100)snprintf(out,size,"%s%lu.%02lu%s",value<0?"-":"",(unsigned long)(magnitude/100),(unsigned long)(magnitude%100),stale);
    else snprintf(out,size,"%ld%s",(long)value,stale);
}

static void field(const panel_snapshot_t *s,char *out,size_t size,const char *label,uint16_t reg,unsigned divisor,bool sign,const char *unit)
{
    char value[32];number(s,reg,divisor,sign,value,sizeof(value));
    snprintf(out,size,"%s: %s%s",label,value,unit);
}

static void status_line(const panel_snapshot_t *s,char *out,size_t size)
{
    uint16_t state;
    if(s->scanning)snprintf(out,size,"搜索中，运行数据暂停更新");
    else if(!s->config.address)snprintf(out,size,"通信未初始化");
    else if(!panel_value(s,0x42,&state))snprintf(out,size,"地址%u  等待逆变器数据",s->config.address);
    else snprintf(out,size,"地址%u  %s  %s",s->config.address,panel_online(s)?"在线":"离线",run_state(s));
}

static void detail_text(panel_view_t *v,unsigned row,const char *label,const char *value,bool stale)
{
    snprintf(v->rows[row],sizeof(v->rows[row]),"%s",label);
    snprintf(v->values[row],sizeof(v->values[row]),"%s%s",value,stale?"*":"");
}

static void detail_view(const panel_ui_t *u,const panel_snapshot_t *s,panel_view_t *v)
{
    static const uint16_t regs[PANEL_DETAIL_PAGES][7]={
        {0x50,0x51,0x52,0x53,0x58,0x5b,0x5d},
        {0x80,0x81,0x84,0x85,0x154,0x177,0x178},
        {0x96,0x97,0x98,0x99,0x9a,0x9b,0xa0},
        {0}, /* Battery telemetry comes from the panel BMS link. */
        {0x330,0x331,0x332,0x333,0x334,0x335,0x336},
        {0x54,0x55,0x56,0x57,0x350,0x42,0x44},
        {0x9c,0x9d,0x58,0x59,0x5a,0x5b,0x5c},
        {0x140,0x142,0x143,0x150,0x141,0x351,0x350}};
    static const char *const labels[PANEL_DETAIL_PAGES][7]={
        {"市电电压","市电电流","市电频率","市电功率","负载电压","负载功率","负载比例"},
        {"电池电压","电池电流","电池电量","电池功率","母线电压","恒压目标","浮充目标"},
        {"光伏1电压","光伏1电流","光伏1功率","光伏2电压","光伏2电流","光伏2功率","光伏总功率"},
        {"电池电压","电池电流","电池电量","电池通信","","",""},
        {"光伏1温度","逆变温度","充电温度","充电2温度","环境温度","放电温度","光伏2温度"},
        {"发电机电压","发电机电流","发电机频率","发电机功率","功能状态","运行状态","能流状态"},
        {"光伏今日电量","光伏累计电量","输出电压","输出电流","输出频率","输出功率","输出视在功率"},
        {"输入范围","输出优先级","充电优先级","电池类型","并机模式","功能开关","功能开关2"}};
    static const unsigned divs[PANEL_DETAIL_PAGES][7]={{10,100,100,1,10,1,10},{10,10,10,1,10,10,10},{10,100,1,10,100,1,1},{10,10,1,10,1,1,1},{10,10,10,10,10,10,10},
        {10,100,100,1,1,1,1},{10,10,10,100,100,1,1},{1,1,1,1,1,1,1}};
    static const char *const units[PANEL_DETAIL_PAGES][7]={{"V","A","Hz","W","V","W","%"},{"V","A","%","W","V","V","V"},{"V","A","W","V","A","W","W"},{"V","A","%","","","",""},{"℃","℃","℃","℃","℃","℃","℃"},
        {"V","A","Hz","W","","",""},{"kWh","kWh","V","A","Hz","W","VA"},{"","","","","","",""}};
    unsigned page=u->detail%PANEL_DETAIL_PAGES;
    const bool is_3ku=panel_is_3ku(s);
    snprintf(v->title,sizeof(v->title),"%s %u/%u",detail_titles[page],page+1,PANEL_DETAIL_PAGES);
    /* RMS voltages, frequencies, SOC, PV power and cumulative energy are
     * unsigned. Directional currents/powers and temperatures are signed. */
    static const uint8_t signed_fields[]={0x2a,0x0a,0x00,0x0a,0x7f,0x0a,0x28,0x00};
    for(unsigned i=0;i<7;i++){
        char value[24];
        unsigned divisor=divs[page][i];
        if(page==3){
            if(i<4)detail_text(v,i,labels[page][i],"--",false);
            continue;
        }
        if(is_3ku && page==2){
            if(i>=3 && i<=5)continue; /* 3KU has one MPPT. */
            if(i==1)divisor=100;
            if(i==2)divisor=1;
        }
        if(is_3ku && page==4)divisor=1;
        if(is_3ku && page==4 && (i==3 || i>=5))continue;
        number(s,regs[page][i],divisor,(signed_fields[page]&(1U<<i))!=0,value,sizeof(value));
        size_t end=strlen(value);
        bool stale=end && value[end-1]=='*';
        if(stale)value[end-1]=0;
        const char *unit=strcmp(value,"--")?units[page][i]:"";
        const char *space=*unit && page!=4?" ":"";
        snprintf(v->rows[i],sizeof(v->rows[i]),"%s",is_3ku && page==4 && !i?"光伏温度":labels[page][i]);
        snprintf(v->values[i],sizeof(v->values[i]),"%s%s%s%s",value,space,unit,stale?"*":"");
    }
    if(page==0){
        int32_t power;
        bool fresh;
        if(panel_grid_watts(s,&power,&fresh) && (power || is_3ku)){
            char watts[16];
            snprintf(watts,sizeof(watts),"%ld W",(long)(power<0?-power:power));
            detail_text(v,3,power==0?"市电功率":power<0?"向电网送电":"从电网取电",watts,!fresh);
        }else if(is_3ku)detail_text(v,3,"市电功率","--",false);
    }
    if(is_3ku && page==2){
        uint32_t power;
        bool fresh;
        if(panel_pv_watts(s,&power,&fresh)){
            char watts[16];
            snprintf(watts,sizeof(watts),"%lu W",(unsigned long)power);
            detail_text(v,6,"光伏总功率",watts,!fresh);
        }
    }
    if(page==1){
        uint16_t raw_power,raw_current;
        if(panel_value(s,0x85,&raw_power)){
            int power=(int16_t)raw_power;
            const char *label="电池功率";
            bool power_fresh=panel_fresh(s,0x85);
            /* V1.27 defines direction on current, not the power word. */
            if(power_fresh && panel_fresh(s,0x81) && panel_value(s,0x81,&raw_current)){
                int16_t current=(int16_t)raw_current;
                if(current)label=current<0?"充电功率":"放电功率";
            }
            char watts[16];
            snprintf(watts,sizeof(watts),"%d W",power<0?-power:power);
            detail_text(v,3,label,watts,!power_fresh);
        }
    }
    if(page==5){
        uint16_t raw;
        bool known=panel_value(s,0x350,&raw);
        detail_text(v,4,"发电机功能",known?((raw&(1U<<8))?"开启":"关闭"):"--",known && !panel_fresh(s,0x350));
        static const char *const input[]={"未连接","连接异常","连接正常","未定义"};
        known=panel_value(s,0x43,&raw);
        detail_text(v,5,"输入",known?input[(raw>>2)&3U]:"--",known && !panel_fresh(s,0x43));
        static const char *const paths[]={"未启用","充电","负载供电","充电与供电"};
        known=panel_value(s,0x44,&raw);
        detail_text(v,6,"发电通路",known?paths[(raw>>2)&3U]:"--",known && !panel_fresh(s,0x44));
    }
    if(page==7){
        static const uint16_t settings[]={0x4103,0x4102,0x4104,0x4107,0x4110};
        for(unsigned i=0;i<5;i++){
            if(i==3){detail_text(v,i,"电池型号","--",false);continue;}
            uint16_t raw;char value[64]="--";
            bool known=panel_value(s,regs[page][i],&raw);
            if(known){
                uint16_t decoded;
                if(panel_setting_decode_current(s,settings[i],raw,&decoded))
                    panel_setting_format(s,settings[i],decoded,value,sizeof(value));
            }
            detail_text(v,i,labels[page][i],value,known && !panel_fresh(s,regs[page][i]));
        }
        static const unsigned bits[]={1,3};
        static const char *const labels[]={"过温重启","过载转旁路"};
        for(unsigned i=0;i<2;i++){
            uint16_t raw;
            bool known=panel_value(s,0x351,&raw);
            const char *value=known?((raw&(1U<<bits[i]))?"开启":"关闭"):"--";
            detail_text(v,5+i,labels[i],value,known && !panel_fresh(s,0x351));
        }
    }
    snprintf(v->footer,sizeof(v->footer),"方向键换页  确认目录  返回");
}

static void alarm_view(const panel_ui_t *u,const panel_snapshot_t *s,panel_view_t *v)
{
    v->alarm_list=true;
    static const char *const faults[32]={"市电软启失败","母线过压","母线欠压","电池过流","过温","电池过压","电池软启故障","母线短路","逆变软启故障","逆变过压","逆变欠压","逆变短路","负功保护","过载故障","机型不匹配","无引导程序","程序烧录中","光伏反接","并机序号异常","并机通信异常","并机电池压差","并机市电压差","并机频率差","并机设置缺相","并机同步丢失","BMS故障","MCU故障","保留位","逆变负载异常","PV过压","保留位","保留位"};
    static const char *const warnings[32]={"电池未接","电池欠压","电池低压","充电器短路","保留位","电池过充","BMS丢失","过温预留","风扇堵转","存储故障","过载","发电机波形","PV能量弱","并机同步丢失","并机设置缺相","并机版本","并机通信异常","并机压差频差","低SOC关机","低SOC告警","并机电池压差","电池短路","电池低于启动","发电机过载","发电机欠压","发电机过压","CT连接异常","市电不稳定","电表通信失败","保留位","保留位","保留位"};
    static const char *const faults_3ku[32]={
        [0]="母线软启故障",[3]="硬件过流 DC/DC",[6]="母线IP软启故障",
        [15]="认证错误",[16]="屏幕Flash故障",[17]="光伏硬件过流",
        [23]="并机输出参数不同",[27]="热敏电阻开路",[28]="逆变硬件过流",[29]="保留位"};
    static const char *const warnings_3ku[32]={
        [7]="过温",[11]="发电机掉电",[14]="并机配置不兼容",
        [20]="保留位",[21]="保留位",[22]="电池辅助电源故障",
        [23]="保留位",[24]="保留位",[25]="保留位",[26]="保留位",[27]="保留位",[28]="保留位"};
    unsigned pages=alarm_pages(s),page=u->selected<pages?u->selected:pages-1;
    unsigned total=0,current=0,shown=0,skip=page*5;
    uint16_t bits[4]={0};
    unsigned known=0,fresh=0;
    for(unsigned i=0;i<4;i++){
        if(panel_value(s,(uint16_t)(0x46+i),&bits[i])){
            known|=1U<<i;
            if(panel_fresh(s,(uint16_t)(0x46+i))){
                fresh|=1U<<i;
            }
        }
    }
    snprintf(v->title,sizeof(v->title),"故障告警 %u/%u",page+1,pages);
    snprintf(v->footer,sizeof(v->footer),"%s",known!=fresh?"方向键翻页  返回  历史仅供参考":"方向键翻页  返回退出");
    if(!known){snprintf(v->rows[0],sizeof(v->rows[0]),"-- 尚未收到告警数据");return;}
    /* Put fresh alarms first, even when cached fault bits are still set. */
    for(unsigned stale=0;stale<2;stale++)for(unsigned i=0;i<4;i++){
        if(stale==((fresh>>i)&1U))continue;
        for(unsigned bit=0;bit<16;bit++)if(bits[i]&(1U<<bit)){
            if(!stale)current++;
            if(total++<skip || shown>=5)continue;
            unsigned index=(i%2)*16+bit;
            const char *name=i<2?faults[index]:warnings[index];
            const char *override=i<2?faults_3ku[index]:warnings_3ku[index];
            if(panel_is_3ku(s) && override)name=override;
            snprintf(v->rows[2+shown],sizeof(v->rows[2+shown]),"%s%c%02u %s",
                stale?"历史 ":"",i<2?'F':'W',index+1,name);
            if(stale)v->stale_rows|=(uint8_t)(1U<<(2+shown));
            if(i<2)v->fault_rows|=(uint8_t)(1U<<(2+shown));
            shown++;
        }
    }
    v->alert=current!=0;
    if(total){
        unsigned previous=total-current;
        if(current || fresh==15)
            snprintf(v->rows[0],sizeof(v->rows[0]),u->language_en?"Current %u  /  Previous %u":"当前 %u  /  上次 %u",current,previous);
        else
            snprintf(v->rows[0],sizeof(v->rows[0]),u->language_en?"Current unknown  /  Previous %u":"当前待确认  /  上次 %u",previous);
    }
    if(!total)snprintf(v->rows[2],sizeof(v->rows[2]),"%s",fresh==15?"未报告故障或告警":"当前告警状态待确认");
    else if(!shown)snprintf(v->rows[2],sizeof(v->rows[2]),"本页无条目，向上翻页");
    if(!fresh)snprintf(v->subtitle,sizeof(v->subtitle),"历史数据，不能作为当前告警");
    else if(fresh!=known)snprintf(v->subtitle,sizeof(v->subtitle),"部分告警数据已过期");
    else if(known!=15)snprintf(v->subtitle,sizeof(v->subtitle),"部分告警数据未收到");
}

static void key_labels(const panel_ui_t *u,const panel_snapshot_t *s,panel_view_t *v,
                       panel_setting_access_t access)
{
    uint8_t *key=v->key_labels;
    if(u->pending){
        if(u->pending_type==PANEL_CMD_SCAN && s->scanning)key[KEY_BACK]=KEY_CANCEL;
        return;
    }
    key[KEY_BACK]=KEY_RETURN;
    if(u->have_result){key[KEY_BACK]=key[KEY_ENTER]=KEY_CLOSE;return;}
    if(u->page==PANEL_PAGE_SETTINGS && u->blocked){
        key[KEY_BACK]=KEY_CLOSE;
        if(!s->command_busy && setting_can_configure_model(s,access))key[KEY_ENTER]=KEY_OPEN;
        return;
    }
    if(u->review || u->editing){
        key[KEY_BACK]=KEY_CANCEL;
        if(!s->command_busy && (u->page!=PANEL_PAGE_SETTINGS || u->draft!=0xffff))
            key[KEY_ENTER]=u->review?KEY_SEND:u->page==PANEL_PAGE_SETTINGS?KEY_SAVE:KEY_OK;
        if(u->editing){
            key[KEY_LEFT]=KEY_PLUS;key[KEY_RIGHT]=KEY_MINUS;
            const panel_setting_t *setting=u->page==PANEL_PAGE_SETTINGS?panel_setting(u->selected):NULL;
            bool fast=setting && setting_uses_fast_step(setting);
            key[KEY_DOWN]=fast?KEY_FAST_DOWN:KEY_MINUS;
            key[KEY_UP]=fast?KEY_FAST_UP:KEY_PLUS;
        }
        return;
    }
    if(u->page==PANEL_PAGE_DETAILS || u->page==PANEL_PAGE_ALARMS || u->page==PANEL_PAGE_DIAG){
        if(u->page!=PANEL_PAGE_ALARMS || alarm_pages(s)>1){
            key[KEY_LEFT]=key[KEY_UP]=KEY_PREV;
            key[KEY_DOWN]=key[KEY_RIGHT]=KEY_NEXT;
        }
        if(u->page==PANEL_PAGE_DETAILS)key[KEY_ENTER]=KEY_INDEX;
        return;
    }
    if(u->page==PANEL_PAGE_SETTING_GROUPS || u->page==PANEL_PAGE_DETAIL_GROUPS){
        key[KEY_LEFT]=KEY_ARROW_LEFT;
        key[KEY_UP]=KEY_UPWARD;
        key[KEY_DOWN]=KEY_DOWNWARD;
        key[KEY_RIGHT]=KEY_ARROW_RIGHT;
        key[KEY_ENTER]=KEY_OPEN;
        return;
    }
    if(u->page==PANEL_PAGE_HOME){key[KEY_LEFT]=key[KEY_RIGHT]=KEY_OPEN;key[KEY_ENTER]=KEY_OPEN;return;}
    if(u->page==PANEL_PAGE_SETTINGS && panel_setting_group_count(u->setting_group)>SETTINGS_PAGE_ROWS){
        key[KEY_LEFT]=KEY_PREV;key[KEY_RIGHT]=KEY_NEXT;
    }
    key[KEY_UP]=KEY_PREV_ITEM;key[KEY_DOWN]=KEY_NEXT_ITEM;
    if(u->page==PANEL_PAGE_SETTINGS){
        if(!s->command_busy)key[KEY_ENTER]=
            access==PANEL_ACCESS_EDITABLE?KEY_EDIT:KEY_INFO;
    }else if(u->page==PANEL_PAGE_POWER){
        if(!s->command_busy)key[KEY_ENTER]=panel_setting_allowed(s,0x4123,power_request(u->selected))?KEY_REVIEW:KEY_INFO;
    }else if(u->page==PANEL_PAGE_COMM){
        if(!s->command_busy)key[KEY_ENTER]=u->selected==4?KEY_SCAN:KEY_EDIT;
    }else key[KEY_ENTER]=KEY_OPEN;
}

bool panel_ui_view(const panel_ui_t *u,const panel_snapshot_t *s,panel_view_t *v)
{
    if(!u || !s || !v)return false;
    memset(v,0,sizeof(*v));v->selected=-1;
    const panel_setting_t *selected_setting=u->page==PANEL_PAGE_SETTINGS?panel_setting(u->selected):NULL;
    panel_setting_access_t access=selected_setting && !u->editing && !u->have_result && !u->pending?
        panel_setting_access(s,selected_setting->address):PANEL_ACCESS_EDITABLE;
    bool setting_notice=false;
    const char *selected_help="";
    key_labels(u,s,v,access);
    status_line(s,v->subtitle,sizeof(v->subtitle));
    snprintf(v->footer,sizeof(v->footer),"确认进入  返回退出");
    switch(u->page){
    case PANEL_PAGE_HOME:{
        char a[32],b[32];
        snprintf(v->title,sizeof(v->title),"逆变器控制面板");
        snprintf(v->rows[0],sizeof(v->rows[0]),"运行: %s%s",run_state(s),panel_fresh(s,0x42)?"":"*");
        number(s,0x50,10,true,a,sizeof(a));
        int32_t grid;
        bool fresh;
        if(panel_grid_watts(s,&grid,&fresh))
            snprintf(b,sizeof(b),"%ld%s",(long)grid,fresh?"":"*");
        else snprintf(b,sizeof(b),"--");
        snprintf(v->rows[1],sizeof(v->rows[1]),"市电: %sV  %sW",a,b);
        field(s,v->rows[2],sizeof(v->rows[2]),"光伏",0xa0,1,false,"W");
        if(panel_is_3ku(s)){
            uint32_t pv;
            bool fresh;
            if(panel_pv_watts(s,&pv,&fresh))
                snprintf(v->rows[2],sizeof(v->rows[2]),"光伏: %luW%s",(unsigned long)pv,fresh?"":"*");
        }
        number(s,0x80,10,true,a,sizeof(a));number(s,0x81,10,true,b,sizeof(b));
        snprintf(v->rows[3],sizeof(v->rows[3]),"电池: %sV  %sA",a,b);
        field(s,v->rows[4],sizeof(v->rows[4]),"电量",0x84,10,true,"%");
        field(s,v->rows[5],sizeof(v->rows[5]),"负载",0x5b,1,true,"W");
        snprintf(v->rows[6],sizeof(v->rows[6]),"电池电流: 正为放电，负为充电");
        snprintf(v->footer,sizeof(v->footer),"确认菜单  右键参数  *为旧值");break;
    }
    case PANEL_PAGE_MENU:
        snprintf(v->title,sizeof(v->title),"主菜单");v->selected=u->selected;
        for(unsigned i=0;i<6;i++)snprintf(v->rows[i],sizeof(v->rows[i]),"%s",menus[i]);
        break;
    case PANEL_PAGE_DETAILS:
        detail_view(u,s,v);v->detail_dashboard=true;v->illustration=u->detail;
        break;
    case PANEL_PAGE_DETAIL_GROUPS:{
        snprintf(v->title,sizeof(v->title),"运行数据");
        snprintf(v->subtitle,sizeof(v->subtitle),"选择分类，确认进入");
        v->selected=u->selected%PANEL_DETAIL_PAGES;
        v->detail_groups=true;
        snprintf(v->footer,sizeof(v->footer),"选择分类，确认进入");
        break;
    }
    case PANEL_PAGE_ALARMS:alarm_view(u,s,v);break;
    case PANEL_PAGE_SETTINGS:{
        v->illustration=u->setting_group;
        const panel_setting_t *setting=selected_setting;
        unsigned count=panel_setting_group_count(u->setting_group);
        unsigned position=panel_setting_group_position(u->setting_group,u->selected);
        unsigned page=position/SETTINGS_PAGE_ROWS+1;
        unsigned pages=(count+SETTINGS_PAGE_ROWS-1)/SETTINGS_PAGE_ROWS;
        snprintf(v->title,sizeof(v->title),"%s %u/%u",panel_setting_group_name(u->setting_group),page,pages);
        if(!setting)break;
        bool charge_switch_reserved=panel_charge_switch_reserved(s);
        bool reserved=charge_switch_reserved && setting->address==0x4124;
        if(!reserved){v->setting_addresses[0]=setting->address;v->setting_count=1;}
        selected_help=setting_help(setting->address);
        char current[64],draft[64];
        bool verified=true,stale=false,have_current=false;
        if(u->blocked || u->editing){
            uint16_t raw;
            have_current=!reserved && displayed_setting(s,setting,&raw,&verified,&stale);
            if(have_current)panel_setting_format(s,setting->address,raw,current,sizeof(current));
            else snprintf(current,sizeof(current),"%s",reserved?"未定义":setting_read_status(s,setting->address,false));
        }
        if(u->blocked){
            snprintf(v->title,sizeof(v->title),"%s",setting->label);
            snprintf(v->subtitle,sizeof(v->subtitle),"当前: %s",current);
            snprintf(v->rows[0],sizeof(v->rows[0]),"暂不能修改");
            bool model=setting_can_configure_model(s,access);
            bool waits=setting_waits_for_state(access);
            snprintf(v->rows[1],sizeof(v->rows[1]),"%s",access==PANEL_ACCESS_MODEL && !model?
                     "请联系安装人员确认机型":panel_setting_restriction(s,setting->address));
            snprintf(v->rows[3],sizeof(v->rows[3]),"%s",model?"候选型号不等于已确认机型":"未向逆变器发送设置");
            if(model || waits)snprintf(v->rows[4],sizeof(v->rows[4]),"%s",
                model?"请核对逆变器铭牌后再选择":"条件恢复后可以重新尝试");
            else if(reserved && access==PANEL_ACCESS_UNSUPPORTED)
                snprintf(v->rows[4],sizeof(v->rows[4]),"仅 3KU 协议定义此开关");
            snprintf(v->footer,sizeof(v->footer),"%s",model?"确认机型设置  返回参数列表":
                "返回参数列表");
            v->alert=true;
        }else if(u->editing){
            v->setting_editor=true;
            snprintf(v->title,sizeof(v->title),"%s",setting->label);
            panel_setting_format(s,setting->address,u->draft,draft,sizeof(draft));
            snprintf(v->subtitle,sizeof(v->subtitle),"%s",setting_edit_help(s,setting->address,u->draft));
            snprintf(v->rows[0],sizeof(v->rows[0]),"%s",setting->label);
            detail_text(v,1,verified?"当前值":"设备返回值",current,stale);
            if(!have_current)setting_read_flags(v,s,setting->address,1);
            detail_text(v,2,"拟设值",draft,false);
            snprintf(v->rows[3],sizeof(v->rows[3]),"%s",u->draft==0xffff?"请先选择拟设值":
                     u->battery_draft?"来自电池，确认后应用":"");
            snprintf(v->rows[4],sizeof(v->rows[4]),"%s",
                setting_uses_fast_step(setting)?"左加右减，上加下减快调":"方向键切换选项");
            v->selected=2;
            setting_notice=u->draft==0xffff;
            snprintf(v->footer,sizeof(v->footer),"%s",setting_notice?
                "请先用方向键选择拟设值":"返回取消  确认保存");
        }else{
            v->setting_list=true;
            v->setting_count=0;
            unsigned first=(position/SETTINGS_PAGE_ROWS)*SETTINGS_PAGE_ROWS;
            for(unsigned row=0;row<SETTINGS_PAGE_ROWS;row++){
                const panel_setting_t *item=panel_setting(panel_setting_group_index(u->setting_group,first+row));
                if(item){
                    snprintf(v->rows[row],sizeof(v->rows[row]),"%s",item->label);
                    if(charge_switch_reserved && item->address==0x4124){
                        snprintf(v->values[row],sizeof(v->values[row]),"未定义");
                        v->unread_rows|=(uint8_t)(1U<<row);
                        continue;
                    }
                    v->setting_addresses[v->setting_count++]=item->address;
                    uint16_t value;
                    bool item_verified,item_stale;
                    if(displayed_setting(s,item,&value,&item_verified,&item_stale)){
                        panel_setting_format(s,item->address,value,v->values[row],sizeof(v->values[row]));
                        if(item_stale)v->stale_rows|=(uint8_t)(1U<<row);
                    }else{
                        snprintf(v->values[row],sizeof(v->values[row]),"%s",setting_read_status(s,item->address,true));
                        setting_read_flags(v,s,item->address,row);
                    }
                }
            }
            v->selected=position%SETTINGS_PAGE_ROWS;
            const char *restriction=panel_setting_restriction(s,setting->address);
            setting_notice=restriction[0];
            snprintf(v->footer,sizeof(v->footer),"%s",setting_notice?restriction:
                "上下选择  确认修改  返回分类");
        }
        break;
    }
    case PANEL_PAGE_SETTING_GROUPS:
        snprintf(v->title,sizeof(v->title),"逆变器设置");
        snprintf(v->subtitle,sizeof(v->subtitle),"选择分类，确认进入");
        if(panel_effective_profile(s)==PANEL_PROFILE_UNKNOWN)
            snprintf(v->subtitle,sizeof(v->subtitle),"先在维护诊断确认逆变器机型");
        v->setting_groups=true;v->selected=u->selected;
        break;
    case PANEL_PAGE_POWER:{
        static const char *const actions[]={"开启逆变器","关闭逆变器","停用远程开关机"};
        static const char *const effects[]={"开启后负载可能通电","关闭后负载可能断电","逆变器将退出远程开关机模式"};
        v->power_action=u->review || u->pending;
        unsigned operation=v->power_action?(u->draft==1?0U:u->draft==2?1U:2U):u->selected%3U;
        if(v->power_action){
            const char *title=u->pending?"开关机处理中":"确认开关机";
            snprintf(v->title,sizeof(v->title),"%s",title);
            snprintf(v->rows[0],sizeof(v->rows[0]),"%s",actions[operation]);
            snprintf(v->rows[1],sizeof(v->rows[1]),"%s",effects[operation]);
            if(operation<2)snprintf(v->rows[2],sizeof(v->rows[2]),"同时启用远程开关机");
            v->selected=0;
        }else{
            snprintf(v->title,sizeof(v->title),"开关机操作");
            for(unsigned i=0;i<3;i++)snprintf(v->rows[i],sizeof(v->rows[i]),"%s",actions[i]);
            snprintf(v->rows[4],sizeof(v->rows[4]),"%s",effects[operation]);
            if(operation<2)snprintf(v->rows[5],sizeof(v->rows[5]),"同时启用远程开关机");
            v->selected=u->selected;
        }
        if(u->review){
            v->alert=true;
            snprintf(v->footer,sizeof(v->footer),"确认发送  返回取消操作");
        }
        break;
    }
    case PANEL_PAGE_COMM:{
        const panel_config_t *c=u->editing?&u->comm_draft:&s->config;
        snprintf(v->title,sizeof(v->title),"%s%s",u->model_setup?"确认逆变器机型":"通信配置",u->review?" - 确认":"");
        snprintf(v->rows[0],sizeof(v->rows[0]),"从机地址: %u",c->address);
        snprintf(v->rows[1],sizeof(v->rows[1]),"波特率: %lu",(unsigned long)c->baud);
        snprintf(v->rows[2],sizeof(v->rows[2]),"串口: 8%c1","NEO"[c->parity<3?c->parity:0]);
        snprintf(v->rows[3],sizeof(v->rows[3]),"%s: %s",s->profile_locked?"出厂机型":"机型",
            c->profile_override==PANEL_PROFILE_UNKNOWN?"自动 / 待确认":panel_profile_name(c->profile_override));
        snprintf(v->rows[4],sizeof(v->rows[4]),"只读搜索从机地址");
        snprintf(v->rows[5],sizeof(v->rows[5]),"运行机型: %s",panel_profile_name(panel_effective_profile(s)));
        if(s->scanning)snprintf(v->rows[6],sizeof(v->rows[6]),"搜索地址 %u/247",s->scan_address);
        else if(s->scan_found)snprintf(v->rows[6],sizeof(v->rows[6]),"找到地址%u，请手动选择",s->scan_found);
        else if(panel_effective_profile(s)==PANEL_PROFILE_UNKNOWN)snprintf(v->rows[6],sizeof(v->rows[6]),"请核对铭牌，再确认机型");
        v->selected=u->selected;
        if(u->editing)snprintf(v->footer,sizeof(v->footer),"左右调整  确认复核  返回取消");
        if(u->review)snprintf(v->footer,sizeof(v->footer),"确认保存本机配置  返回编辑");
        if(u->review && u->selected==4){
            snprintf(v->rows[5],sizeof(v->rows[5]),"按当前波特率和校验搜索");
            snprintf(v->rows[6],sizeof(v->rows[6]),"搜索期间暂停实时监测");
            snprintf(v->footer,sizeof(v->footer),"确认搜索  返回取消");
        }
        if(u->model_setup){
            memset(v->rows,0,sizeof(v->rows));
            snprintf(v->rows[0],sizeof(v->rows[0]),"机型: %s",c->profile_override==PANEL_PROFILE_UNKNOWN?"自动 / 待确认":panel_profile_name(c->profile_override));
            snprintf(v->rows[2],sizeof(v->rows[2]),"候选机型: %s",panel_profile_name(s->detected_profile));
            snprintf(v->rows[3],sizeof(v->rows[3]),"请核对逆变器铭牌后再选择");
            snprintf(v->rows[4],sizeof(v->rows[4]),"候选型号不等于已确认机型");
            snprintf(v->footer,sizeof(v->footer),"%s",u->review?"确认保存本机配置  返回编辑":"方向键调整  确认复核  返回取消");
            v->selected=0;
        }
        break;
    }
    case PANEL_PAGE_DIAG:{
        char a[32]="--",b[32]="--";uint16_t value;
        snprintf(v->title,sizeof(v->title),"通信诊断");
        if(u->detail%2){
            snprintf(v->title,sizeof(v->title),"主控升级只读诊断");
            if(s->ota_valid&1)snprintf(v->rows[0],sizeof(v->rows[0]),"升级状态4090: %04X%s",s->ota_error,s->now_ms-s->ota_updated_ms[0]>PANEL_FRESH_MS?"*":"");
            else snprintf(v->rows[0],sizeof(v->rows[0]),"升级状态4090: --");
            if(s->ota_valid&2)snprintf(v->rows[1],sizeof(v->rows[1]),"运行状态4092: %04X%s",s->ota_run,s->now_ms-s->ota_updated_ms[1]>PANEL_FRESH_MS?"*":"");
            else snprintf(v->rows[1],sizeof(v->rows[1]),"运行状态4092: --");
            if(s->ota_valid&4){
                const char *mark=s->now_ms-s->ota_updated_ms[2]>PANEL_FRESH_MS?"*":"";
                snprintf(v->rows[2],sizeof(v->rows[2]),"可烧录类型: %04X%s",s->ota_info[1],mark);
                snprintf(v->rows[3],sizeof(v->rows[3]),"机型: %04X 软件: %04X%s",s->ota_info[2],s->ota_info[3],mark);
                snprintf(v->rows[4],sizeof(v->rows[4]),"版本: %04X %04X%s",s->ota_info[6],s->ota_info[7],mark);
            }else snprintf(v->rows[2],sizeof(v->rows[2]),"升级能力: --");
            snprintf(v->rows[5],sizeof(v->rows[5]),"主控升级包格式尚未明确");
            snprintf(v->rows[6],sizeof(v->rows[6]),"此页仅读取，不发送升级命令");
            snprintf(v->footer,sizeof(v->footer),"左右切换诊断  返回退出");
            break;
        }
        if(panel_value(s,0x38,&value))snprintf(a,sizeof(a),"0x%04X%s",value,panel_fresh(s,0x38)?"":"*");
        if(panel_value(s,0x39,&value))snprintf(b,sizeof(b),"0x%04X%s",value,panel_fresh(s,0x39)?"":"*");
        snprintf(v->rows[0],sizeof(v->rows[0]),"0x0038: %s",a);
        snprintf(v->rows[1],sizeof(v->rows[1]),"0x0039: %s",b);
        snprintf(v->rows[2],sizeof(v->rows[2]),"候选机型: %s",panel_profile_name(s->detected_profile));
        snprintf(v->rows[3],sizeof(v->rows[3]),"轮询%lu  错误%lu",(unsigned long)s->polls,(unsigned long)s->errors);
        snprintf(v->rows[4],sizeof(v->rows[4]),"写入0x%04X  异常0x%02X",s->write_address,s->exception);
        snprintf(v->rows[5],sizeof(v->rows[5]),"识别地址定义存在版本差异");
        if(s->scanning)snprintf(v->rows[6],sizeof(v->rows[6]),"搜索%u  找到%u",s->scan_address,s->scan_found);
        else snprintf(v->rows[6],sizeof(v->rows[6]),"GPIO1 TX / GPIO0 RX");
        snprintf(v->footer,sizeof(v->footer),"左右切换诊断  返回退出");
        break;
    }
    }
    bool verify_output=panel_power_changes_output(u->result_address,u->result_value);
    if(u->have_result){
        const panel_setting_t *setting=panel_setting_find(u->result_address);
        v->result=true;
        v->stale_rows=v->unread_rows=v->read_error_rows=0;
        char target[64];panel_setting_format_profile(u->result_profile,u->result_address,u->result_value,target,sizeof(target));
        memset(v->rows,0,sizeof(v->rows));
        memset(v->values,0,sizeof(v->values));
        snprintf(v->title,sizeof(v->title),"操作结果");
        snprintf(v->subtitle,sizeof(v->subtitle),"%s",setting?setting->label:"逆变器设置");
        snprintf(v->rows[0],sizeof(v->rows[0]),"目标: %s",target);
        snprintf(v->rows[2],sizeof(v->rows[2]),"%s",result_state(u->last_result));
        if(u->last_result==PANEL_WRITE_REJECTED && !u->result_exception){
            snprintf(v->rows[2],sizeof(v->rows[2]),"命令未发送");
            snprintf(v->rows[3],sizeof(v->rows[3]),"请检查连接和运行状态");
        }
        if(u->result_exception){
            static const char *const errors[]={"","不支持此写入命令","该地址不支持设置","逆变器拒绝参数值","逆变器处理失败"};
            if(u->result_exception<5)snprintf(v->rows[3],sizeof(v->rows[3]),"%s",errors[u->result_exception]);
            else snprintf(v->rows[3],sizeof(v->rows[3]),"应答码: %02X",u->result_exception);
        }
        if(u->last_result==PANEL_WRITE_PROTOCOL_ACCEPTED)
            snprintf(v->rows[4],sizeof(v->rows[4]),"逆变器已返回完成应答");
        if(setting && u->result_readback_valid && u->result_profile==panel_effective_profile(s)){
            char value[48];panel_setting_format(s,setting->address,u->result_readback,value,sizeof(value));
            detail_text(v,1,"当前值",value,false);
            if(u->last_result==PANEL_WRITE_PROCESSED){
                snprintf(v->rows[2],sizeof(v->rows[2]),"读回值与目标不一致");
                snprintf(v->rows[4],sizeof(v->rows[4]),"逆变器当前值与目标不同");
            }
        }
        if(verify_output && u->last_result==PANEL_WRITE_CONFIRMED){
            snprintf(v->rows[2],sizeof(v->rows[2]),"%s",u->result_value==1?"开机成功":"关机成功");
            snprintf(v->rows[4],sizeof(v->rows[4]),"已通过输出电压确认");
        }else if(verify_output && u->last_result==PANEL_WRITE_UNCERTAIN){
            snprintf(v->rows[2],sizeof(v->rows[2]),"未确认输出变化");
            snprintf(v->rows[4],sizeof(v->rows[4]),"请检查逆变器状态");
        }
        if((u->last_result==PANEL_WRITE_PROCESSED || u->last_result==PANEL_WRITE_UNCERTAIN) && !v->rows[4][0])
            snprintf(v->rows[4],sizeof(v->rows[4]),"未能核对当前值，请稍后重试");
        if(u->last_result==PANEL_WRITE_REJECTED || (u->last_result==PANEL_WRITE_UNCERTAIN && !verify_output))
            snprintf(v->rows[5],sizeof(v->rows[5]),"返回后可以重新设置");
        v->selected=-1;
        v->alert=u->last_result!=PANEL_WRITE_CONFIRMED && u->last_result!=PANEL_WRITE_PROTOCOL_ACCEPTED;
        snprintf(v->footer,sizeof(v->footer),"确认或返回关闭  方向键继续");
    }else if(u->pending){
        const char *progress="命令处理中，请等待";
        if(u->pending_type==PANEL_CMD_SCAN_CANCEL)progress="正在停止搜索";
        else if(u->pending_type==PANEL_CMD_SCAN)progress=s->scanning?"正在搜索，返回键停止":"等待开始搜索";
        else if(u->pending_type==PANEL_CMD_WRITE){
            if(verify_output)progress="正在检测输出电压";
            else if(u->page!=PANEL_PAGE_POWER)progress="正在保存并检查结果";
        }
        snprintf(v->footer,sizeof(v->footer),"%s",progress);
    }else if(u->message[0] && !u->blocked)snprintf(v->footer,sizeof(v->footer),"%s",u->message);
    else if(s->command_busy)snprintf(v->footer,sizeof(v->footer),"其他操作进行中，可浏览或返回");
    else if(u->show_recovery && !u->blocked)snprintf(v->footer,sizeof(v->footer),"连接已恢复，可继续操作");
    for(unsigned i=0;i<7;i++){
        size_t n=strlen(v->values[i]);
        if(n && v->values[i][n-1]=='*'){
            v->values[i][n-1]=0;v->stale_rows|=(uint8_t)(1U<<i);
        }
    }
    if(v->setting_list && !v->result && (!setting_notice || access==PANEL_ACCESS_OFFLINE) &&
       v->stale_rows && !u->pending && !u->message[0] && !u->show_recovery)
        snprintf(v->footer,sizeof(v->footer),"%s",panel_online(s)?"显示上次读取值":"通信已断开，显示上次数据");
    if(v->setting_list && !v->result && !setting_notice && *selected_help && !v->stale_rows &&
       !u->pending && !s->command_busy && !u->message[0] && !u->show_recovery){
        snprintf(v->footer,sizeof(v->footer),"%s",selected_help);
        setting_notice=true;
    }
    v->footer_notice=setting_notice || u->pending || (!u->blocked && (u->message[0] || u->show_recovery)) || s->command_busy ||
        (v->setting_list && v->stale_rows);
    v->language_en=u->language_en;
    if(u->language_en){
        panel_locale_english(v->title,sizeof(v->title));panel_locale_english(v->subtitle,sizeof(v->subtitle));
        panel_locale_english(v->footer,sizeof(v->footer));
        for(unsigned i=0;i<7;i++){panel_locale_english(v->rows[i],sizeof(v->rows[i]));panel_locale_english(v->values[i],sizeof(v->values[i]));}
    }
    return true;
}

void panel_ui_draw(const panel_view_t *v,uint16_t *pixels,int y0,int height)
{
    if(!v || !pixels || y0<0 || height<=0 || y0+height>240)return;
    panel_theme_t t=panel_theme(v->theme_light);
    direct_gfx_t g;direct_gfx_begin(&g,pixels,320,height,y0,t.background);g.language_zh=!v->language_en;
    direct_gfx_rect(&g,0,0,320,30,t.surface);
    direct_gfx_rect(&g,10,9,3,12,t.accent);
    direct_gfx_rect(&g,8,29,304,1,t.line);
    direct_gfx_text(&g,19,5,v->title,16,t.text);
    char detail_status[sizeof(v->subtitle)+16];
    const char *subtitle=v->subtitle;
    if(v->detail_dashboard && v->stale_rows){
        snprintf(detail_status,sizeof(detail_status),"%s  %s",subtitle,v->language_en?"Last data":"上次数据");
        subtitle=detail_status;
    }
    direct_gfx_text(&g,12,34,subtitle,16,v->alert?t.warning:t.muted);
    if(v->setting_groups || v->detail_groups){
        bool settings=v->setting_groups;
        unsigned columns=settings?PANEL_SETTING_GROUP_COLUMNS:PANEL_DETAIL_COLUMNS,groups=settings?PANEL_SETTING_GROUPS:PANEL_DETAIL_PAGES;
        int card_width=settings?96:148,card_height=settings?48:36;
        for(unsigned group=0;group<groups;group++){
            const int x=8+(int)(group%columns)*(card_width+8),y=56+(int)(group/columns)*(card_height+4);
            const char *name=settings?panel_setting_group_label(group,v->language_en):panel_detail_group_label(group,v->language_en);
            bool selected=(int)group==v->selected;
            panel_card(&g,x,y,card_width,card_height,selected?t.selected:t.surface);
            int width=direct_gfx_text_width(name,16);
            direct_gfx_text(&g,x+(card_width-width)/2,y+(card_height-20)/2,name,16,selected?t.accent:t.text);
        }
    }
    for(int row=0;!v->setting_groups && !v->detail_groups && row<7;row++){
        int y=57+22*row;
        if(v->result && row==2){
            uint16_t ink=v->alert?t.warning:t.accent;
            panel_card(&g,8,y-4,304,24,v->alert?t.warning_surface:t.selected);
            if(v->alert){
                direct_gfx_rect(&g,20,y+2,2,7,ink);
                direct_gfx_rect(&g,20,y+11,2,2,ink);
            }else{
                direct_gfx_line(&g,17,y+8,21,y+12,ink);
                direct_gfx_line(&g,21,y+12,28,y+4,ink);
            }
            direct_gfx_text(&g,36,y,v->rows[row],16,ink);
            direct_gfx_rect(&g,14,y+18,292-v->focus_frame*16,1,ink);
            continue;
        }
        bool selected=row==v->selected;
        if(selected || (v->rows[row][0] && v->values[row][0])){
            panel_card(&g,8,y-1,304,20,selected?t.selected:t.surface);
            if(selected)direct_gfx_rect(&g,10,y+3,2+v->focus_frame,11,t.accent);
        }
        direct_gfx_text(&g,17,y,v->rows[row],16,selected?t.accent:t.text);
        const char *value=v->values[row];
        if(*value){
            bool old_or_missing=(v->stale_rows&(1U<<row)) || !strcmp(value,"--");
            direct_gfx_text(&g,305-direct_gfx_text_width(value,16),y,value,16,
                            old_or_missing?t.muted:selected?t.accent:t.text);
        }
    }
    direct_gfx_rect(&g,0,215,320,25,t.surface);
    direct_gfx_rect(&g,8,215,304,1,t.line);
    direct_gfx_text(&g,10,218,v->footer,16,t.muted);
    if(v->activity_frame)for(unsigned i=0;i<3;i++)
        direct_gfx_rect(&g,10+(int)i*101,237,96,2,i+1U==v->activity_frame?t.accent:t.line);
}

