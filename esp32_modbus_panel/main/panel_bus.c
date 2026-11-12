#include "panel_bus.h"
#include "panel_control.h"
#include "panel_settings_query.h"
#include "panel_catalog.h"
#include "panel_modbus.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "sdkconfig.h"
#include <string.h>
#include <stdatomic.h>
#include <stdio.h>

static const char *TAG="panel_bus";
static SemaphoreHandle_t s_lock;
static QueueHandle_t s_commands;
static panel_snapshot_t s_live, s_shared;
static bool s_reserved;
static bool s_running;
static atomic_bool s_quiet;
static atomic_uint s_tick;
static atomic_bool s_diag_request;
static uint32_t s_sequence, s_last_end;
static uint8_t s_target;
static uint32_t s_timeout=350;
static panel_control_t s_control;
static panel_settings_query_t s_setting_query;
static uint16_t s_setting_requested[PANEL_SETTING_READ_SLOTS];
static unsigned s_setting_count,s_setting_version,s_setting_applied;
static uint32_t s_active_sequence;
static panel_command_type_t s_active_type;
static atomic_uint s_context_generation;
typedef struct {panel_command_t command;uint32_t sequence;} queued_command_t;
typedef struct {
    uint8_t address,function,exception,head[24],head_length;
    uint16_t start,count;
    uint32_t bytes,at_ms;
    const char *result;
    bool valid;
} exchange_trace_t;
static exchange_trace_t s_rx,s_failed_rx;
static uint32_t s_read_ok,s_read_failed,s_read_at;
static bool s_have_read;

static void log_link_health(void)
{
    static uint32_t next;
    static const char *previous;
    uint32_t now=(uint32_t)(esp_timer_get_time()/1000);
    s_live.now_ms=now;
    const char *state=atomic_load(&s_quiet)?"PAUSED":s_live.scanning?"SCANNING":
        panel_online(&s_live)?"ONLINE":
        s_have_read && now-s_read_at<=PANEL_FRESH_MS?"VALID_READ_NO_STATUS":
        s_failed_rx.valid && s_failed_rx.address==s_live.config.address &&
          s_failed_rx.exception && now-s_failed_rx.at_ms<=PANEL_FRESH_MS?"MODBUS_EXCEPTION":
        s_failed_rx.valid && s_failed_rx.address==s_live.config.address &&
          s_failed_rx.bytes && now-s_failed_rx.at_ms<=PANEL_FRESH_MS?"RX_UNMATCHED":"NO_RESPONSE";
    if(state==previous && (int32_t)(now-next)<0)return;
    previous=state;next=now+10000;
    uint16_t status=0xffff;uint32_t age=UINT32_MAX;
    if(panel_value(&s_live,0x42,&status))age=now-panel_stamp(&s_live,0x42);
    ESP_LOGI(TAG,"INV_LINK %s UART0 TX=1 RX=0 invert=off addr=%u baud=%lu parity=%c status=%04X age_ms=%lu read_ok=%lu read_fail=%lu",
        state,s_live.config.address,(unsigned long)s_live.config.baud,"NEO"[s_live.config.parity],
        status,(unsigned long)age,(unsigned long)s_read_ok,(unsigned long)s_read_failed);
    if(s_failed_rx.valid && !s_live.scanning){
        char head[73];panel_mb_hex_head(s_failed_rx.head,s_failed_rx.head_length,head,sizeof(head));
        ESP_LOGI(TAG,"INV_RX last_fail addr=%u fc=%02X reg=%04X count=%u result=%s rx_bytes=%lu ex=%02X age_ms=%lu head=%s",
            s_failed_rx.address,s_failed_rx.function,s_failed_rx.start,s_failed_rx.count,
            s_failed_rx.result,(unsigned long)s_failed_rx.bytes,s_failed_rx.exception,
            (unsigned long)(now-s_failed_rx.at_ms),head);
    }
}

uint32_t panel_bus_context_generation(void){return atomic_load(&s_context_generation);}

static uint32_t uptime(void *context)
{ (void)context; return (uint32_t)(esp_timer_get_time()/1000); }
static void sleep_ms(void *context,uint32_t ms)
{ (void)context; atomic_store(&s_tick,uptime(NULL));vTaskDelay(pdMS_TO_TICKS(ms)); }

