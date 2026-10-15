#include "factory_identity.h"
#include "factory_pairing_record.h"
#include "nvs.h"
#include "freertos/semphr.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint8_t blobs[4][FACTORY_RECORD_WIRE_SIZE];static size_t sizes[4];static bool locked;
static char admin_sn[17];
static bool fail_pair_write, fail_pair_read, corrupt_pair_read;
esp_err_t nvs_get_str(nvs_handle_t h,const char*k,char*out,size_t*n){(void)h;assert(!strcmp(k,"admin_sn"));if(!admin_sn[0])return ESP_ERR_NVS_NOT_FOUND;if(*n<strlen(admin_sn)+1)return ESP_ERR_NVS_INVALID_LENGTH;*n=strlen(admin_sn)+1;memcpy(out,admin_sn,*n);return 0;}
esp_err_t nvs_set_str(nvs_handle_t h,const char*k,const char*sn){(void)h;assert(!strcmp(k,"admin_sn"));snprintf(admin_sn,sizeof(admin_sn),"%s",sn);return 0;}
esp_err_t nvs_erase_key(nvs_handle_t h,const char*k){(void)h;if(!strcmp(k,"admin_sn")){if(!admin_sn[0])return ESP_ERR_NVS_NOT_FOUND;admin_sn[0]=0;return 0;}int i=2+k[4]-'0';assert(i>=2&&i<4);sizes[i]=0;return 0;}
esp_err_t nvs_flash_init_partition(const char*p){assert(!strcmp(p,"factory"));return 0;}
esp_err_t nvs_open_from_partition(const char*p,const char*n,int mode,nvs_handle_t*h){assert(!strcmp(p,"factory")&&!strcmp(n,"identity")&&mode==1);*h=1;return 0;}
esp_err_t nvs_commit(nvs_handle_t h){(void)h;return 0;}void nvs_close(nvs_handle_t h){(void)h;}
esp_err_t nvs_get_u8(nvs_handle_t h,const char*k,uint8_t*v){(void)h;assert(!strcmp(k,"locked"));if(!locked)return ESP_ERR_NVS_NOT_FOUND;*v=1;return 0;}
esp_err_t nvs_set_u8(nvs_handle_t h,const char*k,uint8_t v){(void)h;assert(!strcmp(k,"locked")&&v==1);locked=true;return 0;}
esp_err_t nvs_get_blob(nvs_handle_t h,const char*k,void*out,size_t*n){(void)h;if(!strncmp(k,"pair",4)&&fail_pair_read)return -9;int i=!strncmp(k,"pair",4)?2+k[4]-'0':k[6]-'0';assert(i>=0&&i<4);if(!sizes[i])return ESP_ERR_NVS_NOT_FOUND;if(*n<sizes[i])return ESP_ERR_NVS_INVALID_LENGTH;memcpy(out,blobs[i],sizes[i]);if(i>=2&&corrupt_pair_read)((uint8_t*)out)[0]^=1;*n=sizes[i];return 0;}
esp_err_t nvs_set_blob(nvs_handle_t h,const char*k,const void*in,size_t n){(void)h;if(!strncmp(k,"pair",4)&&fail_pair_write)return -9;int i=!strncmp(k,"pair",4)?2+k[4]-'0':k[6]-'0';assert(i>=0&&i<4&&n<=FACTORY_RECORD_WIRE_SIZE);memcpy(blobs[i],in,n);sizes[i]=n;return 0;}
esp_err_t esp_read_mac(uint8_t*out,int t){(void)t;uint8_t mac[]={2,3,4,5,6,7};memcpy(out,mac,6);return 0;}
SemaphoreHandle_t xSemaphoreCreateMutex(void){return (void*)1;}int xSemaphoreTake(SemaphoreHandle_t h,unsigned n){(void)h;(void)n;return 1;}void xSemaphoreGive(SemaphoreHandle_t h){(void)h;}
int main(void){
 factory_record_t r={0};char pin[7];esp_read_mac(r.mac,0);
 strcpy(r.sn,"H1ZZX0013900001P");strcpy(r.product,"CSERGY");strcpy(r.model,"CS-L10-6K2");strcpy(r.hw_system,"UNSET");strcpy(r.hw_controller,"UNSET");strcpy(r.batch,"test");strcpy(r.production_time,"2026-10-12T00:00:00Z");
 assert(factory_identity_init()==FACTORY_OK);
 assert(factory_identity_set_pairing(r.sn,"000001",true)==FACTORY_UNPROVISIONED);
 assert(factory_identity_set_admin_sn(r.sn,true)==FACTORY_OK);
 assert(factory_identity_set_pairing(r.sn,"000001",false)==FACTORY_UNPROVISIONED);
 assert(factory_identity_set_pairing(r.sn,"000001",true)==FACTORY_OK);
 assert(factory_identity_pairing(r.sn,pin)&&!strcmp(pin,"000001"));
 assert(factory_identity_set_admin_sn(r.sn,true)==FACTORY_OK);assert(factory_identity_pairing(r.sn,pin));
 assert(factory_identity_set_admin_sn("H1ZZX00139000000",true)==FACTORY_OK);assert(!factory_identity_pairing(r.sn,pin));
 assert(factory_identity_set_admin_sn(r.sn,true)==FACTORY_OK);assert(!factory_identity_pairing(r.sn,pin));
 assert(factory_identity_set_pairing(r.sn,"000002",true)==FACTORY_OK);
 assert(factory_identity_provision(&r)==FACTORY_OK);assert(!factory_identity_pairing(r.sn,pin));
 assert(factory_identity_set_pairing(r.sn,"000003",false)==FACTORY_OK);
 fail_pair_write=true;assert(factory_identity_set_pairing(r.sn,"000004",false)==FACTORY_STORAGE_ERROR);fail_pair_write=false;
 assert(factory_identity_pairing(r.sn,pin)&&!strcmp(pin,"000003"));
 fail_pair_read=true;assert(!factory_identity_pairing(r.sn,pin));assert(!pin[0]);fail_pair_read=false;
 assert(factory_identity_set_pairing("H1ZZX00139000000","000004",false)==FACTORY_INVALID_ARGUMENT);
 assert(factory_identity_set_pairing(r.sn,"000004",false)==FACTORY_OK);
 assert(factory_identity_pairing(r.sn,pin)&&!strcmp(pin,"000004"));
 /* Interrupted newest slot: complete older generation survives. */
 sizes[3]=11;assert(factory_identity_pairing(r.sn,pin)&&!strcmp(pin,"000003"));
 assert(factory_identity_set_pairing(r.sn,"000005",false)==FACTORY_OK);
 /* Actual encoded foreign MAC is rejected by identity adapter, not CRC. */
 factory_pairing_record_t foreign;assert(factory_pairing_decode(blobs[3],sizes[3],&foreign)==FACTORY_OK);
 foreign.mac[5]++;assert(factory_pairing_encode(&foreign,blobs[3],sizes[3])==FACTORY_OK);
 assert(!factory_identity_pairing(r.sn,pin));assert(factory_identity_set_pairing(r.sn,"000006",false)==FACTORY_MAC_MISMATCH);
 foreign.mac[5]--;assert(factory_pairing_encode(&foreign,blobs[3],sizes[3])==FACTORY_OK);
 factory_pairing_record_t decoded;assert(factory_pairing_decode(blobs[3],sizes[3],&decoded)==FACTORY_OK);
 decoded.generation=UINT32_MAX;assert(factory_pairing_encode(&decoded,blobs[3],sizes[3])==FACTORY_OK);
 assert(factory_identity_set_pairing(r.sn,"000006",false)==FACTORY_GENERATION_EXHAUSTED);
 assert(factory_identity_lock()==FACTORY_OK);
 assert(factory_identity_set_pairing(r.sn,"000005",false)==FACTORY_OK);
 assert(factory_identity_set_pairing(r.sn,"000006",false)==FACTORY_LOCKED);
 assert(factory_identity_set_admin_sn("H1ZZX00139000000",true)==FACTORY_LOCKED);
 blobs[1][40]^=1;assert(factory_identity_set_pairing(r.sn,"000006",false)==FACTORY_LOCKED);
 puts("PASS pairing identity persistence, SN reuse invalidation, partial-slot fallback, exhausted counter, sticky lock");
}
