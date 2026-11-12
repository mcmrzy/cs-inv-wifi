#include "panel_ble.h"
#include "panel_ble_aux.h"
#include "ble_ct_cmd.h"
#include "panel_bus.h"
#include "panel_catalog.h"
#include "panel_locale.h"
#include "ota_service.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>

/* All three entry points run in the application's BLE dispatcher task. */
static panel_snapshot_t snapshot;
static struct {
    char id[11];
    uint32_t token,sequence,context,started_ms;
    uint16_t value;
    bool active, configuration;
} pending;

panel_snapshot_t *panel_ble_snapshot(void)
{ return panel_bus_snapshot(&snapshot)?&snapshot:NULL; }

static bool get_uint(const cJSON *item,uint32_t max,uint32_t *out)
{
    if(!cJSON_IsNumber(item) || !(item->valuedouble>=0 && item->valuedouble<=max))return false;
    uint32_t value=(uint32_t)item->valuedouble;
    if(item->valuedouble!=value)return false;
    *out=value;
    return true;
}

bool panel_ble_read_uint(const cJSON *params,const char *key,uint32_t max,uint32_t *out)
{
    return get_uint(cJSON_GetObjectItemCaseSensitive(params,key),max,out);
}

bool panel_ble_english(const cJSON *params)
{
    return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(params,"en"));
}

static const char *localized_result(const cJSON *params,cJSON *actual,const char *error)
{
    if(error || !panel_ble_english(params))return error;
    cJSON *field;
    cJSON_ArrayForEach(field,actual)if(cJSON_IsString(field)){
        char text[128];
        snprintf(text,sizeof(text),"%s",field->valuestring);
        panel_locale_english(text,sizeof(text));
        if(!cJSON_SetValuestring(field,text))return "RESULT_UNAVAILABLE";
    }
    return NULL;
}

static uint16_t register_id(const char *id)
{
    if(strncmp(id,"panel_",6) || strlen(id)!=10)return 0;
    static const char hex[]="0123456789abcdef";
    uint16_t reg=0;
    for(unsigned i=6;i<10;i++){
        const char *digit=strchr(hex,id[i]);
        if(!digit)return 0;
        reg=(uint16_t)((reg<<4)+(unsigned)(digit-hex));
    }
    return reg;
}

static const char *panel_info(cJSON *actual)
{
    panel_profile_t configured=snapshot.config.profile_override;
    bool confirmed=panel_profile_supported(configured);
    panel_profile_t model=confirmed?configured:snapshot.detected_profile;
    return cJSON_AddStringToObject(actual,"model",panel_profile_name(model)) &&
        cJSON_AddNumberToObject(actual,"count",panel_setting_count()) &&
        cJSON_AddBoolToObject(actual,"online",panel_online(&snapshot)) &&
        cJSON_AddNumberToObject(actual,"context",snapshot.context_generation) &&
        cJSON_AddBoolToObject(actual,"confirmed",confirmed) &&
        cJSON_AddBoolToObject(actual,"power_on",panel_setting_allowed(&snapshot,0x4123,1)) &&
        cJSON_AddBoolToObject(actual,"power_off",panel_setting_allowed(&snapshot,0x4123,2))
        ?NULL:"RESULT_UNAVAILABLE";
}

static bool current_setting(const panel_setting_t *setting,uint16_t *value,bool *observed)
{
    uint16_t address=panel_setting_current_address(&snapshot,setting),raw;
    if(address!=0xffff && panel_fresh(&snapshot,address) && panel_value(&snapshot,address,&raw) &&
       panel_setting_decode_current(&snapshot,setting->address,raw,value)){
        *observed=true;
        return true;
    }
    const panel_setting_read_t *read=panel_setting_read_find(&snapshot,setting->address);
    if(!read || snapshot.now_ms-read->stamp>PANEL_FRESH_MS || read->state!=PANEL_SETTING_VALUE)
        return false;
    *value=read->value;
    *observed=read->independent;
    return true;
}

