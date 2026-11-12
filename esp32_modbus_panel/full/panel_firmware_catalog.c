#include "panel_firmware_catalog.h"
#include "mqtt_handler.h"
#include "cJSON.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

enum { REQUEST_TIMEOUT_MS=15000, MANIFEST_AGE_MS=300000, RESPONSE_MAX=1800 };
static StaticSemaphore_t mutex_store;
static SemaphoreHandle_t mutex;
static panel_firmware_catalog_snapshot_t view;
static OtaManifest manifest;
static char request_id[17];
static uint32_t epoch,sent_ms,received_ms;
static bool connected,started;
/* Implemented by the full MQTT owner: nonblocking QoS1, retain=false, no cache. */
extern bool full_panel_rpc_publish(const char *payload,size_t length);
static uint32_t clock_ms(void){return (uint32_t)(esp_timer_get_time()/1000);}
static bool lock(void){return mutex && xSemaphoreTake(mutex,pdMS_TO_TICKS(50))==pdTRUE;}
static void unlock(void){xSemaphoreGive(mutex);}
static void state(panel_catalog_state_t value){view.state=value;view.revision++;}

void panel_firmware_catalog_init(void)
{
    /* Startup owner calls once, before any MQTT event or LCD interaction. */
    if(!mutex)mutex=xSemaphoreCreateMutexStatic(&mutex_store);
}
void panel_firmware_catalog_network(bool online)
{
    if(!lock())return;
    connected=online;epoch++;request_id[0]=0;
    if(view.state!=PANEL_CATALOG_IDLE)state(online?PANEL_CATALOG_IDLE:PANEL_CATALOG_OFFLINE);
    unlock();
}
static bool request(unsigned index,bool refresh)
{
    if(ota_service_is_busy() || !lock())return false;
    if(!connected){state(PANEL_CATALOG_OFFLINE);unlock();return false;}
    if(!refresh && (view.state!=PANEL_CATALOG_READY || index>=view.total)){unlock();return false;}
    snprintf(request_id,sizeof(request_id),"%08lx%08lx",(unsigned long)esp_random(),(unsigned long)esp_random());
    char query[256];
    int length=snprintf(query,sizeof(query),"{\"v\":1,\"data\":{\"type\":\"catalog.query\",\"v\":1,\"request_id\":\"%s\",\"sn\":\"%s\",\"epoch\":%lu,\"index\":%u,\"target\":\"esp\"}}",
        request_id,g_device_sn,(unsigned long)epoch,index);
    memset(&manifest,0,sizeof(manifest));started=false;
    view.index=index;view.version[0]=view.changelog[0]=0;view.file_size=0;view.current_version=false;
    if(refresh)view.total=0;
    sent_ms=clock_ms();state(PANEL_CATALOG_LOADING);
    bool ok=length>0 && length<(int)sizeof(query) && full_panel_rpc_publish(query,(size_t)length);
    if(!ok){request_id[0]=0;state(PANEL_CATALOG_ERROR);}
    unlock();return ok;
}
bool panel_firmware_catalog_refresh(void){return request(0,true);}
bool panel_firmware_catalog_select(unsigned index){return request(index,false);}
void panel_firmware_catalog_process(void)
{
    if(!lock())return;
    if(view.state==PANEL_CATALOG_LOADING && clock_ms()-sent_ms>=REQUEST_TIMEOUT_MS){request_id[0]=0;state(PANEL_CATALOG_TIMEOUT);}
    unlock();
}
bool panel_firmware_catalog_snapshot(panel_firmware_catalog_snapshot_t *out)
{
    if(!out || !lock())return false;
    *out=view;unlock();return true;
}
static bool text(cJSON *root,const char *key,char *out,size_t size,bool optional)
{
    const cJSON *v=cJSON_GetObjectItemCaseSensitive(root,key);
    if(!v && optional){out[0]=0;return true;}
    if(!cJSON_IsString(v) || !v->valuestring || strlen(v->valuestring)>=size)return false;
    memcpy(out,v->valuestring,strlen(v->valuestring)+1);return true;
}
static bool number(cJSON *root,const char *key,uint32_t *out)
{
    const cJSON *v=cJSON_GetObjectItemCaseSensitive(root,key);
    if(!cJSON_IsNumber(v) || !isfinite(v->valuedouble) || v->valuedouble<0 ||
       v->valuedouble>UINT32_MAX || floor(v->valuedouble)!=v->valuedouble)return false;
    *out=(uint32_t)v->valuedouble;return true;
}
static bool hash_valid(const char *hash)
{
    if(strlen(hash)!=64)return false;
    for(unsigned i=0;i<64;i++)if(!((hash[i]>='0'&&hash[i]<='9')||(hash[i]>='a'&&hash[i]<='f')))return false;
    return true;
}
static size_t version_length(const char *version)
{
    size_t n=strlen(version);
    static const char suffix[]="-modbus-full";
    if(n>=sizeof(suffix)-1 && !strcmp(version+n-(sizeof(suffix)-1),suffix))n-=sizeof(suffix)-1;
    return n;
}
bool panel_firmware_catalog_receive(const uint8_t *payload,size_t length,bool retained)
{
    if(!payload || !length || length>2048)return false;
    cJSON *root=cJSON_ParseWithLength((const char *)payload,length);
    cJSON *action=cJSON_GetObjectItemCaseSensitive(root,"action");
    if(!cJSON_IsString(action)||strcmp(action->valuestring,"catalog")){cJSON_Delete(root);return false;}
    if(retained || length>RESPONSE_MAX || !lock()){cJSON_Delete(root);return true;}
    char id[17],sn[SN_MAX_LEN],status[8];uint32_t version,wire_epoch,index,total;
    bool correlated=connected && view.state==PANEL_CATALOG_LOADING && clock_ms()-sent_ms<REQUEST_TIMEOUT_MS &&
        text(root,"request_id",id,sizeof(id),false)&&!strcmp(id,request_id) &&
        text(root,"sn",sn,sizeof(sn),false)&&!strcmp(sn,g_device_sn) &&
        number(root,"v",&version)&&version==1 && number(root,"epoch",&wire_epoch)&&wire_epoch==epoch &&
        number(root,"index",&index)&&index==view.index;
    if(!correlated){unlock();cJSON_Delete(root);return true;}
    bool valid=text(root,"status",status,sizeof(status),false)&&!strcmp(status,"ok")&&
        number(root,"total",&total)&&total<=1000 && (!total || index<total);
    cJSON *f=cJSON_GetObjectItemCaseSensitive(root,"firmware");
    if(valid && total){
        valid=cJSON_IsObject(f) && text(f,"version",manifest.version,sizeof(manifest.version),false) &&
            text(f,"target_chip",view.target,sizeof(view.target),false)&&!strcmp(view.target,"esp") &&
            text(f,"model",manifest.model,sizeof(manifest.model),false) &&
            text(f,"hardware",manifest.hardware,sizeof(manifest.hardware),true) &&
            text(f,"file_url",manifest.url,sizeof(manifest.url),false)&&!strncmp(manifest.url,"https://",8) &&
            text(f,"file_sha256",manifest.sha256,sizeof(manifest.sha256),false)&&hash_valid(manifest.sha256) &&
            text(f,"release_signature",manifest.signature,sizeof(manifest.signature),true) &&
            text(f,"changelog",view.changelog,sizeof(view.changelog),true) &&
            number(f,"file_size",&manifest.file_size)&&manifest.file_size>0&&manifest.file_size<=0x1a0000 &&
            number(f,"security_version",&manifest.security_version);
    }
    request_id[0]=0;
    if(valid){
        view.total=total;
        if(total){
            strcpy(view.version,manifest.version);view.file_size=manifest.file_size;
            size_t n=version_length(view.version);
            view.current_version=n==version_length(FIRMWARE_VERSION)&&!strncmp(view.version,FIRMWARE_VERSION,n);
            manifest.source=OTA_SOURCE_LOCAL;manifest.target=OTA_TARGET_ESP;manifest.timeout_seconds=300;
            snprintf(manifest.task_id,sizeof(manifest.task_id),"panel-%s",id);
            received_ms=clock_ms();
        }
        state(total?PANEL_CATALOG_READY:PANEL_CATALOG_EMPTY);
    }else{memset(&manifest,0,sizeof(manifest));state(PANEL_CATALOG_ERROR);}
    unlock();cJSON_Delete(root);return true;
}
OtaResult panel_firmware_catalog_start(void)
{
    if(!lock())return OTA_RESULT_BUSY;
    OtaResult result=OTA_RESULT_INVALID_PARAMS;
    if(started)result=OTA_RESULT_DUPLICATE;
    else if(!connected)result=OTA_RESULT_HTTP_ERROR;
    else if(view.state==PANEL_CATALOG_READY && clock_ms()-received_ms>=MANIFEST_AGE_MS){state(PANEL_CATALOG_TIMEOUT);result=OTA_RESULT_TIMEOUT;}
    else if(view.state==PANEL_CATALOG_READY && !view.current_version && clock_ms()-received_ms<MANIFEST_AGE_MS){
        started=true;
        /* Lock protects the current manifest from replacement while OTA copies
         * it. Its observer cannot re-enter the catalog (source is LOCAL). */
        result=ota_service_start_remote(&manifest);
        if(result!=OTA_RESULT_OK && result!=OTA_RESULT_DUPLICATE)started=false;
    }
    unlock();return result;
}
