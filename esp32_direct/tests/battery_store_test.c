#include "direct_dsp_params_store.h"
#include "nvs.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint8_t base[92],extra[DIRECT_DSP_PROFILES_RECORD_SIZE],next_base[92],next_extra[DIRECT_DSP_PROFILES_RECORD_SIZE];
static bool have_base,have_extra;static int fault;static unsigned commits;
esp_err_t nvs_open(const char *name,int mode,nvs_handle_t *h){(void)mode;if(strcmp(name,"dsp_params"))return ESP_ERR_NVS_NOT_FOUND;*h=1;return 0;}
void nvs_close(nvs_handle_t h){(void)h;}
esp_err_t nvs_get_u16(nvs_handle_t h,const char *k,uint16_t *v){(void)h;(void)k;(void)v;return ESP_ERR_NVS_NOT_FOUND;}
esp_err_t nvs_get_blob(nvs_handle_t h,const char *key,void *out,size_t *n){
    (void)h;bool primary=!strcmp(key,"record");size_t len=primary?sizeof(base):sizeof(extra);
    if(!(primary?have_base:have_extra))return ESP_ERR_NVS_NOT_FOUND;
    if(*n<len){*n=len;return ESP_ERR_NVS_INVALID_LENGTH;}
    memcpy(out,primary?base:extra,len);*n=len;return 0;
}
esp_err_t nvs_set_blob(nvs_handle_t h,const char *key,const void *data,size_t n){
    (void)h;bool primary=!strcmp(key,"record");assert(n==(primary?sizeof(base):sizeof(extra)));
    if(fault==(primary?1:2))return -1;
    memcpy(primary?next_base:next_extra,data,n);return 0;
}
esp_err_t nvs_commit(nvs_handle_t h){(void)h;commits++;if(fault==3)return -1;
    memcpy(base,next_base,sizeof(base));memcpy(extra,next_extra,sizeof(extra));have_base=have_extra=true;return 0;}
int main(void){
    const char *sn="H1ZZX00139000079";direct_dsp_params_t p,loaded;
    assert(direct_dsp_params_arm_defaults(&p));assert(direct_dsp_params_save(sn,&p));
    direct_battery_config_t c;assert(direct_battery_preset(4,0,&c));
    assert(direct_dsp_params_battery_set(&p,&c)==DIRECT_BATTERY_OK);p.revision=2;
    assert(direct_dsp_params_save(sn,&p));bool defaults=true;
    assert(direct_dsp_params_bootstrap(sn,&loaded,&defaults) && !defaults);
    assert(loaded.battery_profiles_valid==p.battery_profiles_valid);
    assert(direct_dsp_params_charge_reply(&loaded,(uint8_t[11]){0})==11);
    /* The downgrade mirror is still a legacy V3 blob, including active Li. */
    direct_dsp_params_t legacy;assert(direct_dsp_params_unpack(base,sizeof(base),sn,&legacy) && legacy.frames[12]==4);
    direct_dsp_params_t before=loaded;assert(direct_battery_preset(1,0,&c));
    assert(direct_dsp_params_battery_set(&p,&c)==DIRECT_BATTERY_OK);p.revision=3;
    for(fault=1;fault<=3;fault++){
        assert(!direct_dsp_params_save(sn,&p));assert(direct_dsp_params_load(sn,&loaded));
        assert(!memcmp(&loaded,&before,sizeof(loaded)));
    }
    fault=0;
    /* A torn multi-key write must never boot into mixed settings or defaults. */
    assert(direct_dsp_params_pack(&p,sn,base));unsigned prior=commits;
    assert(!direct_dsp_params_bootstrap(sn,&loaded,&defaults) && !defaults && commits==prior);
    assert(direct_dsp_params_save(sn,&p));
    assert(direct_dsp_params_bootstrap(sn,&loaded,&defaults) && !defaults && loaded.frames[12]==1);
    have_extra=false;assert(direct_dsp_params_load(sn,&loaded) && !loaded.battery_profiles_valid);
    puts("PASS battery NVS: complete reboot restore, V3 downgrade mirror, both set/commit failures, torn records fail closed, legacy loading");
}