void panel_bus_set_quiet(bool quiet) { atomic_store(&s_quiet,quiet); }
void panel_bus_request_ota_diagnostics(void) { atomic_store(&s_diag_request,true); }
bool panel_bus_software_healthy(void)
{ return s_running && uptime(NULL)-atomic_load(&s_tick)<2000U; }

static void publish_locked(const panel_snapshot_t *snapshot)
{
    s_shared=*snapshot;
    s_shared.command_sequence=s_sequence;
    s_shared.command_busy=s_reserved;
}

static void publish(void *context,const panel_snapshot_t *snapshot)
{
    (void)context;
    xSemaphoreTake(s_lock,portMAX_DELAY);
    publish_locked(snapshot);
    xSemaphoreGive(s_lock);
}

bool panel_bus_snapshot(panel_snapshot_t *out)
{
    if (!out || !s_lock) return false;
    xSemaphoreTake(s_lock,portMAX_DELAY);
    *out=s_shared;
    xSemaphoreGive(s_lock);
    out->now_ms=uptime(NULL);
    return true;
}
bool panel_bus_alarm_sample(panel_alarm_sample_t *out)
{
    if(!out || !s_lock)return false;
    *out=(panel_alarm_sample_t){.now_ms=uptime(NULL)};
    xSemaphoreTake(s_lock,portMAX_DELAY);
    panel_profile_t profile=s_shared.detected_profile;
    if(!panel_profile_supported(profile))profile=s_shared.config.profile_override;
    out->is_3ku=profile==PANEL_PROFILE_3KU;
    if(panel_profile_supported(profile))for(unsigned i=0;i<4;i++){
        unsigned slot=panel_slot((uint16_t)(0x46+i));
        if(s_shared.valid[slot] && out->now_ms-s_shared.updated_ms[slot]<=PANEL_FRESH_MS){
            out->words[i]=s_shared.values[slot];out->fresh|=(uint8_t)(1U<<i);
        }
    }
    for(unsigned i=0;i<PANEL_CACHE_SLOTS;i++)if(s_shared.valid[i] && out->now_ms-s_shared.updated_ms[i]<=PANEL_FRESH_MS){out->reachable=true;break;}
    out->paused=atomic_load(&s_quiet) || s_shared.scanning;
    xSemaphoreGive(s_lock);return true;
}

bool panel_bus_submit(const panel_command_t *command)
{ return panel_bus_submit_sequence(command,NULL); }

static bool submit_sequence(const panel_command_t *command,const uint32_t *context,uint32_t *sequence)
{
    if (!command || (unsigned)command->type>PANEL_CMD_SCAN_CANCEL || !s_running ||
        !s_lock || !s_commands || atomic_load(&s_quiet)) return false;
    xSemaphoreTake(s_lock,portMAX_DELAY);
    bool available=command->type==PANEL_CMD_SCAN_CANCEL ?
        s_reserved && s_shared.scanning : !s_reserved;
    if(context && *context!=s_shared.context_generation)available=false;
    queued_command_t request={.command=*command,.sequence=s_sequence+1U};
    bool accepted=available && xQueueSend(s_commands,&request,0)==pdTRUE;
    if (accepted) {
        s_reserved=true;
        s_sequence++;
        s_shared.command_sequence=s_sequence;
        s_shared.command_busy=true;
        if(sequence)*sequence=s_sequence;
    }
    xSemaphoreGive(s_lock);
    return accepted;
}

bool panel_bus_submit_sequence(const panel_command_t *command,uint32_t *sequence)
{ return submit_sequence(command,NULL,sequence); }

bool panel_bus_submit_in_context(const panel_command_t *command,uint32_t context,uint32_t *sequence)
{ return submit_sequence(command,&context,sequence); }

static void complete(void)
{
    if(s_active_type==PANEL_CMD_WRITE){
        static const char *const states[]={"IDLE","QUEUED","SENDING","ACK",
            "PROCESSED_UNCONFIRMED","CONFIRMED","UNCERTAIN","REJECTED","PROTOCOL_ACCEPTED"};
        unsigned state=(unsigned)s_live.write_state;
        ESP_LOGI(TAG,"WRITE_RESULT seq=%lu reg=%04X value=%u state=%s(%u) exception=%02X marker_seen=%u marker=%04X actual_reg=%04X actual_seen=%u actual=%u",
            (unsigned long)s_active_sequence,s_live.write_address,s_live.write_value,
            state<sizeof(states)/sizeof(states[0])?states[state]:"UNKNOWN",state,s_live.exception,
            s_control.marker_seen,s_control.marker_raw,s_control.actual_reg,s_control.actual_seen,s_control.actual_raw);
    }
    panel_result_record(&s_live,s_active_sequence,s_live.write_state,s_live.exception);
    xSemaphoreTake(s_lock,portMAX_DELAY);
    /* A cancellation may have queued while the last scan read completed. */
    s_reserved=uxQueueMessagesWaiting(s_commands)>0;
    publish_locked(&s_live);
    xSemaphoreGive(s_lock);
}

