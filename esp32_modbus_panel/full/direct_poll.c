/* Full direct compatibility layer: inverter UART0 belongs only to panel_bus.
 * UART1 retains the original BMS polling, identity and raw BMS IAP transport.
 * There is no private DSP startup/configuration replay in this product. */
#include "arm_poll.h"
#include "direct_protocol.h"
#include "bms_bbj_frame.h"
#include "direct_link.h"
#include "full_modbus_map.h"
#include "full_write_result.h"
#include "panel_bms_protocol.h"
#include "panel_preferences.h"
#include "panel_bus.h"
#include "panel_modbus.h"
#include "cs_protocol.h"
#include "config.h"
#include "mqtt_handler.h"
#include <limits.h>
#include <string.h>
#include <stdio.h>
#include <stdatomic.h>
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#if PANEL_BMS_DIAGNOSTIC
#include "freertos/queue.h"
static QueueHandle_t s_bms_uart_events;
#endif
#include "nvs.h"

#define BMS_PORT UART_NUM_1
#define BMS_BAUD 9600
#define BMS_POLL_INTERVAL_MS 500U
#define LINK_STALE_MS 120000U

RunParamDef g_run_param;
OnlyReadDataDef g_only_read;
ReadWiteDataDef g_read_write;
uint16_t g_ctrl_param[CTRL_PARAM_COUNT];
arm_link_stat_t g_arm_stat;
static const char *TAG="modbus_direct";
static SemaphoreHandle_t s_lock;
static atomic_bool s_quiet,s_bms_started,s_bridge_started,s_iap_busy;
static atomic_uint s_bms_tick,s_bridge_tick;
/* Low bit selects the protocol. The full value identifies one selection. */
static atomic_uint s_bms_mode;
static atomic_uint s_bms_failed_mode=UINT_MAX;
static atomic_uint s_bms_detection;
static TaskHandle_t s_bms_owner;
static TaskHandle_t s_bridge_owner;
typedef struct {
    uint16_t problem,raw;
    uint32_t age,context;
    int state;
    bool valid;
} data_diagnostic_t;
/* Plain data only on the 3KB bridge stack; main_loop owns formatting. */
static data_diagnostic_t s_data_diagnostic;
/* One shared cached bus snapshot; all access is serialized by s_lock. */
static panel_snapshot_t s_mb;
static uint32_t s_context_generation;
uint32_t full_link_context_generation(void){return panel_bus_context_generation();}
static direct_bms_measurement_t s_bms;
static uint32_t s_bms_last_ms;
static direct_bms_summary_t s_bms_summary;
static direct_bms_identity_t s_bms_identity;
static bool s_bms_identity_pending;
static uint8_t s_bms_identity_next;
static uint32_t s_bms_identity_last_ms,s_bms_identity_generation;

panel_bms_protocol_t panel_bms_protocol_get(void)
{
    return (panel_bms_protocol_t)(atomic_load(&s_bms_mode)&1U);
}

bool panel_bms_probe_failed(void)
{
    return atomic_load(&s_bms_failed_mode)==atomic_load(&s_bms_mode);
}

panel_bms_detection_t panel_bms_detection(void)
{
    return atomic_load(&s_bms_detection);
}

bool panel_bms_detect_start(void)
{
#if PANEL_BMS_DIAGNOSTIC
    return false;
#else
    if(!atomic_load(&s_bms_started) || !arm_poll_take_param())return false;
    bool ready=!atomic_load(&s_iap_busy) && !atomic_load(&s_quiet) &&
        panel_bms_detection()!=PANEL_BMS_DETECT_RUNNING;
    if(ready)atomic_store(&s_bms_detection,PANEL_BMS_DETECT_RUNNING);
    arm_poll_give_param();
    return ready;
#endif
}

static void bms_protocol_load(void)
{
#if !PANEL_BMS_DIAGNOSTIC
    nvs_handle_t h;
    uint8_t saved;
    if(nvs_open("direct_ui",NVS_READONLY,&h)!=ESP_OK)return;
    if(nvs_get_u8(h,"bms_protocol",&saved)==ESP_OK && saved==PANEL_BMS_CSERGY)
        atomic_store(&s_bms_mode,PANEL_BMS_CSERGY);
    nvs_close(h);
#endif
}

#if !PANEL_BMS_DIAGNOSTIC
static bool bms_protocol_save_locked(panel_bms_protocol_t protocol)
{
    if(panel_bms_protocol_get()==protocol)return true;
    nvs_handle_t h;
    if(nvs_open("direct_ui",NVS_READWRITE,&h)!=ESP_OK)return false;
    bool saved=nvs_set_u8(h,"bms_protocol",(uint8_t)protocol)==ESP_OK && nvs_commit(h)==ESP_OK;
    nvs_close(h);
    if(!saved)return false;
    s_bms_last_ms=0;
    s_bms_summary.acquired=false;
    memset(&s_bms_identity,0,sizeof(s_bms_identity));
    s_bms_identity_pending=false;
    s_bms_identity_next=0;
    s_bms_identity_last_ms=0;
    s_bms_identity_generation++;
    atomic_fetch_add(&s_bms_mode,1U);
    return true;
}
#endif

bool panel_bms_protocol_set(panel_bms_protocol_t protocol)
{
    if(protocol!=PANEL_BMS_BBJ && protocol!=PANEL_BMS_CSERGY)return false;
#if PANEL_BMS_DIAGNOSTIC
    return protocol==PANEL_BMS_BBJ;
#else
    if(!arm_poll_take_param())return false;
    bool saved=!atomic_load(&s_iap_busy) && !atomic_load(&s_quiet) &&
        panel_bms_detection()!=PANEL_BMS_DETECT_RUNNING && bms_protocol_save_locked(protocol);
    if(saved)atomic_store(&s_bms_detection,PANEL_BMS_DETECT_IDLE);
    arm_poll_give_param();
    return saved;
#endif
}
static direct_dsp_status_t s_status;
static bool s_status_valid;
static uint32_t s_status_last_ms,s_measurement_last_ms,s_last_utc_ms;
static uint16_t s_config[DIRECT_DSP_CONFIG_COUNT];
static bool s_config_valid[DIRECT_DSP_CONFIG_COUNT];
static uint16_t s_local[3]={1,0,1};
static uint32_t s_revision=1,s_observation_sequence;
static arm_write_result_cb_t s_write_cb[2];
static arm_ctrl_snapshot_cb_t s_ctrl_cb;
static atomic_int s_ui_write_result=-3;
static struct {
    bool pending,completed,cloud;
    uint16_t index,value;
    uint32_t sequence;
    int result;
} s_write;