static const char *setting_info(const panel_setting_t *setting,const cJSON *params,cJSON *actual)
{
    char id[11],text[64]="--";
    uint16_t value=0;
    bool observed=false;
    bool have=panel_online(&snapshot) && current_setting(setting,&value,&observed);
    bool reading=false;
    const cJSON *refresh=cJSON_GetObjectItemCaseSensitive(params,"refresh");
    if(refresh && !cJSON_IsBool(refresh))return "INVALID_ARGS";
    if(cJSON_IsTrue(refresh) && !have && setting->address!=0x4123 && panel_online(&snapshot)){
        const panel_setting_read_t *read=panel_setting_read_find(&snapshot,setting->address);
        if(!read || read->state==PANEL_SETTING_UNREAD || snapshot.now_ms-read->stamp>PANEL_FRESH_MS){
            panel_bus_request_ble_setting(setting->address);
            reading=true;
        }
    }
    if(have)panel_setting_format(&snapshot,setting->address,value,text,sizeof(text));
    uint16_t suggestion=cJSON_IsTrue(refresh)?panel_ble_battery_suggestion(&snapshot,setting->address):0;
    if(suggestion && suggestion!=value){
        char suggested_text[64];
        panel_setting_format(&snapshot,setting->address,suggestion,suggested_text,sizeof(suggested_text));
        if(!cJSON_AddNumberToObject(actual,"suggestion",suggestion) ||
           !cJSON_AddStringToObject(actual,"suggested_text",suggested_text))return "RESULT_UNAVAILABLE";
    }
    snprintf(id,sizeof(id),"panel_%04x",(unsigned)setting->address);
    panel_setting_access_t access=panel_setting_access(&snapshot,setting->address);
    return cJSON_AddStringToObject(actual,"id",id) &&
        cJSON_AddStringToObject(actual,"label",setting->label) &&
        cJSON_AddStringToObject(actual,"group",panel_setting_group_name(panel_setting_group_for(setting->address))) &&
        cJSON_AddNumberToObject(actual,"access",access) &&
        (have?cJSON_AddNumberToObject(actual,"value",value):cJSON_AddNullToObject(actual,"value")) &&
        cJSON_AddStringToObject(actual,"text",text) &&
        (access==PANEL_ACCESS_EDITABLE ||
         cJSON_AddStringToObject(actual,"reason",panel_setting_restriction(&snapshot,setting->address))) &&
        cJSON_AddNumberToObject(actual,"context",snapshot.context_generation) &&
        cJSON_AddBoolToObject(actual,"reading",reading) &&
        cJSON_AddBoolToObject(actual,"observed",observed)
        ?NULL:"RESULT_UNAVAILABLE";
}

static const char *step_setting(const panel_setting_t *setting,const cJSON *params,cJSON *actual)
{
    const cJSON *direction=cJSON_GetObjectItemCaseSensitive(params,"direction");
    const cJSON *value=cJSON_GetObjectItemCaseSensitive(params,"value");
    const cJSON *context=cJSON_GetObjectItemCaseSensitive(params,"context");
    uint32_t current=0xffff,expected;
    if(context && (!get_uint(context,UINT32_MAX,&expected) || expected!=snapshot.context_generation))
        return "REVISION_CONFLICT";
    if(!cJSON_IsNumber(direction) || !value ||
        (!cJSON_IsNull(value) && !get_uint(value,UINT16_MAX,&current)))return "INVALID_ARGS";
    double d=direction->valuedouble;
    if(d!=1 && d!=-1 && d!=10 && d!=-10)return "INVALID_ARGS";
    if(panel_setting_access(&snapshot,setting->address)!=PANEL_ACCESS_EDITABLE)return "UNSUPPORTED_PARAM";
    int sign=d>0?1:-1;
    unsigned steps=d==1 || d==-1?1:10;
    uint16_t next=(uint16_t)current;
    bool changed=false;
    for(unsigned i=0;i<steps;i++){
        if(!panel_setting_step(&snapshot,setting->address,next,sign,&next))break;
        changed=true;
    }
    if(!changed)return "OUT_OF_RANGE";
    char text[64];
    panel_setting_format(&snapshot,setting->address,next,text,sizeof(text));
    return cJSON_AddNumberToObject(actual,"value",next) &&
        cJSON_AddStringToObject(actual,"text",text) &&
        cJSON_AddNumberToObject(actual,"context",snapshot.context_generation)?NULL:"RESULT_UNAVAILABLE";
}

