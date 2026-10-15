#include "factory_identity.h"
#include "factory_store.h"
#include "factory_pairing_record.h"
#include <limits.h>
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>
static SemaphoreHandle_t mutex;
static nvs_handle_t handle;
static uint8_t mac[6];
static bool initialized;
static const char *keys[]={"record0","record1"};
static bool read_slot(void *ctx,int slot,uint8_t *out,size_t *n){
 (void)ctx;esp_err_t err=nvs_get_blob(handle,keys[slot],out,n);
 if(err==ESP_ERR_NVS_NOT_FOUND){*n=0;return true;}
 if(err==ESP_ERR_NVS_INVALID_LENGTH){/* oversized/corrupt peer must not hide valid slot */*n=1;out[0]=0;return true;}
 return err==ESP_OK;
}
static bool write_slot(void *ctx,int slot,const uint8_t *in,size_t n){
 (void)ctx;if(nvs_set_blob(handle,keys[slot],in,n)!=ESP_OK)return false;
 esp_err_t err=nvs_erase_key(handle,"admin_sn");
 return (err==ESP_OK||err==ESP_ERR_NVS_NOT_FOUND)&&nvs_commit(handle)==ESP_OK;
}
/* Clear before identity mutation. A interrupted transition may lose pairing,
 * but must never resurrect credentials when an old SN is reused. */