typedef struct {
    direct_iap_image_t image;
    direct_iap_io_t source;
    SemaphoreHandle_t done;
    direct_iap_result_t result;
    atomic_uint progress_bytes;
    atomic_bool progress_pending;
} iap_job_t;
static iap_job_t *s_iap_pending;

static uint32_t now_ms(void){return (uint32_t)(esp_timer_get_time()/1000);}
static int local_slot(uint16_t index){return index==14?0:index==15?1:index==41?2:-1;}
static void local_load(void)
{
    panel_preferences_t preferences;
    if(!panel_preferences_get(&preferences))return;
    s_local[0]=preferences.alarm;s_local[1]=preferences.blank;s_local[2]=preferences.buzzer;
}
static bool local_save(const uint16_t values[3])
{
    panel_preferences_t preferences={.alarm=values[0],.blank=values[1],.buzzer=values[2]};
    return panel_preferences_update(&preferences,PANEL_PREF_LOCAL);
}
static bool cached_context_current(void)
{return s_context_generation==panel_bus_context_generation();}
static bool read_config_locked(uint16_t index,bool wire,uint16_t *value)
{return cached_context_current() && full_mb_read_config(&s_mb,index,wire,value);}
static void update_config_locked(void)
{
    bool changed=false;
    s_mb.now_ms=now_ms();
    for(unsigned i=0;i<DIRECT_DSP_CONFIG_COUNT;i++){
        uint16_t native=0,wire=0;
        int local=local_slot((uint16_t)i);
        bool known=local>=0 || read_config_locked((uint16_t)i,false,&native);
        if(local>=0)native=s_local[local];
        if(known!=s_config_valid[i] || (known && native!=s_config[i]))changed=true;
        s_config_valid[i]=known;s_config[i]=known?native:0;
        if(i<CTRL_PARAM_COUNT){
            bool wire_known=local>=0 || read_config_locked((uint16_t)i,true,&wire);
            if(local>=0)wire=i==15?(uint16_t)(1U-native):native;
            g_ctrl_param[i]=wire_known?wire:0;
        }
    }
    if(changed){if(s_revision!=UINT32_MAX)s_revision++;s_observation_sequence++;}
    g_read_write.CtrlParamAlterTime=s_revision;
}
void full_ui_reload_local_preferences(void)
{
    if(!s_lock)return;
    xSemaphoreTake(s_lock,portMAX_DELAY);local_load();update_config_locked();xSemaphoreGive(s_lock);
}

static void log_bridge_diagnostics(const data_diagnostic_t *d)
{
    static uint32_t next_diagnostic;
    static int previous_data_state=-1;
    if(!d->valid)return;
    if(d->state!=previous_data_state || (int32_t)(now_ms()-next_diagnostic)>=0){
        static const char *const names[]={"READY","MISSING","STALE","INVALID","PAUSED"};
        unsigned free_stack=s_bridge_owner?(unsigned)uxTaskGetStackHighWaterMark(s_bridge_owner):0;
        ESP_LOGI(TAG,"INV_DATA %s problem_reg=%04X raw=%04X age_ms=%lu context=%lu bridge_stack_free=%u",
            names[d->state],d->problem,d->raw,(unsigned long)d->age,(unsigned long)d->context,free_stack);
        previous_data_state=d->state;next_diagnostic=now_ms()+10000;
    }
}

