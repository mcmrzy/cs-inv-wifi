#include <time.h>
static time_t mock_alarm_time(time_t *);
#undef time
#define time mock_alarm_time
#define PANEL_ALARM_HOST 1
#include "../full/full_alarm_service.c"
#include "panel_modbus.h"
#include <assert.h>
#include <stdio.h>

char g_device_sn[33]="MB00001122334455";
static uint32_t ms;
static int64_t utc;
static bool connected,busy,disk_exists,fail_commit,immediate_ack,fail_enqueue;
static panel_alarm_sample_t observed;
static panel_alarm_store_t disk,pending_disk;
static uint8_t legacy_disk[offsetof(panel_alarm_store_t,legacy_end)+sizeof(uint16_t)];
static bool legacy_exists;
static unsigned published,committed;
static char last_json[160];
static time_t mock_alarm_time(time_t *p){if(p)*p=(time_t)utc;return (time_t)utc;}
void mock_alarm_log(const char *t,const char *f,...){(void)t;(void)f;}
int64_t esp_timer_get_time(void){return (int64_t)ms*1000;}
const char *esp_err_to_name(int e){(void)e;return "mock";}
QueueHandle_t xQueueCreateStatic(unsigned n,size_t size,uint8_t *data,StaticQueue_t *q)
{assert(n==16 && size==8);(void)data;memset(q,0,sizeof(*q));q->item=size;return q;}
int xQueueSend(QueueHandle_t q,const void *p,unsigned t)
{(void)t;if(q->count==16)return 0;memcpy(q->bytes+((q->head+q->count)%16)*q->item,p,q->item);q->count++;return pdTRUE;}
int xQueueReceive(QueueHandle_t q,void *p,unsigned t)
{(void)t;if(!q->count)return 0;memcpy(p,q->bytes+q->head*q->item,q->item);q->head=(q->head+1)%16;q->count--;return pdTRUE;}
esp_err_t nvs_open(const char *n,int mode,nvs_handle_t *h){(void)n;(void)mode;*h=1;return ESP_OK;}
esp_err_t nvs_get_blob(nvs_handle_t h,const char *k,void *data,size_t *size)
{
    (void)h;
    if(k[0]=='q'){
        if(!legacy_exists)return ESP_ERR_NVS_NOT_FOUND;
        assert(*size>=sizeof(legacy_disk));*size=sizeof(legacy_disk);
        memcpy(data,legacy_disk,*size);return ESP_OK;
    }
    assert(k[0]=='r');
    if(!disk_exists)return ESP_ERR_NVS_NOT_FOUND;
    assert(*size==sizeof(disk));memcpy(data,&disk,sizeof(disk));return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h,const char *k,const void *data,size_t size)
{(void)h;assert(k[0]=='r' && size==sizeof(disk));memcpy(&pending_disk,data,size);return ESP_OK;}
esp_err_t nvs_commit(nvs_handle_t h)
{(void)h;if(fail_commit)return ESP_FAIL;disk=pending_disk;disk_exists=true;committed++;return ESP_OK;}
void nvs_close(nvs_handle_t h){(void)h;}
bool mqtt_is_connected(void){return connected;}
bool ota_service_is_busy(void){return busy;}
bool panel_bus_alarm_sample(panel_alarm_sample_t *out){*out=observed;out->now_ms=ms;return true;}
int full_alarm_mqtt_enqueue(const char *json)
{if(fail_enqueue)return -4;published++;snprintf(last_json,sizeof(last_json),"%s",json);int id=98+(int)published;if(immediate_ack)full_alarm_puback(id);return id;}
static void reset(bool keep_disk)
{
    ms=100;utc=0;connected=busy=fail_commit=immediate_ack=fail_enqueue=false;published=committed=0;
    if(!keep_disk){disk_exists=legacy_exists=false;memset(&disk,0,sizeof(disk));}
    memset(&s_store,0,sizeof(s_store));s_ready=s_dirty=s_write_error=s_enqueue_error=s_clock_waiting=false;
    s_inflight=-1;s_inflight_serial=s_inflight_generation=s_sent_ms=s_boot_serial=s_started_ms=0;
    s_retry_ms=s_seen_generation=s_log_ms=0;atomic_store(&s_generation,0);
    s_resume_ms=0;s_was_paused=false;
    s_health_ms=0;
    observed=(panel_alarm_sample_t){.fresh=15,.reachable=true};
}
int main(void)
{
    reset(false);assert(full_alarm_init());full_alarm_tick();assert(!published);
    observed.words[0]=1;connected=true;full_alarm_transport_reset();full_alarm_tick();
    assert(s_store.count==1 && disk.count==1 && !published && s_clock_waiting);
    utc=-1;full_alarm_tick();assert(!published && !disk.events[disk.head].epoch);utc=0;
    observed.words[0]=0;full_alarm_tick();assert(s_store.count==2 && disk.count==2);
    utc=1800000000;full_alarm_tick();assert(published==1 && s_inflight==99 && disk.events[disk.head].epoch);
    char active[160];snprintf(active,sizeof(active),"%s",last_json);
    full_alarm_puback(500);full_alarm_tick();assert(s_store.count==2 && s_inflight==99);
    ms+=11000;full_alarm_tick();assert(published==1 && s_store.count==2 && full_alarm_needs_attention());
    full_alarm_puback(99);full_alarm_tick();assert(s_store.count==1 && disk.count==1 && s_inflight==100);
    full_alarm_puback(100);full_alarm_tick();assert(!s_store.count && !disk.count);
    immediate_ack=true;observed.words[2]=4;full_alarm_tick();assert(s_store.count==1);
    full_alarm_tick();assert(!s_store.count); /* ACK may precede enqueue return. */
    reset(false);assert(full_alarm_init());observed.is_3ku=true;observed.words[0]=1;
    full_alarm_tick();assert(disk.count==1 && !published);
    reset(true);assert(full_alarm_init());observed.is_3ku=true;
    utc=1800000000;connected=true;observed.fresh=0;full_alarm_tick();
    assert(published==1 && strstr(last_json,"\"code\":22016") && strstr(last_json,"\"state\":1"));
    full_alarm_puback(99);observed.fresh=15;full_alarm_tick();
    assert(published==2 && strstr(last_json,"\"code\":22016") && strstr(last_json,"\"state\":0"));
    reset(false);
    panel_alarm_init(&s_store);strcpy(s_store.identity,g_device_sn);
    observed.words[0]=1;panel_alarm_observe(&s_store,&observed,false);
    s_store.magic=0x4d424101U;
    const size_t prefix=offsetof(panel_alarm_store_t,legacy_end);
    uint16_t crc=panel_mb_crc((const uint8_t *)&s_store,prefix);
    memcpy(legacy_disk,&s_store,prefix);memcpy(legacy_disk+prefix,&crc,sizeof(crc));
    legacy_exists=true;
    assert(full_alarm_init() && disk_exists && disk.count==1 && legacy_exists);
    assert(disk.events[disk.head].code==0x4600 && panel_alarm_validate(&disk));
    reset(true);assert(full_alarm_init() && !committed && s_store.count==1);
    reset(false);assert(full_alarm_init());observed.words[0]=1;connected=true;utc=1800000000;
    full_alarm_transport_reset();full_alarm_tick();snprintf(active,sizeof(active),"%s",last_json);
    full_alarm_expired(500);full_alarm_tick();assert(s_inflight==99 && published==1);
    full_alarm_expired(99);full_alarm_tick();assert(s_inflight==100 && s_store.count==1 && !strcmp(last_json,active));
    for(unsigned i=0;i<18;i++)full_alarm_puback(900+(int)i);
    full_alarm_tick();assert(s_store.count==1 && published==3 && !strcmp(last_json,active));
    /* Deliberate bus pause does not synthesize a failure while reacquiring. */
    reset(false);assert(full_alarm_init());full_alarm_tick();observed.paused=true;ms+=20000;
    observed.fresh=0;observed.reachable=false;full_alarm_tick();assert(!s_store.count);
    observed.paused=false;ms+=100;full_alarm_tick();assert(!s_store.count);
    observed.fresh=15;observed.reachable=true;full_alarm_tick();assert(!s_store.count);
    reset(false);assert(full_alarm_init());connected=true;utc=1800000000;observed.words[0]=1;fail_enqueue=true;
    full_alarm_transport_reset();full_alarm_tick();assert(!published && disk.count==1 && s_enqueue_error && full_alarm_needs_attention());
    fail_enqueue=false;ms+=1000;full_alarm_tick();assert(published==1 && !s_enqueue_error && s_store.count==1);
    reset(false);assert(full_alarm_init());observed.words[0]=1;connected=true;utc=1800000000;
    full_alarm_transport_reset();full_alarm_tick();assert(s_store.count==1 && s_inflight==99);
    snprintf(active,sizeof(active),"%s",last_json);
    uint32_t previous_generation=s_inflight_generation;
    connected=false;full_alarm_transport_reset();full_alarm_tick();assert(s_store.count==1 && s_inflight<0);
    alarm_ack_t stale={99,previous_generation};xQueueSend(s_acks,&stale,0);
    connected=true;full_alarm_transport_reset();full_alarm_tick();
    assert(s_store.count==1 && s_inflight==100 && !strcmp(last_json,active));
    /* Power loss before broker ACK: durable event returns with same envelope. */
    reset(true);assert(full_alarm_init());utc=1800000005;connected=true;observed.fresh=0;observed.reachable=false;
    full_alarm_transport_reset();full_alarm_tick();assert(published==1 && !strcmp(last_json,active));
    reset(false);assert(full_alarm_init());connected=true;utc=1800000000;observed.words[1]=0x8000;fail_commit=true;
    full_alarm_tick();assert(!published && s_store.count==1 && s_write_error && full_alarm_needs_attention());
    fail_commit=false;ms+=1000;full_alarm_tick();assert(published==1 && disk.count==1 && !s_write_error);
    busy=true;full_alarm_puback(99);full_alarm_tick();assert(s_dirty && disk.count==1);
    busy=false;full_alarm_tick();assert(!disk.count);
    /* A healthy loop must still publish after 32-bit uptime wraps. */
    ms=UINT32_MAX-1000U;s_retry_ms=ms;s_health_ms=ms;s_log_ms=ms;s_resume_ms=ms;
    full_alarm_tick();ms+=2000U;observed.words[1]=1;full_alarm_tick();assert(published==2 && s_inflight==100);
    snprintf(disk.identity,sizeof(disk.identity),"OTHER_DEVICE");panel_alarm_seal(&disk);
    panel_alarm_store_t foreign=disk;reset(true);assert(!full_alarm_init() && !memcmp(&disk,&foreign,sizeof(disk)));
    disk.magic^=1;panel_alarm_store_t corrupt=disk;
    reset(true);assert(!full_alarm_init() && !memcmp(&disk,&corrupt,sizeof(disk)) && full_alarm_needs_attention());
    puts("PASS alarm service: journal before publish, first alarm/recovery without telemetry, invalid UTC, matching/early/stale ACKs, expiration/ACK overflow replay, reconnect/power loss, enqueue/NVS failure, OTA/search quiet, uptime wrap and corrupt/foreign journals");return 0;
}