static bool config_store(panel_config_t *c,bool write)
{
    nvs_handle_t h;
    if(nvs_open("mb_panel",write ? NVS_READWRITE : NVS_READONLY,&h)!=ESP_OK) return false;
    uint8_t b[8]={1,c->address,c->parity,(uint8_t)c->profile_override,
        (uint8_t)c->baud,(uint8_t)(c->baud>>8),(uint8_t)(c->baud>>16),(uint8_t)(c->baud>>24)};
    esp_err_t err;
    if(write) {
        err=nvs_set_blob(h,"config",b,sizeof(b));
        if(err==ESP_OK)err=nvs_commit(h);
    } else {
        size_t length=sizeof(b);
        err=nvs_get_blob(h,"config",b,&length);
        if(err==ESP_OK && length==sizeof(b) && b[0]==1) {
            panel_config_t loaded={.address=b[1],.parity=b[2],.profile_override=(panel_profile_t)b[3],
                .baud=(uint32_t)b[4]|((uint32_t)b[5]<<8)|((uint32_t)b[6]<<16)|((uint32_t)b[7]<<24)};
            if(!panel_profile_supported(loaded.profile_override))loaded.profile_override=PANEL_PROFILE_UNKNOWN;
            if(panel_config_valid(&loaded))*c=loaded;else err=ESP_ERR_INVALID_ARG;
        } else err=ESP_ERR_INVALID_SIZE;
    }
    nvs_close(h);
    return err==ESP_OK;
}

static esp_err_t configure_uart(const panel_config_t *config)
{
    uart_config_t u={.baud_rate=(int)config->baud,.data_bits=UART_DATA_8_BITS,
        .parity=config->parity==1 ? UART_PARITY_EVEN : config->parity==2 ? UART_PARITY_ODD : UART_PARITY_DISABLE,
        .stop_bits=UART_STOP_BITS_1,.flow_ctrl=UART_HW_FLOWCTRL_DISABLE,.source_clk=UART_SCLK_DEFAULT};
    esp_err_t err=uart_param_config(UART_NUM_0,&u);
    if(err!=ESP_OK)return err;
    /* Clear both software inversion bits on startup and reconfiguration. */
    return uart_set_line_inverse(UART_NUM_0,0);
}

static panel_io_result_t exchange(const uint8_t *request,size_t length,uint8_t function,
                                 uint16_t start,uint16_t count,panel_mb_reply_t *reply)
{
    s_rx=(exchange_trace_t){.address=s_target,.function=function,.start=start,.count=count,.valid=true};
    /* Dedicated bus, 8N1 at 9600: 3.5 characters <4ms. Also cover 1200 8E1.
     * Flush only after a quiet interval; never read this UART in another task. */
    uint32_t gap=(38500U+s_live.config.baud-1U)/s_live.config.baud+2U;
    uint32_t elapsed=uptime(NULL)-s_last_end;
    if(elapsed<gap)sleep_ms(NULL,gap-elapsed);
    uart_flush_input(UART_NUM_0);
    if(uart_write_bytes(UART_NUM_0,request,length)!=(int)length ||
       uart_wait_tx_done(UART_NUM_0,pdMS_TO_TICKS(150))!=ESP_OK) {
        s_last_end=uptime(NULL);s_rx.at_ms=s_last_end;s_rx.result="TX_ERROR";
        s_failed_rx=s_rx;return PANEL_IO_ERROR;
    }
    panel_mb_stream_t stream={0};
    uint32_t started=uptime(NULL);
    uint8_t bytes[64];
    while(uptime(NULL)-started<s_timeout) {
        atomic_store(&s_tick,uptime(NULL));
        int n=uart_read_bytes(UART_NUM_0,bytes,sizeof(bytes),pdMS_TO_TICKS(10));
        for(int i=0;i<n;i++) {
            s_rx.bytes++;
            if(s_rx.head_length<sizeof(s_rx.head))s_rx.head[s_rx.head_length++]=bytes[i];
            if(panel_mb_feed(&stream,bytes[i],s_target,function,start,count,reply)) {
                s_last_end=uptime(NULL);
                s_rx.at_ms=s_last_end;s_rx.exception=reply->exception;
                s_rx.result=reply->exception?"EXCEPTION":"MATCHED";
                if(reply->exception)s_failed_rx=s_rx;
                return reply->exception ? PANEL_IO_EXCEPTION : PANEL_IO_OK;
            }
        }
    }
    s_last_end=uptime(NULL);
    s_rx.at_ms=s_last_end;s_rx.result=s_rx.bytes?"UNMATCHED_RX":"NO_RX";s_failed_rx=s_rx;
    return PANEL_IO_ERROR;
}