static const char *read_panel(const char *action,const cJSON *params,cJSON *actual)
{
    bool info=!strcmp(action,"panel_info");
    bool setting=!strcmp(action,"panel_setting");
    bool step=!strcmp(action,"panel_step");
    if(!info && !setting && !step)
        return localized_result(params,actual,panel_ble_aux_read(action,params,actual));
    if(params && !cJSON_IsObject(params))return "INVALID_ARGS";
    if(!panel_ble_snapshot())return "NOT_READY";
    if(info)return localized_result(params,actual,panel_info(actual));
    uint32_t index=0;
    if(!panel_ble_read_uint(params,"index",(uint32_t)panel_setting_count()-1U,&index))
        return "INVALID_ARGS";
    const panel_setting_t *item=panel_setting(index);
    if(!item)return "INVALID_ARGS";
    const char *error=step?step_setting(item,params,actual):setting_info(item,params,actual);
    return localized_result(params,actual,error);
}

static int configure_or_scan(const char *id, double value, const cJSON *params, uint32_t token)
{
    bool scan = !strcmp(id, "panel_scan");
    uint32_t context, sequence;
    if (!panel_ble_read_uint(params, "context", UINT32_MAX, &context))
        return BLE_CMD_REVISION_CONFLICT;
    if (!panel_ble_snapshot()) return BLE_CMD_NOT_READY;
    if (context != snapshot.context_generation) return BLE_CMD_REVISION_CONFLICT;

    panel_command_t command = {0};
    if (scan) {
        if (value != 0 && value != 1) return BLE_CMD_OUT_OF_RANGE;
        command.type = value == 1 ? PANEL_CMD_SCAN : PANEL_CMD_SCAN_CANCEL;
    } else {
        uint32_t address, baud, parity, profile;
        if (value != 1 || !panel_ble_read_uint(params, "address", 247, &address) ||
            !panel_ble_read_uint(params, "baud", 115200, &baud) ||
            !panel_ble_read_uint(params, "parity", 2, &parity) ||
            !panel_ble_read_uint(params, "profile", PANEL_PROFILE_COUNT - 1, &profile))
            return BLE_CMD_OUT_OF_RANGE;
        command.type = PANEL_CMD_CONFIG;
        command.config = (panel_config_t){.address = address, .baud = baud, .parity = parity, .profile_override = profile};
        if (!panel_config_valid(&command.config)) return BLE_CMD_OUT_OF_RANGE;
        if (snapshot.profile_locked && profile != (uint32_t)snapshot.config.profile_override)
            return BLE_CMD_UNSUPPORTED;
    }
    if (!panel_bus_submit_in_context(&command, context, &sequence))
        return context == panel_bus_context_generation() ? BLE_CMD_BUSY : BLE_CMD_REVISION_CONFLICT;
    if (scan) {
        ble_ct_cmd_on_panel_write_result(token, id, NULL, false, true, value);
        return BLE_CMD_OK;
    }
    memcpy(pending.id, "panel_comm", sizeof(pending.id));
    pending.token = token;
    pending.sequence = sequence;
    pending.context = context;
    pending.started_ms = snapshot.now_ms;
    pending.value = 1;
    pending.configuration = true;
    pending.active = true;
    return BLE_CMD_OK;
}

