#include "cloud_binding.h"
#include <stdint.h>
#include <string.h>

/* Policy functions are shared with the host regression, not replicas. */
typedef struct {
 bool session, pending;
 unsigned failures;
 uint32_t last_attempt, started, interval;
} binding_policy_t;
static uint32_t retry_delay(unsigned failures){return failures<=1?30000U:failures==2?60000U:300000U;}
static void policy_failed(binding_policy_t *p,uint32_t now){
 p->pending=false;if(p->failures<3)p->failures++;p->last_attempt=now;p->interval=retry_delay(p->failures);
}
static bool policy_due(binding_policy_t *p,bool ready,uint32_t now){
 if(!ready){memset(p,0,sizeof(*p));return false;}
 if(!p->session){p->session=true;return true;}
 if(p->pending){if((uint32_t)(now-p->started)>=10000U)policy_failed(p,now);else return false;}
 return (uint32_t)(now-p->last_attempt)>=p->interval;
}
static void policy_sent(binding_policy_t *p,bool success,uint32_t now){
 p->last_attempt=now;if(!success){policy_failed(p,now);return;}
 p->pending=true;p->started=now;
}
static bool policy_nonce_live(const binding_policy_t *p,uint32_t now){return p->session&&p->pending&&(uint32_t)(now-p->started)<10000U;}
static void policy_answered(binding_policy_t *p,uint32_t now){p->pending=false;p->failures=0;p->last_attempt=now;p->interval=300000U;}
static bool revision_allowed(bool have,uint64_t old_revision,cloud_binding_state_t old_state,uint64_t next_revision,cloud_binding_state_t next_state){
 return !have||next_revision>old_revision||(next_revision==old_revision&&next_state==old_state);
}
static bool public_key_valid(const char *key){
 if(!key||strlen(key)!=64)return false;
 bool nonzero=false;for(unsigned i=0;i<64;i++){char c=key[i];if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))return false;nonzero|=c!='0';}return nonzero;
}