static factory_result_t clear_pairing(void){
 for(int i=0;i<2;i++){esp_err_t e=nvs_erase_key(handle,i?"pair1":"pair0");if(e!=ESP_OK&&e!=ESP_ERR_NVS_NOT_FOUND)return FACTORY_STORAGE_ERROR;}
 return nvs_commit(handle)==ESP_OK?FACTORY_OK:FACTORY_STORAGE_ERROR;
}
static const factory_store_io_t io={read_slot,write_slot,NULL};
static factory_result_t sticky_lock(bool *locked){
 uint8_t value=0;esp_err_t err=nvs_get_u8(handle,"locked",&value);
 if(err==ESP_ERR_NVS_NOT_FOUND){*locked=false;return FACTORY_OK;}
 if(err!=ESP_OK||value!=1)return FACTORY_STORAGE_ERROR;
 *locked=true;return FACTORY_OK;
}
factory_result_t factory_identity_init(void){
 if(initialized)return FACTORY_OK;
 if(nvs_flash_init_partition("factory")!=ESP_OK)return FACTORY_STORAGE_ERROR; /* NEVER erase */
 if(nvs_open_from_partition("factory","identity",NVS_READWRITE,&handle)!=ESP_OK)return FACTORY_STORAGE_ERROR;
 if(esp_read_mac(mac,ESP_MAC_WIFI_STA)!=ESP_OK){nvs_close(handle);return FACTORY_STORAGE_ERROR;}
 mutex=xSemaphoreCreateMutex();if(!mutex){nvs_close(handle);return FACTORY_STORAGE_ERROR;}
 initialized=true;return FACTORY_OK;
}
static bool take(void){return initialized&&xSemaphoreTake(mutex,pdMS_TO_TICKS(100))==pdTRUE;}
factory_result_t factory_identity_read(factory_record_t *r){
 if(!initialized)return FACTORY_NOT_INITIALIZED;
 if(!take())return FACTORY_BUSY;
 factory_result_t res=factory_store_read(&io,mac,r,NULL);
 bool locked=false;if(res==FACTORY_OK){res=sticky_lock(&locked);if(locked)r->locked=true;}
 xSemaphoreGive(mutex);return res;
}
factory_result_t factory_identity_provision(const factory_record_t *r){
 if(!initialized)return FACTORY_NOT_INITIALIZED;
 factory_result_t valid=factory_record_validate(r);
 if(valid!=FACTORY_OK)return valid;
 if(memcmp(r->mac,mac,6))return FACTORY_MAC_MISMATCH;
 if(r->locked)return FACTORY_INVALID_ARGUMENT;
 if(!take())return FACTORY_BUSY;
 bool locked=false;factory_result_t res=sticky_lock(&locked);
 if(res==FACTORY_OK&&locked){factory_record_t old;res=factory_store_read(&io,mac,&old,NULL);if(res==FACTORY_OK)res=factory_record_same_identity(&old,r)?FACTORY_OK:FACTORY_LOCKED;}
 else if(res==FACTORY_OK){
  factory_record_t old;factory_result_t prior=factory_store_read(&io,mac,&old,NULL);
  if(prior==FACTORY_OK&&factory_record_same_identity(&old,r))res=FACTORY_OK;
  else if(prior==FACTORY_OK&&old.locked)res=FACTORY_LOCKED;
  else if(prior!=FACTORY_OK&&prior!=FACTORY_UNPROVISIONED)res=prior;
  else{res=clear_pairing();if(res==FACTORY_OK)res=factory_store_provision(&io,mac,r);}
 }
 xSemaphoreGive(mutex);return res;
}
factory_result_t factory_identity_lock(void){
 if(!initialized)return FACTORY_NOT_INITIALIZED;
 if(!take())return FACTORY_BUSY;
 factory_record_t old;factory_result_t res=factory_store_read(&io,mac,&old,NULL);
 /* Sticky lock survives record-slot fallback. Failure after this commit remains
  * locked; it cannot silently reopen provisioning after a corrupt peer. */
 if(res==FACTORY_OK&&(nvs_set_u8(handle,"locked",1)!=ESP_OK||nvs_commit(handle)!=ESP_OK))res=FACTORY_STORAGE_ERROR;
 if(res==FACTORY_OK)res=factory_store_lock(&io,mac);
 xSemaphoreGive(mutex);return res;
}
bool factory_identity_ready(void){factory_record_t r;return factory_identity_read(&r)==FACTORY_OK;}
static bool serial_sn_valid(const char *sn){
 if(!sn)return false;
 size_t n=strlen(sn);if(!n||n>16)return false;
 for(size_t i=0;i<n;i++)if(!((sn[i]>='0'&&sn[i]<='9')||(sn[i]>='A'&&sn[i]<='Z')||(sn[i]>='a'&&sn[i]<='z')))return false;
 return true;
}
bool factory_identity_admin_sn(char *out,size_t capacity){
 if(!out||!capacity||!take())return false;
 size_t n=capacity;bool ok=nvs_get_str(handle,"admin_sn",out,&n)==ESP_OK&&serial_sn_valid(out);
 xSemaphoreGive(mutex);return ok;
}
factory_result_t factory_identity_set_admin_sn(const char *sn,bool allow_unprovisioned){
 if(!serial_sn_valid(sn))return FACTORY_INVALID_ARGUMENT;
 if(!initialized)return FACTORY_NOT_INITIALIZED;
 if(!take())return FACTORY_BUSY;
 bool locked=false;factory_result_t res=sticky_lock(&locked);factory_record_t r;
 if(res==FACTORY_OK){
  res=factory_store_read(&io,mac,&r,NULL);
  if(locked){res=res==FACTORY_OK&&!strcmp(r.sn,sn)?FACTORY_OK:FACTORY_LOCKED;}
  else if(res==FACTORY_OK&&r.locked)res=!strcmp(r.sn,sn)?FACTORY_OK:FACTORY_LOCKED;
  else if(res==FACTORY_OK){
   if(strcmp(r.sn,sn))res=clear_pairing();
   if(res==FACTORY_OK){snprintf(r.sn,sizeof(r.sn),"%s",sn);res=factory_store_provision(&io,mac,&r);}
  }
  else if(res==FACTORY_UNPROVISIONED&&allow_unprovisioned){
   char old_sn[17]={0};size_t n=sizeof(old_sn);
   esp_err_t e=nvs_get_str(handle,"admin_sn",old_sn,&n);
   res=(e==ESP_OK&&!strcmp(old_sn,sn))?FACTORY_OK:clear_pairing();
  }
 }
 if(res==FACTORY_OK){
  if(nvs_set_str(handle,"admin_sn",sn)!=ESP_OK||nvs_commit(handle)!=ESP_OK)res=FACTORY_STORAGE_ERROR;
  else{char actual[17]={0};size_t n=sizeof(actual);if(nvs_get_str(handle,"admin_sn",actual,&n)!=ESP_OK||strcmp(actual,sn))res=FACTORY_READBACK_ERROR;}
 }
 xSemaphoreGive(mutex);return res;
}

