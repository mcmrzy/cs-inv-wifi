#include "full_alarm_service.h"
#include "panel_alarm.h"
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <time.h>
#ifdef PANEL_ALARM_HOST
#include "alarm_service_test_sdk.h"
#else
#include "panel_bus.h"
#include "config.h"
#include "mqtt_handler.h"
#include "ota_service.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#endif
typedef struct {int id;uint32_t generation;} alarm_ack_t;
static panel_alarm_store_t s_store __attribute__((aligned(4)));
static StaticQueue_t s_ack_control;
static uint8_t s_ack_storage[16*sizeof(alarm_ack_t)];
static QueueHandle_t s_acks;
static atomic_uint s_generation;
static nvs_handle_t s_nvs;
static char s_key[12],s_json[160];
static bool s_ready,s_dirty,s_write_error,s_enqueue_error,s_clock_waiting;
static int s_inflight=-1;
static uint32_t s_inflight_serial,s_inflight_generation,s_sent_ms,s_boot_serial,s_started_ms;
static uint32_t s_retry_ms,s_seen_generation,s_log_ms;
static uint32_t s_resume_ms;
static uint32_t s_health_ms;
static bool s_was_paused;
static const char *TAG="mb_alarm";
static uint32_t clock_ms(void){return (uint32_t)(esp_timer_get_time()/1000);}
void full_alarm_transport_reset(void){atomic_fetch_add(&s_generation,1);}
static void transport_notice(int id)
{
    if(!s_acks)return;
    alarm_ack_t ack={id,atomic_load(&s_generation)};
    /* A lost ACK must not strand the journal forever. Replay is safe because
     * the persisted envelope keeps the same event timestamp and identity. */
    if(xQueueSend(s_acks,&ack,0)!=pdTRUE)full_alarm_transport_reset();
}
void full_alarm_puback(int id){if(id>=0)transport_notice(id);}
void full_alarm_expired(int id){if(id>=0)transport_notice(~id);}
bool full_alarm_needs_attention(void)
{
    return !s_ready || s_write_error || s_enqueue_error || (s_store.sys_active&2U) ||
        (s_inflight>=0 && clock_ms()-s_sent_ms>10000U) ||
        (s_clock_waiting && clock_ms()-s_started_ms>30000U);
}
static bool checkpoint(void)
{
    if(!s_dirty)return true;
    panel_alarm_seal(&s_store);
    esp_err_t err=nvs_set_blob(s_nvs,s_key,&s_store,sizeof(s_store));
    if(err==ESP_OK)err=nvs_commit(s_nvs);
    s_write_error=err!=ESP_OK;
    if(s_write_error){
        if((int32_t)(clock_ms()-s_log_ms)>=0){ESP_LOGE(TAG,"Journal commit failed: %s; pending=%u retained, no new publish",esp_err_to_name(err),s_store.count);s_log_ms=clock_ms()+30000U;}
        s_retry_ms=clock_ms()+1000U;return false;
    }
    s_dirty=false;return true;
}
bool full_alarm_init(void)
{
    s_started_ms=clock_ms();
    s_acks=xQueueCreateStatic(16,sizeof(alarm_ack_t),s_ack_storage,&s_ack_control);
    if(!s_acks || !g_device_sn[0])return false;
    uint32_t hash=2166136261U;
    for(const char *p=g_device_sn;*p;p++)hash=(hash^(uint8_t)*p)*16777619U;
    snprintf(s_key,sizeof(s_key),"r%08lx",(unsigned long)hash);
    esp_err_t err=nvs_open("mb_alarm",NVS_READWRITE,&s_nvs);
    if(err!=ESP_OK){ESP_LOGE(TAG,"Journal open failed: %s",esp_err_to_name(err));return false;}
    size_t bytes=sizeof(s_store);err=nvs_get_blob(s_nvs,s_key,&s_store,&bytes);
    if(err==ESP_ERR_NVS_NOT_FOUND){
        s_key[0]='q';
        bytes=sizeof(s_store);
        err=nvs_get_blob(s_nvs,s_key,&s_store,&bytes);
        s_key[0]='r';
    }
    if(err==ESP_ERR_NVS_NOT_FOUND){
        panel_alarm_init(&s_store);snprintf(s_store.identity,sizeof(s_store.identity),"%s",g_device_sn);s_dirty=true;
    }else if(err!=ESP_OK || !panel_alarm_restore(&s_store,bytes) ||
             strncmp(s_store.identity,g_device_sn,sizeof(s_store.identity))){
        ESP_LOGE(TAG,"Journal invalid or identity mismatch; retained for diagnosis, collection disabled");
        nvs_close(s_nvs);return false;
    }else s_dirty=bytes!=sizeof(s_store);
    s_boot_serial=s_store.next_serial;s_ready=true;
    (void)checkpoint();
    ESP_LOGI(TAG,"RAW Modbus alarms ready: pending=%u lost_bursts=%lu V1 topic=alarm journal=%u bytes",
        s_store.count,(unsigned long)s_store.loss_count,(unsigned)sizeof(s_store));return true;
}
void full_alarm_tick(void)
{
    if(!s_ready)return;
    uint32_t now=clock_ms(),generation=atomic_load(&s_generation);
    alarm_ack_t ack;
    while(xQueueReceive(s_acks,&ack,0)==pdTRUE){
        if(s_inflight>=0 && ack.id==~s_inflight && ack.generation==s_inflight_generation && generation==s_inflight_generation){
            ESP_LOGW(TAG,"SDK expired id=%d; journal retained for replay",s_inflight);
            s_inflight=-1;s_retry_ms=now;s_enqueue_error=true;
        }
        if(s_inflight>=0 && ack.id==s_inflight && ack.generation==s_inflight_generation && generation==s_inflight_generation){
            if(panel_alarm_pop(&s_store,s_inflight_serial))s_dirty=true;
            ESP_LOGI(TAG,"PUBACK id=%d event=%lu; broker received (server ingest not confirmed)",ack.id,(unsigned long)s_inflight_serial);
            s_inflight=-1;s_enqueue_error=false;
        }
    }
    if(s_inflight>=0 && generation!=s_inflight_generation){s_inflight=-1;s_retry_ms=now;}
    bool connected=mqtt_is_connected();
    if(connected && generation!=s_seen_generation){
        s_seen_generation=generation;
        s_dirty=panel_alarm_resync(&s_store,now)||s_dirty;
    }
    panel_alarm_sample_t sample={0};
    if(panel_bus_alarm_sample(&sample)){
        if(sample.paused){s_was_paused=true;s_resume_ms=now+5000U;}
        if(s_was_paused && !sample.paused &&
           ((int32_t)(now-s_resume_ms)>=0 || (sample.fresh==15 && sample.reachable)))s_was_paused=false;
        if(!s_was_paused)s_dirty=panel_alarm_observe(&s_store,&sample,now-s_started_ms>=10000U)||s_dirty;
    }
    /* Respect the existing flash/TLS quiet window without clearing alarms. */
    if(ota_service_is_busy())return;
    if((int32_t)(now-s_health_ms)>=0){
        ESP_LOGI(TAG,"ALARM_PIPE pending=%u mqtt=%u inflight=%d nvs_error=%u lost_bursts=%lu fresh=%X baseline=%04X/%04X/%04X/%04X",
            s_store.count,connected,s_inflight,s_write_error,(unsigned long)s_store.loss_count,sample.fresh,
            s_store.words[0],s_store.words[1],s_store.words[2],s_store.words[3]);
        s_health_ms=now+30000U;
    }
    if((int32_t)(now-s_retry_ms)<0)return;
    s_retry_ms=now; /* Keep the deadline near uptime across long runs/wrap. */
    if(!checkpoint())return;
    if(s_inflight>=0){
        if(now-s_sent_ms>10000U && (int32_t)(now-s_log_ms)>=0){
            ESP_LOGW(TAG,"Awaiting PUBACK id=%d; journal retained, SDK retries QoS1",s_inflight);s_log_ms=now+30000U;
        }
        return;
    }
    const panel_alarm_event_t *event=panel_alarm_front(&s_store);
    if(!event || !connected)return;
    time_t wall=time(NULL);
    uint32_t epoch=wall>=1577836800LL && (uint64_t)wall<=UINT32_MAX?(uint32_t)wall:0;
    if(!event->epoch && epoch<1577836800U){
        s_clock_waiting=true;
        if((int32_t)(now-s_log_ms)>=0){ESP_LOGW(TAG,"Waiting for valid UTC; pending=%u retained in NVS",s_store.count);s_log_ms=now+30000U;}
        return;
    }
    s_clock_waiting=false;
    if(panel_alarm_assign_time(&s_store,epoch,now,s_boot_serial)){s_dirty=true;if(!checkpoint())return;}
    event=panel_alarm_front(&s_store);
    if(panel_alarm_json(event,s_json,sizeof(s_json))<0){ESP_LOGE(TAG,"Journal event cannot encode");return;}
    generation=atomic_load(&s_generation);
    int id=full_alarm_mqtt_enqueue(s_json);
    if(id<0){s_enqueue_error=id!=-1;s_retry_ms=now+1000U;return;}
    s_enqueue_error=false;s_inflight=id;s_inflight_generation=generation;s_inflight_serial=event->serial;s_sent_ms=now;
    ESP_LOGI(TAG,"QUEUED event=%lu code=%u state=%u id=%d pending=%u estimated_time=%u; awaiting PUBACK",
        (unsigned long)event->serial,event->code,event->flags&1U,id,s_store.count,(event->flags>>7)&1U);
}
