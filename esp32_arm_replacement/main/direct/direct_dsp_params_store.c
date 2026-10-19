#include "direct_dsp_params_store.h"
#include "nvs.h"
#include "esp_log.h"
static const char *TAG="dsp_params";
static bool legacy_preferences(direct_dsp_params_t *params){
    nvs_handle_t h;if(nvs_open("direct_ui",NVS_READONLY,&h)!=ESP_OK)return false;
    const char *keys[]={"alarm","blank","buzzer"};const uint16_t indexes[]={14,15,41};bool found=false;
    for(unsigned i=0;i<3;i++){uint16_t value;if(nvs_get_u16(h,keys[i],&value)==ESP_OK && value<=1 && direct_dsp_params_set(params,indexes[i],value))found=true;}
    nvs_close(h);return found;
}
bool direct_dsp_params_bootstrap(const char *sn,direct_dsp_params_t *params,bool *defaults_used){
    if(!params || !defaults_used)return false;
    *defaults_used=false;
    nvs_handle_t handle;uint8_t record[DIRECT_DSP_PARAMS_RECORD_SIZE];size_t length=sizeof(record);
    esp_err_t err=nvs_open("dsp_params",NVS_READONLY,&handle);
    if(err==ESP_OK){
        err=nvs_get_blob(handle,"record",record,&length);nvs_close(handle);
        if(err==ESP_OK){
            direct_dsp_params_t loaded;
            if(direct_dsp_params_unpack(record,length,sn,&loaded)){
                if(record[4]==1 && legacy_preferences(&loaded) && !direct_dsp_params_save(sn,&loaded))return false;
                *params=loaded;return true;
            }
            ESP_LOGW(TAG,"Invalid DSP record (SN/CRC/format/range); restoring ARM defaults");
        }
    }
    if(err!=ESP_OK && err!=ESP_ERR_NVS_NOT_FOUND && err!=ESP_ERR_NVS_INVALID_LENGTH){
        ESP_LOGE(TAG,"DSP parameter storage read/open failed: %d; polling remains blocked",(int)err);
        return false;
    }
    if(err==ESP_ERR_NVS_NOT_FOUND)ESP_LOGW(TAG,"DSP parameter record missing; initializing ARM defaults");
    if(err==ESP_ERR_NVS_INVALID_LENGTH)ESP_LOGW(TAG,"DSP parameter record too large; restoring ARM defaults");
    direct_dsp_params_t defaults;
    /* A successful bootstrap is treated as durable by direct_poll. Never enable
     * the replacement in RAM until its full SN-bound record has been committed. */
    if(!direct_dsp_params_arm_defaults(&defaults) || !direct_dsp_params_save(sn,&defaults)){
        ESP_LOGE(TAG,"Failed to save ARM defaults; polling remains blocked");
        return false;
    }
    ESP_LOGW(TAG,"ARM default parameters saved; startup 00/01/02 sync enabled");
    *params=defaults;*defaults_used=true;return true;
}
bool direct_dsp_params_load(const char *sn,direct_dsp_params_t *params){
    nvs_handle_t handle;uint8_t record[DIRECT_DSP_PARAMS_RECORD_SIZE];size_t length=sizeof(record);
    if(nvs_open("dsp_params",NVS_READONLY,&handle)!=ESP_OK)return false;
    esp_err_t err=nvs_get_blob(handle,"record",record,&length);nvs_close(handle);
    return err==ESP_OK && direct_dsp_params_unpack(record,length,sn,params);
}
bool direct_dsp_params_save(const char *sn,const direct_dsp_params_t *params){
    uint8_t record[DIRECT_DSP_PARAMS_RECORD_SIZE];nvs_handle_t handle;
    if(!direct_dsp_params_pack(params,sn,record) || nvs_open("dsp_params",NVS_READWRITE,&handle)!=ESP_OK)return false;
    esp_err_t err=nvs_set_blob(handle,"record",record,sizeof(record));
    if(err==ESP_OK)err=nvs_commit(handle);
    nvs_close(handle);return err==ESP_OK;
}