static panel_io_result_t read_regs(void *context,uint16_t start,uint16_t count,
                                   uint16_t *values,uint8_t *exception)
{
    (void)context;
    uint8_t request[8];panel_mb_reply_t reply={0};
    size_t n=panel_mb_read(s_target,start,count,request);
    if(!n)return PANEL_IO_ERROR;
    panel_io_result_t r=exchange(request,n,3,start,count,&reply);
    if(!s_live.scanning){
        if(r==PANEL_IO_OK){s_read_ok++;s_read_at=uptime(NULL);s_have_read=true;}
        else s_read_failed++;
    }
    s_live.polls++;
    if(r==PANEL_IO_OK)memcpy(values,reply.values,count*sizeof(*values));
    else { s_live.errors++; if(exception)*exception=reply.exception; }
    return r;
}

static panel_io_result_t write_reg(void *context,uint16_t reg,uint16_t value,uint8_t *exception)
{
    (void)context;
    uint8_t request[11];panel_mb_reply_t reply={0};
    size_t n=panel_mb_write(s_target,reg,value,request);
    if(!n)return PANEL_IO_ERROR;
    panel_io_result_t r=exchange(request,n,0x10,reg,1,&reply);
    if(r!=PANEL_IO_OK){s_live.errors++; if(exception)*exception=reply.exception;}
    ESP_LOGI(TAG,"WRITE addr=%u reg=%04X value=%u result=%d exception=%u",
             s_target,reg,value,r,reply.exception);
    return r;
}

static void poll_block(uint16_t start,uint16_t count)
{
    uint16_t values[32];uint8_t exception=0;
    if(count>32 || (uint32_t)start+count>PANEL_REG_COUNT)return;
    if(read_regs(NULL,start,count,values,&exception)==PANEL_IO_OK) {
        uint32_t received=uptime(NULL);
        for(unsigned i=0;i<count;i++) {
            s_live.values[panel_slot(start+i)]=values[i];
            s_live.updated_ms[panel_slot(start+i)]=received;
            s_live.valid[panel_slot(start+i)]=true;
        }
    }
    s_live.now_ms=uptime(NULL);
    if(s_live.valid[panel_slot(0x38)] && s_live.valid[panel_slot(0x39)])
        s_live.detected_profile=panel_detect_profile(s_live.values[panel_slot(0x38)],s_live.values[panel_slot(0x39)]);
    publish(NULL,&s_live);
}

static void change_config(panel_config_t config)
{
    s_live.write_state=PANEL_WRITE_REJECTED;
    s_live.exception=0xff; /* local configuration failure, not Modbus exception */
    if(!panel_config_valid(&config) ||
       (s_live.profile_locked && config.profile_override!=s_live.config.profile_override))return;
    if(config.address==s_live.config.address && config.baud==s_live.config.baud &&
       config.parity==s_live.config.parity && config.profile_override==s_live.config.profile_override){
        s_live.write_state=PANEL_WRITE_IDLE;s_live.exception=0;return;
    }
    if(configure_uart(&config)!=ESP_OK) {
        /* param_config may already have changed the baud before a later
         * driver step fails. Restore the committed context in either case. */
        if(configure_uart(&s_live.config)!=ESP_OK)ESP_LOGE(TAG,"UART rollback failed");
        ESP_LOGE(TAG,"UART reconfiguration failed");return;
    }
    if(!config_store(&config,true)) {
        (void)configure_uart(&s_live.config);
        ESP_LOGE(TAG,"Local configuration save failed"); return;
    }
    panel_device_context_reset(&s_live,&config);
    s_read_ok=0;s_read_failed=0;s_read_at=0;s_have_read=false;
    memset(&s_failed_rx,0,sizeof(s_failed_rx));
    atomic_store(&s_context_generation,s_live.context_generation);
    s_target=config.address;
    s_live.detected_profile=PANEL_PROFILE_UNKNOWN;
    s_live.write_state=PANEL_WRITE_IDLE;
    s_live.exception=0;
    s_live.scan_found=0;
    s_live.ota_valid=0;
    ESP_LOGI(TAG,"CONFIG address=%u baud=%lu parity=%u model=%s",config.address,
             (unsigned long)config.baud,config.parity,panel_profile_name(config.profile_override));
}