#ifndef CLOUD_BINDING_POLICY_ONLY
#include "cloud_binding_codec.h"
#include "mqtt_handler.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "nvs.h"
#include "cJSON.h"
#include <stdatomic.h>
#include <stdio.h>
#ifndef CONFIG_DIRECT_BINDING_PUBLIC_KEY
#define CONFIG_DIRECT_BINDING_PUBLIC_KEY ""
#endif
typedef struct {char json[512];} binding_message_t;
static QueueHandle_t queue;
static atomic_int state;
static atomic_bool subscription_ready;
static atomic_uint subscription_epoch;
static char nonce[33];
static uint64_t revision;
static bool have_revision;
static nvs_handle_t cache;
static const char *str(cJSON *obj,const char *name){cJSON *v=cJSON_GetObjectItemCaseSensitive(obj,name);return cJSON_IsString(v)?v->valuestring:NULL;}
static cloud_binding_state_t parsed_state(cJSON *root){const char *s=str(root,"state");return !strcmp(s,"BOUND")?CLOUD_BINDING_BOUND:!strcmp(s,"UNBOUND")?CLOUD_BINDING_UNBOUND:CLOUD_BINDING_UNKNOWN;}
static bool validate(cJSON *root,uint64_t *rev){
 const char *sn=str(root,"sn"),*s=str(root,"state"),*r=str(root,"revision"),*n=str(root,"nonce"),*sig=str(root,"signature");
 cJSON *key_id=cJSON_GetObjectItemCaseSensitive(root,"key_id");
 return sn&&!strcmp(sn,g_device_sn)&&cJSON_IsNumber(key_id)&&key_id->valuedouble==1&&
 cloud_binding_verify(sn,s,r,n,sig,CONFIG_DIRECT_BINDING_PUBLIC_KEY,rev);
}
static void apply(cJSON *root,uint64_t rev){atomic_store(&state,parsed_state(root));revision=rev;have_revision=true;}
static void worker(void *arg){
 (void)arg;binding_policy_t policy={0};unsigned epoch=atomic_load(&subscription_epoch);
 for(;;){
  bool ready=mqtt_is_connected()&&atomic_load(&subscription_ready);uint32_t now=(uint32_t)(esp_timer_get_time()/1000);
  unsigned current_epoch=atomic_load(&subscription_epoch);
  if(current_epoch!=epoch){memset(&policy,0,sizeof(policy));nonce[0]=0;epoch=current_epoch;}
  if(policy_due(&policy,ready,now)){
   uint8_t random[16];esp_fill_random(random,sizeof(random));for(unsigned i=0;i<16;i++)snprintf(nonce+2*i,3,"%02x",random[i]);
   /* Existing MQTT publisher sends {t,v,data}; server reads data.sn/data.nonce. */
   char request[112];snprintf(request,sizeof(request),"{\"sn\":\"%s\",\"nonce\":\"%s\"}",g_device_sn,nonce);
   policy_sent(&policy,mqtt_publish_volatile("binding/query",request,0,1)==0,now);
  }
  if(!ready||!policy_nonce_live(&policy,now))nonce[0]=0;
  binding_message_t message;
  if(xQueueReceive(queue,&message,pdMS_TO_TICKS(250))==pdTRUE){
   now=(uint32_t)(esp_timer_get_time()/1000);
   cJSON *root=cJSON_Parse(message.json);uint64_t rev;
   const char *incoming=root?str(root,"nonce"):NULL;
   if(mqtt_is_connected()&&atomic_load(&subscription_ready)&&atomic_load(&subscription_epoch)==epoch&&policy_nonce_live(&policy,now)&&incoming&&!strcmp(incoming,nonce)&&validate(root,&rev)&&
      revision_allowed(have_revision,revision,(cloud_binding_state_t)atomic_load(&state),rev,parsed_state(root))){
    bool stored=have_revision&&rev==revision;
    if(!stored)stored=nvs_set_str(cache,"signed_state",message.json)==ESP_OK&&nvs_commit(cache)==ESP_OK;
    if(stored){apply(root,rev);policy_answered(&policy,now);nonce[0]=0;}
   }
   cJSON_Delete(root);
  }
 }
}
bool cloud_binding_start(void){
 if(queue)return true;
 if(!public_key_valid(CONFIG_DIRECT_BINDING_PUBLIC_KEY))return false;
 if(nvs_open("cloud_binding",NVS_READWRITE,&cache)!=ESP_OK)return false;
 /* Synchronous load before panel startup; SN/signature isolate cached identity. */
 char stored[512];size_t size=sizeof(stored);
 if(nvs_get_str(cache,"signed_state",stored,&size)==ESP_OK){cJSON *r=cJSON_Parse(stored);uint64_t rev;if(r&&validate(r,&rev))apply(r,rev);cJSON_Delete(r);}
 queue=xQueueCreate(2,sizeof(binding_message_t));if(!queue){nvs_close(cache);return false;}
 if(xTaskCreate(worker,"binding",4096,NULL,1,NULL)!=pdPASS){vQueueDelete(queue);queue=NULL;nvs_close(cache);return false;}return true;
}
void cloud_binding_subscription(bool ready){if(!ready)atomic_fetch_add(&subscription_epoch,1);atomic_store(&subscription_ready,ready);}
cloud_binding_state_t cloud_binding_get(void){return (cloud_binding_state_t)atomic_load(&state);}
void cloud_binding_receive(const char *topic,const char *payload,size_t n){
 if(!queue||!topic||!payload||!n||n>=512)return;
 char expected[80];snprintf(expected,sizeof(expected),"cs_inv/%s/binding/state",g_device_sn);if(strcmp(topic,expected))return;
 binding_message_t message;memcpy(message.json,payload,n);message.json[n]=0;xQueueSend(queue,&message,0);
}
#endif