static int write_panel(const char *id,double value,const cJSON *params,uint32_t token)
{
    if(ota_service_is_busy())return BLE_CMD_UPGRADE_BUSY;
    if(pending.active)return BLE_CMD_BUSY;
    if (!strcmp(id, "panel_comm") || !strcmp(id, "panel_scan"))
        return configure_or_scan(id, value, params, token);
    uint16_t address=register_id(id);
    if(!address){
        int rc=panel_ble_aux_write(id,value);
        if(rc==BLE_CMD_OK)ble_ct_cmd_on_panel_write_result(token,id,NULL,true,true,value);
        return rc==-100?BLE_CMD_UNSUPPORTED:rc;
    }
    if(!panel_setting_find(address))return BLE_CMD_UNSUPPORTED;
    if(!(value>=0 && value<UINT16_MAX) || value!=(uint16_t)value)return BLE_CMD_OUT_OF_RANGE;
    uint32_t context,sequence;
    if(!panel_ble_read_uint(params,"context",UINT32_MAX,&context))
        return BLE_CMD_REVISION_CONFLICT;
    if(!panel_ble_snapshot() || !panel_online(&snapshot))return BLE_CMD_NOT_READY;
    if(context!=snapshot.context_generation)return BLE_CMD_REVISION_CONFLICT;
    if(snapshot.command_busy || snapshot.scanning)return BLE_CMD_BUSY;
    if(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(params,"bms_suggestion")) &&
       (!value || value!=panel_ble_battery_suggestion(&snapshot,address)))return BLE_CMD_REVISION_CONFLICT;
    if(!panel_setting_allowed(&snapshot,address,(uint16_t)value)){
        panel_setting_access_t access=panel_setting_access(&snapshot,address);
        return access==PANEL_ACCESS_MODEL?BLE_CMD_PROFILE_REQUIRED:
            access==PANEL_ACCESS_EDITABLE?BLE_CMD_OUT_OF_RANGE:BLE_CMD_UNSUPPORTED;
    }
    panel_command_t command={.type=PANEL_CMD_WRITE,.address=address,.value=(uint16_t)value};
    if(!panel_bus_submit_in_context(&command,context,&sequence)){
        return context==panel_bus_context_generation()?BLE_CMD_BUSY:BLE_CMD_REVISION_CONFLICT;
    }
    memcpy(pending.id,id,sizeof(pending.id));
    pending.token=token;
    pending.sequence=sequence;
    pending.context=context;
    pending.started_ms=snapshot.now_ms;
    pending.value=command.value;
    pending.configuration=false;
    pending.active=true;
    return BLE_CMD_OK;
}

void panel_ble_init(void)
{
    ble_ct_cmd_set_panel_executors(read_panel,write_panel);
}

void panel_ble_tick(void)
{
    if(!pending.active || !panel_ble_snapshot())return;
    const panel_result_t *result=panel_result_find(&snapshot,pending.sequence);
    const char *error=NULL;
    bool observed=false,submitted=false;
    if(pending.configuration && result){
        if(result->state==PANEL_WRITE_IDLE)observed=submitted=true;
        else error="STORAGE_FAILED";
    }else if(snapshot.context_generation!=pending.context && !pending.configuration)error="REVISION_CONFLICT";
    else if(result){
        switch(result->state){
        case PANEL_WRITE_CONFIRMED:observed=submitted=true;break;
        case PANEL_WRITE_PROTOCOL_ACCEPTED:submitted=true;break;
        case PANEL_WRITE_PROCESSED:submitted=true;error="VALUE_MISMATCH";break;
        case PANEL_WRITE_REJECTED:error="INVERTER_REJECTED";break;
        default:error="SYNC_TIMEOUT";break;
        }
    }else if(snapshot.now_ms-pending.started_ms>=25000U ||
             ((int32_t)(snapshot.command_sequence-pending.sequence)>0 && !snapshot.command_busy))
        error="SYNC_TIMEOUT";
    else return;
    ble_ct_cmd_on_panel_write_result(pending.token,pending.id,error,observed,submitted,pending.value);
    pending.active=false;
}