/* The phone may request one detail without displacing the five LCD items.
 * It stops consuming bus time automatically when the phone stops polling. */
static uint16_t s_ble_setting;
static uint32_t s_ble_setting_until;

void panel_bus_request_settings(const uint16_t *addresses,unsigned count)
{
    if(!s_lock)return;
    if(!addresses)count=0;
    if(count>PANEL_SETTING_LCD_SLOTS)count=PANEL_SETTING_LCD_SLOTS;
    xSemaphoreTake(s_lock,portMAX_DELAY);
    if(count!=s_setting_count || (count && memcmp(addresses,s_setting_requested,count*sizeof(*addresses)))){
        s_setting_count=count;
        if(count)memcpy(s_setting_requested,addresses,count*sizeof(*addresses));
        s_setting_version++;
    }
    xSemaphoreGive(s_lock);
}

void panel_bus_request_ble_setting(uint16_t address)
{
    if(!s_lock || !panel_setting_find(address))return;
    xSemaphoreTake(s_lock,portMAX_DELAY);
    if(s_ble_setting!=address){
        s_ble_setting=address;
        s_setting_version++;
    }
    s_ble_setting_until=uptime(NULL)+4000U;
    xSemaphoreGive(s_lock);
}

static bool service_settings_query(const panel_control_io_t *io)
{
    uint16_t addresses[PANEL_SETTING_READ_SLOTS];unsigned count=0;bool changed;
    xSemaphoreTake(s_lock,portMAX_DELAY);
    if(s_ble_setting && (int32_t)(uptime(NULL)-s_ble_setting_until)>=0){
        s_ble_setting=0;
        s_setting_version++;
    }
    changed=s_setting_applied!=s_setting_version;
    if(changed){
        if(s_ble_setting)addresses[count++]=s_ble_setting;
        for(unsigned i=0;i<s_setting_count;i++)
            if(s_setting_requested[i]!=s_ble_setting)addresses[count++]=s_setting_requested[i];
        s_setting_applied=s_setting_version;
    }
    xSemaphoreGive(s_lock);
    if(changed)panel_settings_query_start(&s_setting_query,&s_live,addresses,count);
    unsigned slot=s_setting_query.cursor<s_setting_query.count?s_setting_query.cursor:0;
    uint32_t previous_timeout=s_timeout;s_timeout=150;
    bool read=panel_settings_query_tick(&s_setting_query,&s_live,io);
    s_timeout=previous_timeout;
    if(read){
        static const char *const states[]={"READING","VALUE","NO_VALUE","TIMEOUT","UNSUPPORTED","INVALID"};
        const panel_setting_read_t *r=&s_live.setting_reads[slot];
        ESP_LOGI(TAG,"SETTING_READ fc=03 setting=%04X reg=%04X raw=%04X state=%s",
            r->address,s_setting_query.last_reg,s_setting_query.last_raw,
            r->state<sizeof(states)/sizeof(states[0])?states[r->state]:"INVALID");
    }
    if(changed || read)publish(NULL,&s_live);
    return read;
}