static void bridge_task(void *arg)
{
    (void)arg;
    for(;;){
        atomic_store(&s_bridge_tick,now_ms());
        if(xSemaphoreTake(s_lock,pdMS_TO_TICKS(100))==pdTRUE){
            if(panel_bus_snapshot(&s_mb)){
                if(s_context_generation!=s_mb.context_generation){
                    s_context_generation=s_mb.context_generation;
                    memset(&g_run_param,0,sizeof(g_run_param));
                    memset(&s_status,0,sizeof(s_status));
                    s_measurement_last_ms=0;s_status_last_ms=0;s_status_valid=false;
                    g_arm_stat.last_rx_ms=0;g_arm_stat.connected=false;
                }
                RunParamDef run;uint32_t received=0;
                if(full_mb_measurement(&s_mb,&run,&received)){
                    g_run_param=run;s_measurement_last_ms=received;
                }
                /* Reachability is a recent valid inverter status reply, not
                 * completion of every register needed by legacy telemetry. */
                if(panel_online(&s_mb)){
                    g_arm_stat.last_rx_ms=panel_stamp(&s_mb,0x42);g_arm_stat.connected=true;
                }
                s_status_valid=full_mb_status(&s_mb,&s_status,&s_status_last_ms);
                data_diagnostic_t d={.raw=0xffff,.age=UINT32_MAX,.context=s_context_generation,.valid=true};
                d.state=atomic_load(&s_quiet)?4:(int)full_mb_measurement_check(&s_mb,&d.problem);
                if(d.state==0){d.raw=0;d.age=0;}
                else if(panel_value(&s_mb,d.problem,&d.raw))d.age=s_mb.now_ms-panel_stamp(&s_mb,d.problem);
                s_data_diagnostic=d;
                g_arm_stat.poll_count=s_mb.polls;g_arm_stat.err_count=s_mb.errors;
                update_config_locked();
                if(s_write.pending){
                    const panel_result_t *receipt=panel_result_find(&s_mb,s_write.sequence);
                    bool ours=receipt!=NULL;
                    bool superseded=(int32_t)(s_mb.command_sequence-s_write.sequence)>0;
                    if(ours || (superseded && !s_mb.command_busy)){
                        s_write.pending=false;s_write.completed=true;
                        /* A later command can replace the shared outcome. Never
                         * acknowledge that later receipt as this request's ACK. */
                        s_write.result=full_write_result(receipt?receipt->state:PANEL_WRITE_UNCERTAIN,ours);
                        if(!s_write.cloud)atomic_store(&s_ui_write_result,
                            ours && receipt->state==PANEL_WRITE_PROCESSED?2:s_write.result);
                        if(s_write.result && s_write.result!=FULL_WRITE_PROTOCOL_ACCEPTED)g_arm_stat.ctrl_err++;
                    }
                }
            }
            xSemaphoreGive(s_lock);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/* BMS IAP callbacks are called only by the BMS UART owner. The submitting OTA
 * task owns the job storage until done is signalled; progress is relayed there. */
static void iap_heartbeat(void){atomic_store(&s_bms_tick,now_ms());}
static bool iap_read(void *context,size_t offset,uint8_t *bytes,size_t count)
{
    iap_job_t *job=context;iap_heartbeat();
    return job->source.read_image(job->source.context,offset,bytes,count);
}
static bool iap_send(void *context,const uint8_t *bytes,size_t count)
{
    (void)context;
    if(!count)return true;
    uint32_t started=now_ms();
    unsigned timeout=(unsigned)(((uint64_t)count*10000U+BMS_BAUD-1U)/BMS_BAUD)+500U;
    bool written=uart_write_bytes(BMS_PORT,bytes,count)==(int)count;
    while(written){
        iap_heartbeat();unsigned elapsed=now_ms()-started;
        if(elapsed>=timeout)break;
        unsigned slice=timeout-elapsed;if(slice>25)slice=25;
        esp_err_t err=uart_wait_tx_done(BMS_PORT,pdMS_TO_TICKS(slice));
        if(err==ESP_OK)return true;
        if(err!=ESP_ERR_TIMEOUT)break;
    }
    return false;
}
static size_t iap_receive(void *context,uint8_t *bytes,size_t count,unsigned timeout)
{
    (void)context;uint32_t started=now_ms();
    do{
        iap_heartbeat();unsigned elapsed=now_ms()-started;
        unsigned slice=elapsed>=timeout?0:timeout-elapsed;if(slice>25)slice=25;
        int n=uart_read_bytes(BMS_PORT,bytes,count,pdMS_TO_TICKS(slice));
        if(n>0)return (size_t)n;
    }while(now_ms()-started<timeout);
    return 0;
}
static void iap_delay(void *context,unsigned ms)
{
    (void)context;
    while(ms){unsigned slice=ms>25?25:ms;iap_heartbeat();vTaskDelay(pdMS_TO_TICKS(slice));ms-=slice;}
    iap_heartbeat();
}
static void iap_flush(void *context){(void)context;iap_heartbeat();uart_flush_input(BMS_PORT);}
static uint32_t iap_now(void *context){(void)context;return now_ms();}
static bool iap_cancelled(void *context)
{iap_job_t *job=context;return job->source.cancelled && job->source.cancelled(job->source.context);}
static void iap_progress(void *context,size_t done,size_t total)
{
    (void)total;iap_job_t *job=context;iap_heartbeat();
    atomic_store(&job->progress_bytes,(unsigned)done);atomic_store(&job->progress_pending,true);
}
static bool iap_owner_step(void)
{
    iap_job_t *job=NULL;
    if(xSemaphoreTake(s_lock,pdMS_TO_TICKS(20))==pdTRUE){job=s_iap_pending;s_iap_pending=NULL;xSemaphoreGive(s_lock);}
    if(!job)return false;
    direct_iap_io_t io={.read_image=iap_read,.send=iap_send,.receive=iap_receive,.delay=iap_delay,
        .cancelled=iap_cancelled,.flush=iap_flush,.progress=iap_progress,.context=job,.now_ms=iap_now};
    bool drained=uart_wait_tx_done(BMS_PORT,pdMS_TO_TICKS(1000))==ESP_OK;
    iap_flush(job);
    job->result=drained?direct_iap_run_legacy(&job->image,&io):DIRECT_IAP_TX_FAILED;
    iap_flush(job);
    xSemaphoreTake(s_lock,portMAX_DELAY);
    s_bms_last_ms=0; /* A fresh application response is needed after restart. */
    atomic_store(&s_bms_failed_mode,UINT_MAX);
    xSemaphoreGive(s_lock);
    iap_heartbeat();
    xSemaphoreGive(job->done);
    return true;
}

direct_iap_result_t direct_link_run_iap(const direct_iap_image_t *image,const direct_iap_io_t *source)
{
    if(!image || !source || !source->read_image || image->target!=DIRECT_IAP_BMS ||
       !s_lock || xTaskGetCurrentTaskHandle()==s_bms_owner)return DIRECT_IAP_INCOMPATIBLE;
    if(!atomic_load(&s_bms_started))return DIRECT_IAP_NOT_READY;
    bool expected=false;
    if(!atomic_compare_exchange_strong(&s_iap_busy,&expected,true))return DIRECT_IAP_BUSY;
    iap_job_t job={.image=*image,.source=*source};
    atomic_init(&job.progress_bytes,0);atomic_init(&job.progress_pending,false);
    job.done=xSemaphoreCreateBinary();
    if(!job.done){atomic_store(&s_iap_busy,false);return DIRECT_IAP_NO_MEMORY;}
    xSemaphoreTake(s_lock,portMAX_DELAY);
    if(panel_bms_protocol_get()==PANEL_BMS_CSERGY){
        xSemaphoreGive(s_lock);vSemaphoreDelete(job.done);atomic_store(&s_iap_busy,false);
        return DIRECT_IAP_INCOMPATIBLE;
    }
    if(s_write.pending || panel_bms_detection()==PANEL_BMS_DETECT_RUNNING){
        xSemaphoreGive(s_lock);vSemaphoreDelete(job.done);atomic_store(&s_iap_busy,false);return DIRECT_IAP_BUSY;
    }
    bool was_quiet=atomic_exchange(&s_quiet,true);
    panel_bus_set_quiet(true);
    s_iap_pending=&job;
    xSemaphoreGive(s_lock);
    for(;;){
        bool done=xSemaphoreTake(job.done,pdMS_TO_TICKS(25))==pdTRUE;
        if(atomic_exchange(&job.progress_pending,false) && source->progress)
            source->progress(source->context,atomic_load(&job.progress_bytes),image->size);
        if(done)break;
    }
    direct_iap_result_t result=job.result;
    vSemaphoreDelete(job.done);
    atomic_store(&s_quiet,was_quiet);panel_bus_set_quiet(was_quiet);atomic_store(&s_iap_busy,false);
    return result;
}
direct_iap_result_t direct_link_dsp_control(direct_dsp_control_t operation)
{(void)operation;return DIRECT_IAP_INCOMPATIBLE;}
bool direct_dsp_boot_active(void){return false;}
int direct_dsp_params_install(const uint8_t *bundle,size_t length,const char *sn)
{(void)bundle;(void)length;(void)sn;return -2;}
bool direct_dsp_params_export(direct_dsp_params_t *params,bool *durable)
{(void)params;if(durable)*durable=false;return false;}

static bool bms_uart_setup(void)
{
    int tx=CONFIG_DIRECT_BMS_TX_PIN,rx=CONFIG_DIRECT_BMS_RX_PIN,de=CONFIG_DIRECT_BMS_RS485_DE_PIN;
    if(tx<0 || rx<0)return false;
    const uart_config_t config={.baud_rate=BMS_BAUD,.data_bits=UART_DATA_8_BITS,.parity=UART_PARITY_DISABLE,
        .stop_bits=UART_STOP_BITS_1,.flow_ctrl=UART_HW_FLOWCTRL_DISABLE,.source_clk=UART_SCLK_DEFAULT};
    /* Match the original direct UART1 allocation: 1KB RX, whole-frame 8KB TX.
     * The TX ring prevents scheduler gaps within BMS Boot's 5ms delimiter. */
    esp_err_t err=uart_driver_install(BMS_PORT,1024,8192,
#if PANEL_BMS_DIAGNOSTIC
        16,&s_bms_uart_events,
#else
        0,NULL,
#endif
        0);
    if(err==ESP_OK)err=uart_param_config(BMS_PORT,&config);
    if(err==ESP_OK)err=uart_set_pin(BMS_PORT,tx,rx,de>=0?de:UART_PIN_NO_CHANGE,UART_PIN_NO_CHANGE);
    if(err==ESP_OK && de>=0)err=uart_set_mode(BMS_PORT,UART_MODE_RS485_HALF_DUPLEX);
    if(err!=ESP_OK){
        ESP_LOGE(TAG,"BMS UART1 setup failed: %s",esp_err_to_name(err));
        uart_driver_delete(BMS_PORT);return false;
    }
    return true;
}

static void bms_identity_step(void)
{
    uint8_t type;uint32_t generation;
    if(xSemaphoreTake(s_lock,pdMS_TO_TICKS(20))!=pdTRUE)return;
    if(!s_bms_identity_pending){xSemaphoreGive(s_lock);return;}
    type=s_bms_identity_next;generation=s_bms_identity_generation;
    uint8_t query[8],rx[128];size_t have=0;
    direct_bms_identity_make_query(type,query);
    uart_flush_input(BMS_PORT);uart_write_bytes(BMS_PORT,query,sizeof(query));
    xSemaphoreGive(s_lock);
    const uint32_t start=now_ms();uint32_t last=start;
    while(have<sizeof(rx)&&now_ms()-start<200U&&(!have||now_ms()-last<30U)){
        int n=uart_read_bytes(BMS_PORT,rx+have,sizeof(rx)-have,pdMS_TO_TICKS(25));
        if(n>0){have+=(size_t)n;last=now_ms();}
    }
    if(xSemaphoreTake(s_lock,pdMS_TO_TICKS(100))!=pdTRUE)return;
    if(s_bms_identity_pending&&generation==s_bms_identity_generation&&type==s_bms_identity_next){
        /* Skip echoed request/log noise; only a matching CRC-valid 32-byte
         * reply changes the identity snapshot. */
        for(size_t i=0;i+32<=have;i++)if(direct_bms_identity_parse_reply(rx+i,32,type,&s_bms_identity)){
            s_bms_identity_last_ms=now_ms();break;
        }
        s_bms_identity_next++;
        if(s_bms_identity_next==8){
            s_bms_identity_pending=false;
            #if !PANEL_BMS_DIAGNOSTIC
            ESP_LOGI(TAG,"BMS optional identity scan completed, valid_mask=0x%02X; not upgrade authorization",s_bms_identity.valid_mask);
            #endif
        }
    }
    xSemaphoreGive(s_lock);
}

#if PANEL_BMS_DIAGNOSTIC
#include "bms_diagnostics_runtime.inc"
#endif

#if !PANEL_BMS_DIAGNOSTIC
#define CSERGY_BMS_ADDRESS 0x10U

static bool csergy_read(unsigned mode,uint8_t function,uint16_t start,uint16_t count,panel_mb_reply_t *reply)
{
    uint8_t request[8], bytes[32];
    panel_mb_stream_t stream={0};
    /* Allow at least one full tick of bus silence before the next RTU frame. */
    vTaskDelay(pdMS_TO_TICKS(5)+1);
    if(mode!=atomic_load(&s_bms_mode) || atomic_load(&s_quiet))return false;
    if(function==3)panel_mb_read(CSERGY_BMS_ADDRESS,start,count,request);
    else panel_mb_read_input(CSERGY_BMS_ADDRESS,start,count,request);
    uart_flush_input(BMS_PORT);
    if(uart_write_bytes(BMS_PORT,request,sizeof(request))!=(int)sizeof(request))return false;
    const uint32_t sent=now_ms();
    atomic_store(&s_bms_tick,sent);
    while(now_ms()-sent<500U && mode==atomic_load(&s_bms_mode) && !atomic_load(&s_quiet)){
        int n=uart_read_bytes(BMS_PORT,bytes,sizeof(bytes),pdMS_TO_TICKS(20));
        if(n<0)return false;
        for(int i=0;i<n;i++)
            if(panel_mb_feed(&stream,bytes[i],CSERGY_BMS_ADDRESS,function,start,count,reply))
                return !reply->exception;
    }
    return false;
}

static bool bms_csergy_poll(unsigned mode,direct_bms_measurement_t *sample)
{
    panel_mb_reply_t reply;
    /* Use different reply sizes for each range. FC04 does not echo start. */
    if(!csergy_read(mode,4,0x1000,12,&reply) || reply.values[0]>1000 || reply.values[1]>1000 ||
       !reply.values[2] || reply.values[2]==0x7fffU || reply.values[2]==0x8000U ||
       reply.values[2]==0xffffU || reply.values[3]==0x7fffU || reply.values[3]==0x8000U)return false;
    *sample=(direct_bms_measurement_t){.address=CSERGY_BMS_ADDRESS,.percent_divisor=10,
        .soc_percent=reply.values[0],.soh_percent=reply.values[1],
        .voltage_10mv=(uint32_t)reply.values[2]*10U,.current_10ma=(int32_t)(int16_t)reply.values[3]*10};
    if(csergy_read(mode,4,0x1020,4,&reply)){
        sample->protection=(reply.values[0]&0xffU) || (reply.values[1]&0x3eU) || reply.values[2];
        sample->warning=reply.values[3];
    }
    if(csergy_read(mode,4,0x1100,32,&reply))
        for(unsigned i=0;i<32;i++)if(reply.values[i] && reply.values[i]<=8191U)sample->cell_mv[i]=reply.values[i];
    /* Read limits only. A missing/disabled/invalid block never supplies a draft. */
    if(csergy_read(mode,3,0x1200,5,&reply) && reply.values[0]==1 &&
       reply.values[1]>0 && reply.values[1]<0x7fffU && reply.values[4]>0 &&
       reply.values[3]>reply.values[4] && reply.values[3]<0x7fffU){
        sample->charge_max_deciamp=reply.values[1];
        sample->charge_max_decivolt=reply.values[3];
        sample->discharge_min_decivolt=reply.values[4];
    }
    return true;
}
#endif

static uint32_t bms_query_interval(uint32_t failed)
{return failed>=10U?5000U:BMS_POLL_INTERVAL_MS;}

static void bms_identity_cancel_if_absent(uint32_t failed)
{
    if(failed<10U || xSemaphoreTake(s_lock,pdMS_TO_TICKS(20))!=pdTRUE)return;
    if(s_bms_identity_pending){s_bms_identity_pending=false;s_bms_identity_generation++;}
    xSemaphoreGive(s_lock);
}

static void bms_task(void *arg)
{
    (void)arg;
    uint8_t request[8], frame[256];
    uint32_t failed=0,next_query=0;
    #if !PANEL_BMS_DIAGNOSTIC
    uint32_t last_warning=0;
    unsigned last_mode=atomic_load(&s_bms_mode);
    unsigned detect_trial=0;
    #endif
    for (;;) {
        atomic_store(&s_bms_tick,now_ms());
        #if PANEL_BMS_DIAGNOSTIC
        atomic_store(&s_bms_diag_stack_free,uxTaskGetStackHighWaterMark(NULL));
        #endif
        if(iap_owner_step()){
            next_query=now_ms();
            continue;
        }
        if (atomic_load(&s_quiet)) {
            vTaskDelay(pdMS_TO_TICKS(100));
            next_query=now_ms();
            continue;
        }
        #if !PANEL_BMS_DIAGNOSTIC
        unsigned mode=atomic_load(&s_bms_mode);
        bool detecting=panel_bms_detection()==PANEL_BMS_DETECT_RUNNING;
        panel_bms_protocol_t protocol=(panel_bms_protocol_t)((mode&1U)^(detecting && detect_trial>=2));
        if(mode!=last_mode){last_mode=mode;failed=0;next_query=now_ms();}
        if(detecting)next_query=now_ms();
        #endif
        /* An absent optional battery must not monopolize UART1 queries or
         * warnings. Wake frequently so IAP and software-health remain live. */
        if((int32_t)(now_ms()-next_query)<0){vTaskDelay(pdMS_TO_TICKS(50));continue;}
        uint32_t query_started=now_ms();
        direct_bms_measurement_t sample;
        uint8_t summary[DIRECT_BMS_SUMMARY_SIZE];
        bool have_summary=false,valid;
        size_t have=0;
#if !PANEL_BMS_DIAGNOSTIC
        if(protocol==PANEL_BMS_CSERGY){
            valid=bms_csergy_poll(mode,&sample);
        }else
#endif
        {
        size_t request_len;
#if CONFIG_DIRECT_BMS_7C
        request_len = direct_bms_make_7c_query(request);
#else
        request_len = direct_bms_make_query(1, 0, 0x25, request);
#endif
        #if PANEL_BMS_DIAGNOSTIC
        unsigned sequence=atomic_fetch_add(&s_bms_diag_queries,1)+1;
        size_t buffered=0;uart_get_buffered_data_len(BMS_PORT,&buffered);
        int idle=gpio_get_level(CONFIG_DIRECT_BMS_RX_PIN);
        if(buffered){
            int old=uart_read_bytes(BMS_PORT,frame,sizeof(frame),0);
            if(old>0)bms_diag_remember_old(frame,(size_t)old);
        }
        bms_diag_events();
        #endif
        uart_flush_input(BMS_PORT);
        #if PANEL_BMS_DIAGNOSTIC
        int written=uart_write_bytes(BMS_PORT,request,request_len);
        esp_err_t tx_done=uart_wait_tx_done(BMS_PORT,pdMS_TO_TICKS(50));
        unsigned chunks=0;uint32_t first_read=UINT32_MAX;bool read_error=false;
        #else
        uart_write_bytes(BMS_PORT, request, request_len);
        #endif
        const uint32_t start = now_ms();
        uint32_t last_rx = start;
        #if CONFIG_DIRECT_BMS_7C
        /* An immediate local echo must not close the diagnostic capture
         * before a delayed BMS reply. Keep waiting for a complete response. */
        const uint32_t silence_limit=300U;
        #else
        const uint32_t silence_limit=30U;
        #endif
        while (have < sizeof(frame) && now_ms() - start < 300U &&
               (!have || now_ms() - last_rx < silence_limit)) {
            int n = uart_read_bytes(BMS_PORT, frame + have, sizeof(frame) - have,
                                    pdMS_TO_TICKS(25));
            if (n > 0) {
                #if PANEL_BMS_DIAGNOSTIC
                if(!have)first_read=now_ms()-query_started;
                chunks++;
                #endif
                have += (size_t)n;
                last_rx = now_ms();
#if CONFIG_DIRECT_BMS_7C && !PANEL_BMS_DIAGNOSTIC
                size_t found_offset,found_length;
                if(bms_bbj_find_response(frame,have,&found_offset,&found_length))break;
#endif
#if !CONFIG_DIRECT_BMS_7C
                if (have >= 79U) break;
#endif
            }
            #if PANEL_BMS_DIAGNOSTIC
            else if(n<0){read_error=true;break;}
            #endif
        }
#if CONFIG_DIRECT_BMS_7C
        size_t response_offset=0,response_length=0;
        bool response_found=bms_bbj_find_response(frame,have,&response_offset,&response_length);
        valid = response_found && direct_bms_parse_7c(frame+response_offset,response_length,&sample);
        have_summary = valid && direct_bms_encode_summary_7c(frame+response_offset,response_length,summary);
#else
        valid = direct_bms_parse_modbus(frame, have, 1, &sample);
#endif
        #if PANEL_BMS_DIAGNOSTIC
        bms_diag_report(sequence,request,request_len,written,tx_done,(unsigned)buffered,idle,
            frame,have,chunks,first_read,read_error,now_ms()-query_started,valid,have_summary,&sample);
        #endif
        }
        #if !PANEL_BMS_DIAGNOSTIC
        if(mode!=atomic_load(&s_bms_mode))continue;
        if(detecting && atomic_load(&s_quiet))continue;
        if(detecting && !valid){
            if(++detect_trial==4){
                detect_trial=0;
                next_query=now_ms();
                atomic_store(&s_bms_detection,PANEL_BMS_DETECT_MISSING);
            }
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        #endif
        if (valid) {
            #if !PANEL_BMS_DIAGNOSTIC
            if(failed)ESP_LOGW(TAG,"BMS communication recovered after %lu failed queries",(unsigned long)failed);
            #endif
            failed=0;
            if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
                #if !PANEL_BMS_DIAGNOSTIC
                if(mode!=atomic_load(&s_bms_mode)){xSemaphoreGive(s_lock);continue;}
                if(detecting && !bms_protocol_save_locked(protocol)){
                    detect_trial=0;
                    atomic_store(&s_bms_detection,PANEL_BMS_DETECT_SAVE_FAILED);
                    xSemaphoreGive(s_lock);
                    continue;
                }
                #endif
                s_bms = sample;
                s_bms_last_ms = now_ms();
                if (have_summary) {
                    memcpy(s_bms_summary.bytes, summary, sizeof(summary));
                    s_bms_summary.received_ms = (uint64_t)(esp_timer_get_time() / 1000);
                    s_bms_summary.acquired = true;
                }
                #if !PANEL_BMS_DIAGNOSTIC
                if(detecting){
                    detect_trial=0;
                    atomic_store(&s_bms_detection,PANEL_BMS_DETECT_MATCHED);
                }
                #endif
                xSemaphoreGive(s_lock);
            }
        } else {
            if(failed!=UINT32_MAX)failed++;
            bms_identity_cancel_if_absent(failed);
            #if !PANEL_BMS_DIAGNOSTIC
            if(failed==3)atomic_store(&s_bms_failed_mode,mode);
            if(failed==1||now_ms()-last_warning>=300000U){
                if((mode&1U)==PANEL_BMS_CSERGY)
                    ESP_LOGW(TAG,"CSERGY BMS no valid FC04 reply: failures=%lu",(unsigned long)failed);
                else{
                    char head[73];panel_mb_hex_head(frame,have,head,sizeof(head));
                    ESP_LOGW(TAG,"BMS no valid reply: UART1 TX=%d RX=%d bytes=%u failures=%lu head=%s",
                        CONFIG_DIRECT_BMS_TX_PIN,CONFIG_DIRECT_BMS_RX_PIN,(unsigned)have,(unsigned long)failed,head);
                }
                last_warning=now_ms();
            }
            #endif
        }
        if(valid && !atomic_load(&s_quiet) && panel_bms_protocol_get()==PANEL_BMS_BBJ)bms_identity_step();
        next_query=query_started+bms_query_interval(failed);
        if((int32_t)(now_ms()-next_query)>=0)next_query=now_ms()+BMS_POLL_INTERVAL_MS;
    }
}

bool onlyread_sn_to_str(const OnlyReadDataDef *o,char *out,size_t length)
{
    if(!o || !out || length<17)return false;
    memcpy(out,&o->Inverter_SN1,16);out[16]=0;return out[0]!=0;
}
bool arm_poll_sn_changed(void){return false;}
void arm_poll_init(void)
{
    s_lock=xSemaphoreCreateMutex();
    if(!s_lock){ESP_LOGE(TAG,"Snapshot mutex unavailable");return;}
    memset(&g_run_param,0,sizeof(g_run_param));
    memset(&g_only_read,0,sizeof(g_only_read));
    memset(&g_arm_stat,0,sizeof(g_arm_stat));
    local_load();
    bms_protocol_load();
    memcpy(&g_only_read.Inverter_SN1,g_device_sn,strnlen(g_device_sn,16));
    if(panel_bus_start()==ESP_OK){
        atomic_store(&s_bridge_started,xTaskCreate(bridge_task,"modbus_bridge",3072,NULL,4,&s_bridge_owner)==pdPASS);
        if(!atomic_load(&s_bridge_started))ESP_LOGE(TAG,"Modbus compatibility task unavailable");
    }else ESP_LOGE(TAG,"Modbus owner unavailable");
    #if PANEL_BMS_DIAGNOSTIC
    ESP_LOGW(TAG,"BMS_DIAG_BOOT firmware=%s UART1 TX=%d RX=%d 9600 8N1 invert=none DE=%d BBJ=7C/80/01 poll_ms=500 absent_ms=5000 capture_ms=300 gap_ms=300 state_report_ms=10000",
        FIRMWARE_VERSION,CONFIG_DIRECT_BMS_TX_PIN,CONFIG_DIRECT_BMS_RX_PIN,CONFIG_DIRECT_BMS_RS485_DE_PIN);
    #endif
    if(bms_uart_setup()){
        #if PANEL_BMS_DIAGNOSTIC
        const unsigned bms_stack=6144;
        #else
        const unsigned bms_stack=4096;
        #endif
        atomic_store(&s_bms_started,xTaskCreate(bms_task,"direct_bms",bms_stack,NULL,5,&s_bms_owner)==pdPASS);
        if(atomic_load(&s_bms_started))ESP_LOGI(TAG,"BMS UART1 TX=%d RX=%d 9600, protocol=%s",
                 CONFIG_DIRECT_BMS_TX_PIN,CONFIG_DIRECT_BMS_RX_PIN,panel_bms_protocol_get()==PANEL_BMS_CSERGY?"CSERGY":"BBJ");
        else ESP_LOGE(TAG,"BMS UART1 task allocation failed");
    }else if(CONFIG_DIRECT_BMS_TX_PIN<0 || CONFIG_DIRECT_BMS_RX_PIN<0)
        ESP_LOGI(TAG,"Independent BMS UART disabled by board configuration");
    else ESP_LOGE(TAG,"BMS UART owner unavailable");
}
void arm_poll_tick(void)
{
    #if PANEL_BMS_DIAGNOSTIC
    bms_diag_tick();
    #endif
    arm_write_result_cb_t callbacks[2]={0};
    bool completed=false;uint16_t index=0;int result=0;
    data_diagnostic_t diagnostic;
    if(!arm_poll_take_param())return;
    diagnostic=s_data_diagnostic;
    if(g_arm_stat.last_rx_ms && now_ms()-g_arm_stat.last_rx_ms>LINK_STALE_MS)g_arm_stat.connected=false;
    if(s_write.completed){
        completed=s_write.cloud;index=s_write.index;result=s_write.result;
        s_write.completed=false;
        callbacks[0]=s_write_cb[0];callbacks[1]=s_write_cb[1];
    }
    arm_poll_give_param();
    /* Formatting runs on main_loop's 6KB stack, outside the parameter lock. */
    log_bridge_diagnostics(&diagnostic);
    /* Consumers may acquire the parameter lock. Invoke only after unlocking. */
    if(completed)for(unsigned i=0;i<2;i++)if(callbacks[i])callbacks[i](CS_CMD_WRITE_CTRLPARAM,index,2,result);
}
void arm_poll_set_quiet(bool quiet)
{atomic_store(&s_quiet,quiet);panel_bus_set_quiet(quiet);}
void arm_poll_request_info(void){}
void arm_poll_request_config(void)
{
    uint16_t values[CTRL_PARAM_COUNT];uint32_t revision=0;arm_ctrl_snapshot_cb_t cb=NULL;bool complete=true;
    if(!arm_poll_take_param())return;
    for(unsigned i=0;i<CTRL_PARAM_COUNT;i++){values[i]=g_ctrl_param[i];if(!s_config_valid[i])complete=false;}
    revision=s_revision;cb=s_ctrl_cb;arm_poll_give_param();
    /* This legacy callback has no validity mask, so never publish absent fields
     * as zero. The ordinary telemetry API uses direct_config_snapshot_wire. */
    if(complete && revision && cb)cb(values,CTRL_PARAM_COUNT,revision);
}
static int enqueue_locked(uint16_t index,uint16_t value,bool cloud,bool wire)
{
    if(atomic_load(&s_quiet) || atomic_load(&s_iap_busy))return ARM_WRITE_UPGRADE_BUSY;
    if(s_write.pending || s_write.completed)return ARM_WRITE_BUSY;
    int local=local_slot(index);
    if(local>=0){
        uint16_t native=value;
        if(wire && index==15){if(value>1)return ARM_WRITE_INVALID;native=(uint16_t)(1U-value);}
        if(native>1)return ARM_WRITE_INVALID;
        panel_preferences_t change={0};
        static const unsigned fields[]={PANEL_PREF_ALARM,PANEL_PREF_BLANK,PANEL_PREF_BUZZER};
        if(local==0)change.alarm=native;else if(local==1)change.blank=native;else change.buzzer=native;
        if(!panel_preferences_update(&change,fields[local]))return ARM_WRITE_SAVE_FAILED;
        local_load();update_config_locked();
        s_write.index=index;s_write.value=value;s_write.cloud=cloud;s_write.result=0;s_write.completed=cloud;
        if(!cloud)atomic_store(&s_ui_write_result,0);
        return ARM_WRITE_ACCEPTED;
    }
    if(!atomic_load(&s_bridge_started) || !cached_context_current())return ARM_WRITE_NOT_READY;
    s_mb.now_ms=now_ms();panel_command_t command={0};
    if(!full_mb_make_write(&s_mb,index,value,wire,&command))return ARM_WRITE_UNSUPPORTED;
    uint32_t sequence=0;
    if(!panel_bus_submit_sequence(&command,&sequence))return ARM_WRITE_BUSY;
    s_write.index=index;s_write.value=value;s_write.cloud=cloud;s_write.sequence=sequence;
    s_write.pending=true;s_write.completed=false;
    if(!cloud)atomic_store(&s_ui_write_result,1);
    return ARM_WRITE_ACCEPTED;
}
int arm_poll_send_write(uint8_t cmd,uint16_t index,uint16_t length,const uint8_t *data)
{
    if(cmd!=CS_CMD_WRITE_CTRLPARAM)return ARM_WRITE_UNSUPPORTED;
    if(length!=2 || !data)return ARM_WRITE_INVALID;
    if(!arm_poll_take_param())return ARM_WRITE_NOT_READY;
    int result=enqueue_locked(index,(uint16_t)(data[0]|((uint16_t)data[1]<<8)),true,true);
    arm_poll_give_param();return result;
}
bool arm_poll_write_pending(void)
{
    if(!arm_poll_take_param())return true;
    bool pending=s_write.pending || s_write.completed;arm_poll_give_param();return pending;
}
void arm_poll_mark_utc_done(void){s_last_utc_ms=now_ms();}
uint32_t arm_poll_last_utc_ms(void){return s_last_utc_ms;}
bool arm_poll_take_param(void)
{return s_lock && xSemaphoreTake(s_lock,pdMS_TO_TICKS(100))==pdTRUE;}
void arm_poll_give_param(void){if(s_lock)xSemaphoreGive(s_lock);}
const char *full_modbus_registry_model(void)
{
    if(!s_lock || xSemaphoreTake(s_lock,0)!=pdTRUE)return NULL;
    const char *model=full_mb_registry_model(&s_mb);
    xSemaphoreGive(s_lock);
    return model;
}
void arm_poll_set_write_cb(arm_write_result_cb_t cb)
{
    if(!cb || !arm_poll_take_param())return;
    if(!s_write_cb[0])s_write_cb[0]=cb;else s_write_cb[1]=cb;
    arm_poll_give_param();
}
void arm_poll_set_ctrl_snapshot_cb(arm_ctrl_snapshot_cb_t cb)
{if(arm_poll_take_param()){s_ctrl_cb=cb;arm_poll_give_param();}}
bool direct_config_get(uint16_t index,uint16_t *value)
{
    if(!value || index>=DIRECT_DSP_CONFIG_COUNT || !arm_poll_take_param())return false;
    int local=local_slot(index);s_mb.now_ms=now_ms();
    bool known=local>=0 || read_config_locked(index,false,value);
    if(local>=0)*value=s_local[local];
    arm_poll_give_param();return known;
}
bool direct_config_get_wire(uint16_t index,uint16_t *value)
{
    if(!value || index>=DIRECT_DSP_CONFIG_COUNT || !arm_poll_take_param())return false;
    int local=local_slot(index);s_mb.now_ms=now_ms();
    bool known=local>=0 || read_config_locked(index,true,value);
    if(local>=0)*value=index==15?(uint16_t)(1U-s_local[local]):s_local[local];
    arm_poll_give_param();return known;
}
bool direct_config_snapshot_wire(uint16_t values[CTRL_PARAM_COUNT],bool valid[CTRL_PARAM_COUNT],uint32_t *revision)
{
    if(!values || !valid || !revision || !arm_poll_take_param())return false;
    s_mb.now_ms=now_ms();
    for(unsigned i=0;i<CTRL_PARAM_COUNT;i++){
        int local=local_slot((uint16_t)i);uint16_t value=0;
        valid[i]=local>=0 || read_config_locked((uint16_t)i,true,&value);
        if(local>=0)value=i==15?(uint16_t)(1U-s_local[local]):s_local[local];
        values[i]=valid[i]?value:0;
    }
    *revision=s_revision;arm_poll_give_param();return true;
}
bool direct_config_observed_snapshot_wire(direct_config_observation_t *out)
{
    if(!out || !arm_poll_take_param())return false;
    memset(out,0,sizeof(*out));s_mb.now_ms=now_ms();bool any=false;uint32_t oldest=s_mb.now_ms;
    for(unsigned i=0;i<DIRECT_DSP_CONFIG_COUNT;i++){
        /* Only actual inverter observations count, never local preferences. */
        out->valid[i]=local_slot((uint16_t)i)<0 && read_config_locked((uint16_t)i,true,&out->values[i]);
        if(out->valid[i])any=true;
    }
    const uint16_t regs[]={0x140,0x142,0x143,0x150,0x2a1,0x351};
    for(unsigned i=0;i<sizeof(regs)/sizeof(regs[0]);i++)if(panel_fresh(&s_mb,regs[i]) &&
        (s_mb.now_ms-s_mb.updated_ms[panel_slot(regs[i])])>(s_mb.now_ms-oldest))oldest=s_mb.updated_ms[panel_slot(regs[i])];
    out->received_ms=oldest;out->age_ms=s_mb.now_ms-oldest;out->sequence=s_observation_sequence;
    arm_poll_give_param();return any;
}
void direct_config_snapshot(uint16_t values[DIRECT_DSP_CONFIG_COUNT],bool valid[DIRECT_DSP_CONFIG_COUNT])
{
    if(!values || !valid)return;
    memset(values,0,sizeof(uint16_t)*DIRECT_DSP_CONFIG_COUNT);memset(valid,0,sizeof(bool)*DIRECT_DSP_CONFIG_COUNT);
    if(!arm_poll_take_param())return;
    s_mb.now_ms=now_ms();
    for(unsigned i=0;i<DIRECT_DSP_CONFIG_COUNT;i++){
        int local=local_slot((uint16_t)i);
        valid[i]=local>=0 || read_config_locked((uint16_t)i,false,&values[i]);
        if(local>=0)values[i]=s_local[local];
    }
    arm_poll_give_param();
}
bool direct_config_writable(uint16_t index)
{
    if(index>=DIRECT_DSP_CONFIG_COUNT || !arm_poll_take_param())return false;
    s_mb.now_ms=now_ms();
    bool writable=!s_write.pending && !s_write.completed && !atomic_load(&s_quiet) && !atomic_load(&s_iap_busy) &&
        (local_slot(index)>=0 || (cached_context_current() && full_mb_config_writable(&s_mb,index)));
    arm_poll_give_param();return writable;
}
int direct_config_write(uint16_t index,uint16_t value)
{
    if(!arm_poll_take_param())return ARM_WRITE_NOT_READY;
    int result=enqueue_locked(index,value,false,false);arm_poll_give_param();return result;
}
int direct_config_last_write_result(void){return atomic_load(&s_ui_write_result);}
int direct_config_reset_local(void)
{
    if(!arm_poll_take_param())return ARM_WRITE_NOT_READY;
    if(s_write.pending || s_write.completed || atomic_load(&s_iap_busy)){
        arm_poll_give_param();return ARM_WRITE_BUSY;
    }
    uint16_t defaults[3]={1,0,1};bool saved=local_save(defaults);
    if(saved){memcpy(s_local,defaults,sizeof(s_local));update_config_locked();atomic_store(&s_ui_write_result,0);}
    arm_poll_give_param();return saved?ARM_WRITE_ACCEPTED:ARM_WRITE_SAVE_FAILED;
}
bool direct_battery_config_get(direct_battery_config_t *config){(void)config;return false;}
bool direct_battery_config_profiles(direct_battery_config_t profiles[DIRECT_BATTERY_PROFILE_COUNT])
{if(profiles)memset(profiles,0,sizeof(*profiles)*DIRECT_BATTERY_PROFILE_COUNT);return false;}
int direct_battery_config_write(const direct_battery_config_t *config,bool cloud)
{(void)config;(void)cloud;return ARM_WRITE_PROTOCOL_UNSUPPORTED;}
bool direct_dsp_get_status(direct_dsp_status_t *status)
{uint32_t received;return direct_dsp_get_status_sample(status,&received);}
bool direct_dsp_get_status_sample(direct_dsp_status_t *status,uint32_t *received)
{
    if(!status || !received || !arm_poll_take_param())return false;
    bool valid=s_context_generation==panel_bus_context_generation() &&
        s_status_valid && now_ms()-s_status_last_ms<=5000U;
    if(valid){*status=s_status;*received=s_status_last_ms;}
    arm_poll_give_param();return valid;
}
bool direct_dsp_get_measurement(RunParamDef *measurement,uint32_t *received)
{
    if(!measurement || !received || !arm_poll_take_param())return false;
    bool valid=s_context_generation==panel_bus_context_generation() &&
        s_measurement_last_ms && now_ms()-s_measurement_last_ms<=FULL_MB_MEASUREMENT_FRESH_MS;
    if(valid){*measurement=g_run_param;*received=s_measurement_last_ms;}
    arm_poll_give_param();return valid;
}
bool direct_dsp_get_diagnostics(direct_dsp_diagnostics_t *out)
{
    if(!out || !arm_poll_take_param())return false;
    memset(out,0,sizeof(*out));out->rx_errors=s_mb.errors;
    out->cmd04=s_mb.polls;out->last_measurement_ms=s_measurement_last_ms;out->last_status_ms=s_status_last_ms;
    out->task_running=atomic_load(&s_bridge_started) && panel_bus_software_healthy();
    out->quiet=atomic_load(&s_quiet);out->poll_hold_reason=out->quiet?1U:0U;
    arm_poll_give_param();return true;
}
bool direct_bms_get_snapshot(direct_bms_measurement_t *measurement,uint32_t *age)
{
    if(!measurement || !age || !arm_poll_take_param())return false;
    bool valid=s_bms_last_ms!=0;
    if(valid){*measurement=s_bms;*age=now_ms()-s_bms_last_ms;}
    else {memset(measurement,0,sizeof(*measurement));*age=UINT32_MAX;}
    arm_poll_give_param();return valid;
}
bool direct_bms_get_summary(direct_bms_summary_t *summary)
{
    if(!summary || !arm_poll_take_param())return false;
    *summary=s_bms_summary;arm_poll_give_param();return summary->acquired;
}
bool direct_link_software_healthy(void)
{
    uint32_t now=now_ms();
    bool bms_disabled=CONFIG_DIRECT_BMS_TX_PIN<0 || CONFIG_DIRECT_BMS_RX_PIN<0;
    bool bms_healthy=bms_disabled || (atomic_load(&s_bms_started) && now-atomic_load(&s_bms_tick)<=2000U);
    return panel_bus_software_healthy() && atomic_load(&s_bridge_started) && bms_healthy &&
        now-atomic_load(&s_bridge_tick)<=2000U;
}
bool direct_bms_identity_request(void)
{
    if(!atomic_load(&s_bms_started) || atomic_load(&s_quiet) || !arm_poll_take_param())return false;
    bool accepted=panel_bms_protocol_get()==PANEL_BMS_BBJ && !s_bms_identity_pending &&
        s_bms_last_ms && now_ms()-s_bms_last_ms<=5000U;
    if(accepted){memset(&s_bms_identity,0,sizeof(s_bms_identity));s_bms_identity_next=0;
        s_bms_identity_last_ms=0;s_bms_identity_generation++;s_bms_identity_pending=true;}
    arm_poll_give_param();return accepted;
}
bool direct_bms_identity_snapshot(direct_bms_identity_t *identity,bool *pending,uint8_t *queried,uint32_t *age)
{
    if(!identity || !pending || !queried || !age || !arm_poll_take_param())return false;
    *identity=s_bms_identity;*pending=s_bms_identity_pending;*queried=s_bms_identity_next;
    *age=s_bms_identity_last_ms?now_ms()-s_bms_identity_last_ms:UINT32_MAX;
    arm_poll_give_param();return true;
}