static factory_result_t effective_sn(char out[17],bool allow_unprovisioned){
 factory_record_t identity;factory_result_t res=factory_store_read(&io,mac,&identity,NULL);
 if(res!=FACTORY_OK&&!(res==FACTORY_UNPROVISIONED&&allow_unprovisioned))return res;
 size_t n=17;esp_err_t err=nvs_get_str(handle,"admin_sn",out,&n);
 if(err==ESP_OK)return serial_sn_valid(out)?FACTORY_OK:FACTORY_CORRUPT;
 if(err!=ESP_ERR_NVS_NOT_FOUND)return FACTORY_STORAGE_ERROR;
 if(res!=FACTORY_OK)return FACTORY_UNPROVISIONED;
 if(strlen(identity.sn)>16)return FACTORY_INVALID_RECORD;
 memcpy(out,identity.sn,strlen(identity.sn)+1);return FACTORY_OK;
}
static factory_result_t read_pairing(factory_pairing_record_t *r,int *slot){
 uint8_t data[2][FACTORY_PAIRING_WIRE_SIZE];size_t sizes[2]={sizeof(data[0]),sizeof(data[1])};
 for(int i=0;i<2;i++){
  esp_err_t e=nvs_get_blob(handle,i?"pair1":"pair0",data[i],&sizes[i]);
  if(e==ESP_ERR_NVS_NOT_FOUND)sizes[i]=0;
  else if(e==ESP_ERR_NVS_INVALID_LENGTH){sizes[i]=1;data[i][0]=0;}
  else if(e!=ESP_OK)return FACTORY_STORAGE_ERROR;
 }
 factory_result_t res=factory_pairing_select(data[0],sizes[0],data[1],sizes[1],r,slot);
 if(res==FACTORY_OK&&memcmp(r->mac,mac,6))res=FACTORY_MAC_MISMATCH;
 return res;
}
bool factory_identity_pairing(const char *sn,char pin[7]){
 if(pin)memset(pin,0,7);
 if(!sn||!pin||!take())return false;
 char actual[17];factory_pairing_record_t r;
 bool ok=effective_sn(actual,true)==FACTORY_OK&&!strcmp(actual,sn)&&read_pairing(&r,NULL)==FACTORY_OK&&!strcmp(r.sn,sn);
 if(ok)memcpy(pin,r.pin,7);
 xSemaphoreGive(mutex);return ok;
}
factory_result_t factory_identity_set_pairing(const char *sn,const char *pin,bool allow_unprovisioned){
 if(!factory_pairing_values_valid(sn,pin))return FACTORY_INVALID_ARGUMENT;
 if(!initialized)return FACTORY_NOT_INITIALIZED;
 if(!take())return FACTORY_BUSY;
 char actual[17];factory_result_t res=effective_sn(actual,allow_unprovisioned);
 if(res==FACTORY_OK&&strcmp(actual,sn))res=FACTORY_INVALID_ARGUMENT;
 bool locked=false;if(res==FACTORY_OK)res=sticky_lock(&locked);
 factory_pairing_record_t old,next={0};int selected=-1;
 if(res==FACTORY_OK){
  factory_result_t prior=read_pairing(&old,&selected);
  if(prior==FACTORY_OK&&!strcmp(old.sn,sn)&&!strcmp(old.pin,pin)){xSemaphoreGive(mutex);return FACTORY_OK;}
  if(locked)res=FACTORY_LOCKED;
  else if(prior!=FACTORY_OK&&prior!=FACTORY_UNPROVISIONED)res=prior;
  else if(prior==FACTORY_OK&&old.generation==UINT32_MAX)res=FACTORY_GENERATION_EXHAUSTED;
  else next.generation=prior==FACTORY_OK?old.generation+1:1;
 }
 if(res==FACTORY_OK){
  memcpy(next.mac,mac,6);memcpy(next.sn,sn,17);memcpy(next.pin,pin,7);
  uint8_t wire[FACTORY_PAIRING_WIRE_SIZE],check[FACTORY_PAIRING_WIRE_SIZE];size_t n=sizeof(check);
  res=factory_pairing_encode(&next,wire,sizeof(wire));
  const char *key=selected==0?"pair1":"pair0";
  if(res==FACTORY_OK&&(nvs_set_blob(handle,key,wire,sizeof(wire))!=ESP_OK||nvs_commit(handle)!=ESP_OK))res=FACTORY_STORAGE_ERROR;
  if(res==FACTORY_OK&&(nvs_get_blob(handle,key,check,&n)!=ESP_OK||n!=sizeof(wire)||memcmp(check,wire,n)))res=FACTORY_READBACK_ERROR;
 }
 xSemaphoreGive(mutex);return res;
}
