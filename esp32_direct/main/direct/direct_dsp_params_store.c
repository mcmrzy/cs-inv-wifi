#include "direct_dsp_params_store.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>
static const char *TAG="dsp_params";
/* NVS does not promise power-loss atomicity across keys. The extended record
 * contains its complete V3 mirror and is accepted only when both agree. */
static bool profiles_load(nvs_handle_t h,const char *sn,const uint8_t base[DIRECT_DSP_PARAMS_RECORD_SIZE],direct_dsp_params_t *params){
    uint8_t record[DIRECT_DSP_PROFILES_RECORD_SIZE];size_t length=sizeof(record);
    esp_err_t err=nvs_get_blob(h,"battery",record,&length);
    if(err==ESP_ERR_NVS_NOT_FOUND)return true; /* exact legacy migration */
    if(err!=ESP_OK)return false;
    direct_dsp_params_t extended;
    if(!direct_dsp_params_profiles_unpack(record,length,sn,&extended))return false;
    if(memcmp(base,record,DIRECT_DSP_PARAMS_RECORD_SIZE)){
        /* A legacy record with no configured profiles has no additional state
         * to lose. Once battery profiles exist, mismatched revisions stop sync. */
        return !extended.battery_profiles_valid;
    }
    *params=extended;return true;
}
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
        err=nvs_get_blob(handle,"record",record,&length);
        if(err==ESP_OK){
            direct_dsp_params_t loaded;
            if(direct_dsp_params_unpack(record,length,sn,&loaded)){
                uint8_t base[DIRECT_DSP_PARAMS_RECORD_SIZE];
                bool valid=direct_dsp_params_pack(&loaded,sn,base) && profiles_load(handle,sn,base,&loaded);
                nvs_close(handle);
                if(!valid){ESP_LOGE(TAG,"Battery profile record is invalid/mismatched; preserving data, DSP sync blocked");return false;}
                if(record[4]==1 && legacy_preferences(&loaded) && !direct_dsp_params_save(sn,&loaded))return false;
                *params=loaded;return true;
            }
            /* A configured battery record is not an excuse to initialize AGM
             * when the compatibility mirror was damaged or partially written. */
            uint8_t profile[DIRECT_DSP_PROFILES_RECORD_SIZE];size_t n=sizeof(profile);
            esp_err_t profile_err=nvs_get_blob(handle,"battery",profile,&n);
            if(profile_err!=ESP_ERR_NVS_NOT_FOUND && (profile_err!=ESP_OK ||
               n!=sizeof(profile) || (profile[95]&0xfeU))){nvs_close(handle);return false;}
            ESP_LOGW(TAG,"Invalid DSP record (SN/CRC/format/range); restoring ARM defaults");
        }
        if(err!=ESP_OK){
            uint8_t profile[DIRECT_DSP_PROFILES_RECORD_SIZE];size_t n=sizeof(profile);
            esp_err_t profile_err=nvs_get_blob(handle,"battery",profile,&n);
            if(profile_err!=ESP_ERR_NVS_NOT_FOUND && (profile_err!=ESP_OK ||
               n!=sizeof(profile) || (profile[95]&0xfeU))){nvs_close(handle);return false;}
        }
        nvs_close(handle);
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
    esp_err_t err=nvs_get_blob(handle,"record",record,&length);
    direct_dsp_params_t loaded;uint8_t base[DIRECT_DSP_PARAMS_RECORD_SIZE];
    bool valid=err==ESP_OK && direct_dsp_params_unpack(record,length,sn,&loaded) &&
        direct_dsp_params_pack(&loaded,sn,base) && profiles_load(handle,sn,base,&loaded);
    nvs_close(handle);if(valid)*params=loaded;return valid;
}
bool direct_dsp_params_save(const char *sn,const direct_dsp_params_t *params){
    uint8_t record[DIRECT_DSP_PROFILES_RECORD_SIZE];nvs_handle_t handle;
    if(!direct_dsp_params_profiles_pack(params,sn,record) || nvs_open("dsp_params",NVS_READWRITE,&handle)!=ESP_OK)return false;
    /* Publish the full profile first. An interrupted compatibility-mirror
     * update is detected on boot instead of silently enabling a partial Li. */
    esp_err_t err=nvs_set_blob(handle,"battery",record,sizeof(record));
    if(err==ESP_OK)err=nvs_set_blob(handle,"record",record,DIRECT_DSP_PARAMS_RECORD_SIZE);
    if(err==ESP_OK)err=nvs_commit(handle);
    nvs_close(handle);return err==ESP_OK;
}
