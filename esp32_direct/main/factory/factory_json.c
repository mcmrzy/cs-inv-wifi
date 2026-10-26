#include "factory_json.h"
#include <string.h>

static bool text(cJSON *object, const char *key, const char *value, bool nullable)
{
    return nullable && (!value || !value[0] || !strcmp(value,"UNSET")) ?
        cJSON_AddNullToObject(object,key)!=NULL :
        cJSON_AddStringToObject(object,key,value ? value : "")!=NULL;
}

#define BASE_FIELDS(X) X(sn) X(product) X(model) X(hw_system) X(hw_controller) \
    X(hw_dsp) X(hw_bms) X(batch) X(production_time) X(calibration_version) X(calibration_summary)
#define BOARD_FIELDS(X) X(controller_type) X(controller_code) X(controller_sn) \
    X(dsp_type) X(dsp_code) X(dsp_sn) X(bms_type) X(bms_code) X(bms_sn) \
    X(power_type) X(power_code) X(power_sn) X(hw_power) X(work_order) X(station_id)

bool factory_record_json(cJSON *object, const factory_record_t *r)
{
    if(!object || !r)return false;
#define ADD(field) if(!text(object,#field,r->field,false))return false;
    BASE_FIELDS(ADD)
    BOARD_FIELDS(ADD)
#undef ADD
    return cJSON_AddNumberToObject(object,"schema",r->schema ? r->schema : FACTORY_RECORD_SCHEMA) &&
        cJSON_AddNumberToObject(object,"generation",r->generation) &&
        cJSON_AddBoolToObject(object,"locked",r->locked);
}

static bool board(cJSON *boards, const char *role, const char *type,
                  const char *code, const char *hardware, const char *sn)
{
    cJSON *b=cJSON_CreateObject();
    if(!b)return false;
    if(!cJSON_AddItemToArray(boards,b)){cJSON_Delete(b);return false;}
    return text(b,"role",role,false) && text(b,"type",type,true) &&
        text(b,"code",code,true) && text(b,"hardware_version",hardware,true) &&
        text(b,"sn",sn,true);
}

cJSON *factory_info_json(const char *runtime_sn, const char *firmware,
                         const char *mac, factory_result_t status,
                         const factory_record_t *record, uint32_t runtime_generation,
                         bool pending_override)
{
    cJSON *root=cJSON_CreateObject();
    if(!root)return NULL;
    static const factory_record_t empty={0};
    const factory_record_t *r=status==FACTORY_OK && record ? record : &empty;
    bool valid=status==FACTORY_OK && record;
    bool match=valid && !strcmp(r->sn,runtime_sn);
    bool pending=pending_override || (valid && (!match || r->generation!=runtime_generation));
#define NEED(expr) do {if(!(expr))goto fail;}while(0)
    NEED(text(root,"sn",runtime_sn,false));
    NEED(text(root,"model",valid && !pending ? r->model : "CS-L10-6K2",false));
    NEED(text(root,"manufacturer","辰烁科技",false));
    NEED(text(root,"firmware_esp",firmware,false));
    NEED(text(root,"chip","esp32c3",false));
    NEED(text(root,"mac",mac,false));
    NEED(text(root,"device_type","off_grid_inverter",false));
    NEED(text(root,"phase","single",false));
    NEED(text(root,"hardware_version",valid && !pending ? r->hw_system : NULL,true));
    static const char *unknown[]={"firmware_arm","firmware_dsp","firmware_bms","rated_power",
        "rated_voltage","rated_frequency","battery_nominal_voltage","battery_type",
        "cell_count","temp_sensor_count","inverter_module","bootloader_version"};
    for(unsigned i=0;i<sizeof(unknown)/sizeof(unknown[0]);i++)NEED(cJSON_AddNullToObject(root,unknown[i]));
    NEED(cJSON_AddBoolToObject(root,"dsp_identity_available",false));
    NEED(text(root,"dsp_protocol","direct_uart_03_04_25",false));
    cJSON *id=cJSON_AddObjectToObject(root,"factory_identity");NEED(id);
    NEED(text(id,"status",factory_result_name(status),false));
    NEED(cJSON_AddBoolToObject(id,"runtime_match",match));
    NEED(cJSON_AddBoolToObject(id,"pending_reboot",pending));
    NEED(cJSON_AddNumberToObject(id,"runtime_generation",runtime_generation));
    if(valid){
        NEED(cJSON_AddNumberToObject(id,"schema",r->schema));
        NEED(cJSON_AddNumberToObject(id,"generation",r->generation));
        NEED(cJSON_AddBoolToObject(id,"locked",r->locked));
    }else{
        NEED(cJSON_AddNullToObject(id,"schema"));
        NEED(cJSON_AddNullToObject(id,"generation"));
        NEED(cJSON_AddNullToObject(id,"locked"));
    }
#define ID(field) NEED(text(id,#field,r->field,true));
    ID(sn) ID(product) ID(model) ID(batch) ID(production_time)
    ID(work_order) ID(station_id) ID(calibration_version) ID(calibration_summary)
#undef ID
    cJSON *boards=cJSON_AddArrayToObject(root,"boards");NEED(boards);
    NEED(board(boards,"controller",r->controller_type,r->controller_code,r->hw_controller,r->controller_sn));
    NEED(board(boards,"dsp",r->dsp_type,r->dsp_code,r->hw_dsp,r->dsp_sn));
    NEED(board(boards,"bms",r->bms_type,r->bms_code,r->hw_bms,r->bms_sn));
    NEED(board(boards,"power",r->power_type,r->power_code,r->hw_power,r->power_sn));
#undef NEED
    return root;
fail:
    cJSON_Delete(root);return NULL;
}
