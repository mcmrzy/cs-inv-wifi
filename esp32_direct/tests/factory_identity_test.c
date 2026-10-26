#include "factory_identity.h"
#include "nvs.h"
#include "freertos/semphr.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint8_t blobs[4][FACTORY_RECORD_WIRE_SIZE];static size_t sizes[4];static bool locked;
static char admin_sn[17];
esp_err_t nvs_get_str(nvs_handle_t h,const char*k,char*out,size_t*n){(void)h;assert(!strcmp(k,"admin_sn"));if(!admin_sn[0])return ESP_ERR_NVS_NOT_FOUND;if(*n<strlen(admin_sn)+1)return ESP_ERR_NVS_INVALID_LENGTH;*n=strlen(admin_sn)+1;memcpy(out,admin_sn,*n);return 0;}
esp_err_t nvs_set_str(nvs_handle_t h,const char*k,const char*sn){(void)h;assert(!strcmp(k,"admin_sn"));snprintf(admin_sn,sizeof(admin_sn),"%s",sn);return 0;}
esp_err_t nvs_erase_key(nvs_handle_t h,const char*k){(void)h;if(!strcmp(k,"admin_sn")){if(!admin_sn[0])return ESP_ERR_NVS_NOT_FOUND;admin_sn[0]=0;return 0;}int i=2+k[4]-'0';assert(i>=2&&i<4);sizes[i]=0;return 0;}
esp_err_t nvs_flash_init_partition(const char*p){assert(!strcmp(p,"factory"));return 0;}
esp_err_t nvs_open_from_partition(const char*p,const char*n,int mode,nvs_handle_t*h){assert(!strcmp(p,"factory")&&!strcmp(n,"identity")&&mode==1);*h=1;return 0;}
esp_err_t nvs_commit(nvs_handle_t h){(void)h;return 0;}void nvs_close(nvs_handle_t h){(void)h;}
esp_err_t nvs_get_u8(nvs_handle_t h,const char*k,uint8_t*v){(void)h;assert(!strcmp(k,"locked"));if(!locked)return ESP_ERR_NVS_NOT_FOUND;*v=1;return 0;}
esp_err_t nvs_set_u8(nvs_handle_t h,const char*k,uint8_t v){(void)h;assert(!strcmp(k,"locked")&&v==1);locked=true;return 0;}
esp_err_t nvs_get_blob(nvs_handle_t h,const char*k,void*out,size_t*n){(void)h;int i=!strncmp(k,"pair",4)?2+k[4]-'0':k[6]-'0';assert(i>=0&&i<4);if(!sizes[i])return ESP_ERR_NVS_NOT_FOUND;if(*n<sizes[i])return ESP_ERR_NVS_INVALID_LENGTH;memcpy(out,blobs[i],sizes[i]);*n=sizes[i];return 0;}
esp_err_t nvs_set_blob(nvs_handle_t h,const char*k,const void*in,size_t n){(void)h;int i=!strncmp(k,"pair",4)?2+k[4]-'0':k[6]-'0';assert(i>=0&&i<4&&n<=FACTORY_RECORD_WIRE_SIZE);memcpy(blobs[i],in,n);sizes[i]=n;return 0;}
esp_err_t esp_read_mac(uint8_t*out,int t){(void)t;uint8_t mac[]={2,3,4,5,6,7};memcpy(out,mac,6);return 0;}
SemaphoreHandle_t xSemaphoreCreateMutex(void){return (void*)1;}int xSemaphoreTake(SemaphoreHandle_t h,unsigned n){(void)h;(void)n;return 1;}void xSemaphoreGive(SemaphoreHandle_t h){(void)h;}
int main(void){factory_record_t r={0},got;esp_read_mac(r.mac,0);strcpy(r.sn,"H1ZZX0013900001P");strcpy(r.product,"CSERGY");strcpy(r.model,"CS-L10-6K2");strcpy(r.hw_system,"UNSET");strcpy(r.hw_controller,"UNSET");strcpy(r.batch,"test");strcpy(r.production_time,"2026-10-12T00:00:00Z");assert(factory_identity_init()==FACTORY_OK);assert(!factory_identity_ready());char serial[17];assert(factory_identity_set_admin_sn("TEST123",false)==FACTORY_UNPROVISIONED);assert(factory_identity_set_admin_sn("TEST123",true)==FACTORY_OK);assert(factory_identity_admin_sn(serial,sizeof(serial))&&!strcmp(serial,"TEST123"));assert(factory_identity_provision(&r)==FACTORY_OK);assert(!factory_identity_admin_sn(serial,sizeof(serial)));assert(factory_identity_set_admin_sn("NEW123",false)==FACTORY_OK);assert(factory_identity_read(&got)==FACTORY_OK&&!strcmp(got.sn,"NEW123")&&!strcmp(got.model,r.model));strcpy(r.sn,"NEW123");assert(factory_identity_admin_sn(serial,sizeof(serial))&&!strcmp(serial,"NEW123"));assert(factory_identity_lock()==FACTORY_OK);assert(factory_identity_set_admin_sn("OTHER",true)==FACTORY_LOCKED);assert(locked);blobs[1][40]^=1;assert(factory_identity_read(&got)==FACTORY_OK&&got.locked);assert(factory_identity_provision(&r)==FACTORY_OK);strcpy(r.sn,"OTHER");assert(factory_identity_provision(&r)==FACTORY_LOCKED);puts("PASS actual identity/NVS adapter + sticky lock survives corrupt newest slot");}