static void bus_task(void *arg)
{
    (void)arg;
    /* Keep all fields consumed by the direct home/telemetry sample within its
     * two-second freshness window at 9600 baud. Interleave one diagnostic block
     * per core round instead of delaying live measurements behind all pages. */
    const uint16_t blocks[][2]={{0x50,16},{0x80,16},{0x96,11},{0x150,16},
        {0x330,7},{0x140,5},{0x2a1,1},{0x350,2},{0x176,12}};
    const uint16_t diagnostics[][2]={{0x91,2},{0x38,2},{0x60,16},{0x70,16},
        {0x210,2},{0x218,16}};
    unsigned block=0,diagnostic=0,scan=0,ota_diagnostic=0,query_gap=0;
    bool diagnostic_due=false,identify=true;
    uint32_t next_status=0,next_poll=0;
    panel_control_io_t io={NULL,uptime,sleep_ms,read_regs,write_reg,publish};
    for(;;) {
        atomic_store(&s_tick,uptime(NULL));
        log_link_health();
        if(atomic_load(&s_quiet)) {
            /* Never replay a user's old write after a long upgrade/quiet
             * interval. The controller may have changed while disconnected. */
            if(panel_control_cancel(&s_control,&s_live,&io))complete();
            if(scan){scan=0;s_live.scanning=false;s_target=s_live.config.address;s_timeout=350;
                s_live.write_state=PANEL_WRITE_REJECTED;s_live.exception=0;complete();}
            queued_command_t cancelled;
            if(xQueueReceive(s_commands,&cancelled,0)==pdTRUE){
                s_active_sequence=cancelled.sequence;s_active_type=cancelled.command.type;
                s_live.write_address=cancelled.command.address;s_live.write_value=cancelled.command.value;
                s_live.write_state=PANEL_WRITE_REJECTED;s_live.exception=0;complete();
            }
            ota_diagnostic=0;atomic_store(&s_diag_request,false);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        queued_command_t request;
        if(!panel_control_active(&s_control) && xQueueReceive(s_commands,&request,0)==pdTRUE) {
            panel_command_t command=request.command;
            if(scan && command.type==PANEL_CMD_SCAN_CANCEL)
                panel_result_record(&s_live,s_active_sequence,PANEL_WRITE_IDLE,0);
            s_active_sequence=request.sequence;s_active_type=command.type;
            if(command.type==PANEL_CMD_SCAN_CANCEL) {
                s_live.scanning=false;scan=0;s_target=s_live.config.address;s_timeout=350;
                s_live.write_state=PANEL_WRITE_IDLE;s_live.exception=0;
                complete();next_status=0;next_poll=0;
            } else if(command.type==PANEL_CMD_SCAN) {
                scan=1;s_live.scanning=true;s_live.scan_address=1;s_live.scan_found=0;
                s_live.write_state=PANEL_WRITE_IDLE;
                publish(NULL,&s_live);
            } else {
                if(s_live.scanning) { s_live.scanning=false;scan=0;s_timeout=350; }
                s_target=s_live.config.address;
                if(command.type==PANEL_CMD_CONFIG){
                    change_config(command.config);identify=true;ota_diagnostic=0;atomic_store(&s_diag_request,false);complete();
                }
                else if(command.type==PANEL_CMD_WRITE &&
                    !panel_control_begin(&s_control,&s_live,command.address,command.value,&io))complete();
                next_status=0;next_poll=0;
            }
        }
        /* One confirmation transaction per turn leaves the regular measurement
         * and alarm polling below active during the inverter's processing wait. */
        if(panel_control_active(&s_control) && (int32_t)(uptime(NULL)-next_status)<0 &&
           panel_control_tick(&s_control,&s_live,&io))complete();
        if(scan) {
            s_live.scan_address=(uint8_t)scan;s_target=(uint8_t)scan;s_timeout=150;
            uint16_t status=0;uint8_t exception=0;
            if(read_regs(NULL,0x42,1,&status,&exception)==PANEL_IO_OK && status<=10) {
                s_live.scan_found=(uint8_t)scan;
                ESP_LOGI(TAG,"SCAN found address=%u status=%u",scan,status);
                scan=248; /* Stop at first matching inverter, never change target automatically. */
            } else scan++;
            if(scan>247) {
                scan=0;s_live.scanning=false;s_target=s_live.config.address;s_timeout=350;
                complete();next_status=0;next_poll=0;
            } else publish(NULL,&s_live);
        } else {
            uint32_t now=uptime(NULL);
            if(identify){
                identify=false;
                poll_block(0x38,2);
            } else if((int32_t)(now-next_status)>=0) {
                /* Alarm acquisition is independent of all live measurements,
                 * version words and the unused parallel-state register0045. */
                poll_block(0x41,4);poll_block(0x46,4);next_status=uptime(NULL)+500;
            } else if((int32_t)(now-next_poll)>=0) {
                if(!panel_control_active(&s_control) && query_gap>=3 && service_settings_query(&io)){
                    query_gap=0;
                }else if(diagnostic_due){
                    /* Slow/unsupported OTA diagnostics may each time out.
                     * Allocate one block per normal diagnostic slot rather
                     * than delaying live sampling behind three reads. */
                    if(ota_diagnostic || atomic_exchange(&s_diag_request,false)){
                        static const uint16_t regs[]={0x4090,0x4092,0x4095};
                        uint16_t values[14];uint8_t exception=0;
                        if(read_regs(NULL,regs[ota_diagnostic],ota_diagnostic==2?14:1,values,&exception)==PANEL_IO_OK){
                            if(ota_diagnostic==0)s_live.ota_error=values[0];
                            else if(ota_diagnostic==1)s_live.ota_run=values[0];
                            else memcpy(s_live.ota_info,values,sizeof(values));
                            s_live.ota_valid|=(uint8_t)(1U<<ota_diagnostic);
                            s_live.ota_updated_ms[ota_diagnostic]=uptime(NULL);
                        }
                        ota_diagnostic=(ota_diagnostic+1U)%3U;publish(NULL,&s_live);
                    }else{
                        poll_block(diagnostics[diagnostic][0],diagnostics[diagnostic][1]);
                        diagnostic=(diagnostic+1U)%(sizeof(diagnostics)/sizeof(diagnostics[0]));
                    }
                    diagnostic_due=false;
                }else{
                    uint16_t start=blocks[block][0],count=blocks[block][1];
                    if(panel_is_3ku(&s_live)){
                        if(start==0x96)count=10; /* 3KU table ends at 009F. */
                        if(start==0x330)count=5; /* No 0335/0336 temperature fields. */
                    }
                    poll_block(start,count);
                    if(query_gap<3)query_gap++;
                    block=(block+1U)%(sizeof(blocks)/sizeof(blocks[0]));
                    diagnostic_due=block==0;
                }
                next_poll=uptime(NULL)+40;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

esp_err_t panel_bus_start(void)
{
    if(s_running)return ESP_OK;
    s_live.context_generation=1;atomic_store(&s_context_generation,1);
    s_live.config=(panel_config_t){.address=CONFIG_PANEL_SLAVE_ADDRESS,.baud=CONFIG_PANEL_BAUD};
    (void)config_store(&s_live.config,false);
    panel_profile_t factory_profile=(panel_profile_t)CONFIG_PANEL_FACTORY_PROFILE;
    s_live.profile_locked=panel_profile_supported(factory_profile);
    if(s_live.profile_locked)s_live.config.profile_override=factory_profile;
    s_target=s_live.config.address;
    esp_err_t err=configure_uart(&s_live.config);
    if(err!=ESP_OK)return err;
    err=uart_set_pin(UART_NUM_0,1,0,UART_PIN_NO_CHANGE,UART_PIN_NO_CHANGE);
    if(err!=ESP_OK)return err;
    err=uart_driver_install(UART_NUM_0,2048,0,0,NULL,0);
    if(err!=ESP_OK)return err;
    s_lock=xSemaphoreCreateMutex();s_commands=xQueueCreate(1,sizeof(queued_command_t));
    if(!s_lock || !s_commands){
        if(s_commands){vQueueDelete(s_commands);s_commands=NULL;}
        if(s_lock){vSemaphoreDelete(s_lock);s_lock=NULL;}
        uart_driver_delete(UART_NUM_0);return ESP_ERR_NO_MEM;
    }
    publish(NULL,&s_live);
    if(xTaskCreate(bus_task,"modbus_owner",6144,NULL,5,NULL)!=pdPASS){
        vQueueDelete(s_commands);s_commands=NULL;vSemaphoreDelete(s_lock);s_lock=NULL;
        uart_driver_delete(UART_NUM_0);return ESP_ERR_NO_MEM;
    }
    s_running=true;
    ESP_LOGI(TAG,"UART0 TX=1 RX=0 invert=off %lu 8%c1 addr=%u; no startup writes",
             (unsigned long)s_live.config.baud,"NEO"[s_live.config.parity],s_target);
    return ESP_OK;
}
