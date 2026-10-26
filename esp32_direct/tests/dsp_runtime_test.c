#include <assert.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "arm_poll.h"
#include "direct_link.h"
#include "cs_protocol.h"
#include "direct_dsp_params.h"
#include "direct_dsp_log_gate.h"
#include "direct_iap.h"

#define DSP_PORT 0
#define LINK_STALE_MS 120000U
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(ms) (ms)
#define pdTRUE 1
#define ESP_LOGW(...) log_line(__VA_ARGS__)
#define ESP_LOGI(...) log_line(__VA_ARGS__)
static const char *TAG="direct_link";
RunParamDef g_run_param;
arm_link_stat_t g_arm_stat;
uint16_t g_ctrl_param[CTRL_PARAM_COUNT];
ReadWiteDataDef g_read_write;
static int s_lock=1;
static atomic_bool s_quiet;
static atomic_uint s_dsp_tick;
static atomic_bool s_dsp_started;
static atomic_bool s_iap_busy;
static atomic_bool s_dsp_boot;
static atomic_int s_ui_write_result;
static direct_bms_measurement_t s_bms;
static uint32_t s_bms_last_ms;
static direct_dsp_status_t s_status;
static bool s_status_valid;
static uint32_t s_status_last_ms,s_measurement_last_ms;
static direct_dsp_diagnostics_t s_dsp_diagnostics;
static uint16_t s_config[DIRECT_DSP_CONFIG_COUNT],s_charge_voltage[3];
static bool s_config_valid[DIRECT_DSP_CONFIG_COUNT];
static uint32_t s_config_revision;
static uint64_t s_config_received_ms;
static uint32_t s_config_observation_sequence;
static bool s_config_observed;
static uint16_t s_ctrl_notification[CTRL_PARAM_COUNT];
static uint32_t s_ctrl_notification_revision;
static uint32_t s_dsp_last_report;
static direct_dsp_log_gate_t s_dsp_raw_log[3];
static direct_dsp_params_t s_dsp_params;
static uint32_t s_dsp_params_revision,s_dsp_params_saved_revision,s_dsp_params_retry_ms;
static uint8_t s_dsp_sync_pending;
static bool s_dsp_params_valid,s_dsp_params_importing;
static const char *g_device_sn="H1ZZX00139000079";
static bool fail_save;
static unsigned saves;
static bool dsp_owner_context,mutex_held;
static bool direct_dsp_params_save(const char *sn,const direct_dsp_params_t *params){
    assert(!dsp_owner_context && !mutex_held);
    assert(!strcmp(sn,g_device_sn) && params);saves++;return !fail_save;
}
static bool s_dsp_reported_online;
static arm_write_result_cb_t s_write_cb[2];
static arm_ctrl_snapshot_cb_t s_ctrl_cb;
static struct {
    bool pending,completed,dispatching;
    uint16_t index,value;
    uint32_t start;
    bool cloud;
    bool persisted;
    uint8_t sync_remaining;
    direct_dsp_params_t candidate;
    int result;
} s_write;

typedef struct { uint32_t at; uint8_t bytes[64]; size_t length; } rx_event_t;
typedef struct { uint32_t at; uint8_t bytes[25]; size_t length; } tx_event_t;
static rx_event_t incoming[48];
static tx_event_t outgoing[48];
static unsigned incoming_count,incoming_next,outgoing_count,read_calls;
static unsigned online_logs,offline_logs,sync_logs;
static uint32_t tick,deadline,ota_until;
static bool fail_tx,fail_rx,fail_all_tx,fail_charge;
static bool dsp_owner_context;
static bool mutex_held;
static unsigned queued_events, event_next, capture_logs;
static int s_dsp_uart_events=1;
enum { UART_DATA, UART_FIFO_OVF, UART_BUFFER_FULL, UART_FRAME_ERR,
       UART_PARITY_ERR, UART_BREAK };
typedef struct { int type; } uart_event_t;
static uart_event_t hardware_events[16];
static unsigned uxQueueMessagesWaiting(int queue) {
    assert(queue==1);return queued_events-event_next;
}
static int xQueueReceive(int queue,uart_event_t *event,unsigned wait) {
    assert(queue==1 && wait==0);
    if(event_next==queued_events)return 0;
    *event=hardware_events[event_next++];return pdTRUE;
}
static unsigned write_callback_count,ctrl_callback_count;
static int write_callback_result;
static uint16_t expected_ctrl_voltage;
static jmp_buf finished;

static void log_line(const char *tag,const char *format,...)
{
    /* Formatting must run in the consumer task, never in the UART owner. */
    assert(!dsp_owner_context);
    assert(strcmp(tag,"direct_link")==0);
    char line[1024]; va_list args;va_start(args,format);
    vsnprintf(line,sizeof(line),format,args);va_end(args);
    if(strstr(line,"DSP link=ONLINE"))online_logs++;
    if(strstr(line,"DSP link=NO_FRESH_DATA"))offline_logs++;
    if(strstr(line,"parameter sync requested"))sync_logs++;
    if(strstr(line,"DSP capture"))capture_logs++;
}
static uint32_t now_ms(void) { return tick; }
static uint64_t uptime_epoch;
static int64_t esp_timer_get_time(void){return (int64_t)(uptime_epoch+tick)*1000;}
static int xSemaphoreTake(int lock,unsigned wait)
{(void)wait;assert(lock==1 && !mutex_held);mutex_held=true;return pdTRUE;}
static void xSemaphoreGive(int lock) {assert(lock==1 && mutex_held);mutex_held=false;}
static unsigned uxTaskGetStackHighWaterMark(void *handle)
{(void)handle;return dsp_owner_context?3072U:4096U;}
static void vTaskDelay(unsigned duration) {tick+=duration;}
static bool iap_owner_step(direct_iap_target_t target)
{
    assert(target==DIRECT_IAP_DSP);
    if(tick>=deadline)longjmp(finished,1);
    if(tick<ota_until){tick+=25;return true;}
    return false;
}
static int uart_write_bytes(int port,const void *bytes,size_t length)
{
    assert(port==DSP_PORT && length>0 && length<=25);
    assert(!atomic_load(&s_quiet) && tick>=ota_until);
    assert(outgoing_count<sizeof(outgoing)/sizeof(outgoing[0]));
    tx_event_t *event=&outgoing[outgoing_count++];
    event->at=tick;event->length=length;memcpy(event->bytes,bytes,length);
    if(fail_tx || fail_all_tx || (fail_charge && ((const uint8_t *)bytes)[2]==0x2f)){fail_tx=false;return (int)length-1;}
    return (int)length;
}
static int uart_read_bytes(int port,void *bytes,size_t capacity,unsigned wait)
{
    assert(port==DSP_PORT && wait==25);read_calls++;tick+=wait;
    if(fail_rx){fail_rx=false;return -1;}
    if(incoming_next<incoming_count && incoming[incoming_next].at<=tick){
        rx_event_t *event=&incoming[incoming_next++];
        assert(event->length<=capacity);memcpy(bytes,event->bytes,event->length);
        return (int)event->length;
    }
    return 0;
}

/* 升级失败证据槽: owner 任务只写, dsp_report_tick(main_loop) 打印。
 * 生产定义在 direct_poll.c 的静态区, 位于被抽取代码之前, 这里补一份。 */
static struct {bool pending;bool dsp;int result;
 unsigned rx_bytes;uint8_t head[DIRECT_IAP_RX_HEAD_SIZE];uint8_t head_len;direct_iap_trace_t trace;} s_iap_evidence;

#include "../tmp/dsp_runtime_under_test.inc"

static void crc_frame(uint8_t *frame,size_t length,uint8_t cmd)
{
    frame[0]=1;frame[1]=(uint8_t)(length-4U);frame[2]=cmd;
    uint16_t crc=direct_crc16(frame,length-2U);
    frame[length-2U]=(uint8_t)crc;frame[length-1U]=(uint8_t)(crc>>8);
}
static void word(uint8_t *frame,unsigned at,int value)
{uint16_t raw=(uint16_t)value;frame[at]=(uint8_t)(raw>>8);frame[at+1]=(uint8_t)raw;}
static void enqueue(uint32_t at,const uint8_t *bytes,size_t length)
{
    assert(incoming_count<sizeof(incoming)/sizeof(incoming[0]) && length<=64);
    rx_event_t *event=&incoming[incoming_count++];
    event->at=at;event->length=length;memcpy(event->bytes,bytes,length);
}
static void enqueue_split(uint32_t at,const uint8_t *frame,size_t length)
{
    size_t first=length>7?7:length;
    enqueue(at,frame,first);
    if(length>first)enqueue(at+25,frame+first,length-first);
}
static void parameters(uint8_t bundle[64]);
static void reset(void)
{
    memset(&g_run_param,0,sizeof(g_run_param));memset(&g_arm_stat,0,sizeof(g_arm_stat));
    memset(&s_bms,0,sizeof(s_bms));memset(&s_status,0,sizeof(s_status));
    memset(&s_dsp_diagnostics,0,sizeof(s_dsp_diagnostics));memset(&s_write,0,sizeof(s_write));
    memset(s_config,0,sizeof(s_config));memset(s_config_valid,0,sizeof(s_config_valid));
    memset(s_charge_voltage,0,sizeof(s_charge_voltage));memset(g_ctrl_param,0,sizeof(g_ctrl_param));
    memset(s_ctrl_notification,0,sizeof(s_ctrl_notification));s_ctrl_notification_revision=0;
    s_dsp_last_report=0;s_dsp_reported_online=false;
    memset(s_dsp_raw_log,0,sizeof(s_dsp_raw_log));
    memset(&s_dsp_params,0,sizeof(s_dsp_params));
    s_dsp_params_revision=s_dsp_params_saved_revision=s_dsp_params_retry_ms=0;
    parameters(s_dsp_params.frames);
    s_dsp_params_valid=true;s_dsp_params_revision=s_dsp_params_saved_revision=1;
    s_dsp_params.revision=1;
    s_dsp_sync_pending=0;s_dsp_params_importing=fail_save=fail_all_tx=false;saves=0;
    memset(s_write_cb,0,sizeof(s_write_cb));s_ctrl_cb=NULL;
    atomic_store(&s_quiet,false);atomic_store(&s_dsp_boot,false);atomic_store(&s_dsp_tick,0);atomic_store(&s_ui_write_result,-3);
    atomic_store(&s_dsp_started,true);
    atomic_store(&s_iap_busy,false);
    tick=ota_until=0;deadline=1000;
    incoming_count=incoming_next=outgoing_count=read_calls=0;
    online_logs=offline_logs=sync_logs=0;fail_tx=fail_rx=fail_charge=false;
    queued_events=event_next=capture_logs=0;s_dsp_uart_events=1;
    write_callback_count=ctrl_callback_count=0;write_callback_result=99;mutex_held=false;
    expected_ctrl_voltage=2300;
    s_bms_last_ms=s_status_last_ms=s_measurement_last_ms=s_config_revision=0;s_status_valid=false;
    s_config_received_ms=s_config_observation_sequence=0;s_config_observed=false;
    uptime_epoch=0;
}
static void run_owner(void) {
    dsp_owner_context=true;
    if(!setjmp(finished))dsp_task(NULL);
    dsp_owner_context=false;
}
static void run_task(void) {run_owner();arm_poll_tick();}

static void write_result_callback(uint8_t cmd,uint16_t index,uint16_t bytes,int result)
{
    assert(!dsp_owner_context && !mutex_held);
    assert(cmd==CS_CMD_WRITE_CTRLPARAM && index==7 && bytes==2);
    assert(s_write.pending && s_write.completed && s_write.dispatching);
    assert(arm_poll_write_pending());
    assert(direct_config_write(4,4)==-1);
    write_callback_count++;write_callback_result=result;
}
static void ctrl_snapshot_callback(const uint16_t *values,size_t count,uint32_t revision)
{
    assert(!dsp_owner_context && !mutex_held);
    assert(count==CTRL_PARAM_COUNT && revision==s_config_revision && values[7]==expected_ctrl_voltage);
    ctrl_callback_count++;
}
static void assert_read_only_polls(void)
{
    static const uint8_t q03[9]={1,5,3,0,0,0,0,0x4f,0x95};
    static const uint8_t q04[9]={1,5,4,0,0,0,0,0xfa,0x55};
    assert(outgoing_count>=2);
    for(unsigned i=0;i<outgoing_count;i++){
        const tx_event_t *event=&outgoing[i];
        if(event->bytes[2]<=2){
            uint8_t reply[25];size_t n=direct_dsp_params_reply(&s_dsp_params,event->bytes[2],reply);
            assert(event->length==n && !memcmp(event->bytes,reply,n));continue;
        }
        assert(event->length==9);
        assert(memcmp(event->bytes,event->bytes[2]==3?q03:q04,9)==0);
    }
}
static void status_frame(uint8_t frame[22])
{
    memset(frame,0,22);frame[6]=7;word(frame,7,0x4089);word(frame,15,0x12);
    word(frame,17,0x1008);frame[19]=16;crc_frame(frame,22,3);
}
static void measurement_frame(uint8_t frame[69])
{
    memset(frame,0,69);word(frame,3,230);word(frame,7,500);
    word(frame,27,231);word(frame,33,-234);word(frame,39,523);
    word(frame,41,-125);word(frame,43,-654);word(frame,45,80);
    word(frame,63,9753);word(frame,65,-123);crc_frame(frame,69,4);
}
static void parameters(uint8_t bundle[64]){
    memset(bundle,0,64);
    uint16_t values[]={2300,5000,540,400,1,60,140,3,1,480};
    for(unsigned i=0;i<10;i++)word(bundle,3+2*i,values[i]);
    crc_frame(bundle,25,0);uint8_t *p=bundle+25;
    uint16_t cfg[]={576,2800,900,0x1dda,1,50,95,5,0x0321};
    for(unsigned i=0;i<9;i++)word(p,3+2*i,cfg[i]);
    crc_frame(p,23,1);p=bundle+48;
    word(p,4,20);word(p,6,80);word(p,8,480);word(p,10,560);word(p,12,6000);crc_frame(p,16,2);
}

int main(void)
{
    reset();
    direct_battery_config_t battery;
    assert(direct_battery_config_get(&battery));uint32_t original_revision=battery.revision;
    assert(direct_battery_preset(4,0,&battery));battery.revision=original_revision;
    assert(direct_battery_config_write(&battery,false)==0);
    dsp_params_persist_tick();assert(saves==1 && s_write.persisted && s_write.sync_remaining==15U);
    assert(!s_config_observed);deadline=2000;run_owner();
    bool charge_sent=false;for(unsigned i=0;i<outgoing_count;i++)if(outgoing[i].bytes[2]==0x2f){
        charge_sent=true;assert(outgoing[i].length==11 && direct_dsp_validate(outgoing[i].bytes,11));
    }
    assert(charge_sent && s_write.completed && s_write.result==0);
    dsp_dispatch_notifications();
    assert(direct_battery_config_write(&battery,false)==ARM_WRITE_REVISION_CONFLICT);
    assert(direct_config_write(4,1)==ARM_WRITE_PROFILE_REQUIRED);
    reset();assert(direct_battery_config_get(&battery));original_revision=battery.revision;
    assert(direct_battery_preset(4,0,&battery));battery.revision=original_revision;
    assert(direct_battery_config_write(&battery,false)==0);dsp_params_persist_tick();
    fail_charge=true;deadline=2000;run_owner();
    assert(!s_write.completed && s_write.sync_remaining==8U && (s_dsp_sync_pending&8U));
    assert(s_dsp_params.frames[12]==4); /* already durable, never fake rollback */
    fail_charge=false;deadline=tick+1000;run_owner();assert(s_write.completed && s_write.result==0);
    reset();assert(direct_battery_config_get(&battery));original_revision=battery.revision;
    assert(direct_battery_preset(4,0,&battery));battery.revision=original_revision;
    assert(direct_battery_config_write(&battery,false)==0);fail_save=true;dsp_params_persist_tick();
    assert(s_dsp_params.frames[12]==1 && s_write.result==ARM_WRITE_SAVE_FAILED);
    puts("PASS grouped battery runtime: complete four-frame submission, stale revision, profile-required scalar, storage failure retains AGM");
    /* Admission errors must tell callers whether retrying can help. */
    uint8_t admission_value[2]={0x60,0x09};
    reset();assert(arm_poll_send_write(CS_CMD_WRITE_CTRLPARAM,7,1,admission_value)==-10);
    assert(arm_poll_send_write(CS_CMD_WRITE_R_W_DATA,7,2,admission_value)==-11);
    assert(direct_config_write(83,1)==-11 && !s_write.pending);
    assert(direct_config_write(7,0)==-10 && !s_write.pending);
    atomic_store(&s_dsp_boot,true);assert(direct_config_write(7,2400)==-12);
    atomic_store(&s_dsp_boot,false);atomic_store(&s_quiet,true);assert(direct_config_write(7,2400)==-12);
    atomic_store(&s_quiet,false);atomic_store(&s_iap_busy,true);assert(direct_config_write(7,2400)==-12);
    atomic_store(&s_iap_busy,false);s_dsp_params_valid=false;assert(direct_config_write(7,2400)==-13);
    s_dsp_params_valid=true;s_dsp_params_importing=true;assert(direct_config_write(7,2400)==-1);
    s_dsp_params_importing=false;s_dsp_params_revision++;assert(direct_config_write(7,2400)==-1);
    reset();s_lock=0;assert(direct_config_write(7,2400)==-13);s_lock=1;
    atomic_store(&s_dsp_started,false);assert(direct_config_write(7,2400)==ARM_WRITE_NOT_READY);
    atomic_store(&s_dsp_started,true);
    puts("PASS write admission: invalid, unsupported, upgrade, not-ready, busy");
    reset();direct_dsp_diagnostics_t logging={0};
    logging.measurement_frame.sequence=1;logging.measurement_frame.length=1;logging.measurement_frame.bytes[0]=4;
    logging.config_frame.sequence=1;logging.config_frame.length=1;logging.config_frame.bytes[0]=25;
    s_dsp_diagnostics=logging;atomic_store(&s_dsp_tick,1000);tick=1000;dsp_report_tick();unsigned first_capture_logs=capture_logs;
    assert(first_capture_logs==6);
    atomic_store(&s_dsp_tick,31000);tick=31000;dsp_report_tick();assert(capture_logs==first_capture_logs);
    /* Changed frames during cooldown are coalesced, latest stays queryable. */
    s_dsp_diagnostics.measurement_frame.bytes[0]=5;s_dsp_diagnostics.measurement_frame.sequence++;
    atomic_store(&s_dsp_tick,32000);tick=32000;dsp_report_tick();assert(capture_logs==first_capture_logs+3);
    s_dsp_diagnostics.measurement_frame.bytes[0]=6;s_dsp_diagnostics.measurement_frame.sequence++;
    atomic_store(&s_dsp_tick,32001);tick=32001;dsp_report_tick();assert(capture_logs==first_capture_logs+3);
    direct_dsp_diagnostics_t evidence;assert(direct_dsp_get_diagnostics(&evidence) && evidence.measurement_frame.bytes[0]==6);
    atomic_store(&s_dsp_tick,37000);tick=37000;dsp_report_tick();assert(capture_logs==first_capture_logs+6);
    s_dsp_diagnostics.fault.sequence=1;s_dsp_diagnostics.fault.length=1;
    atomic_store(&s_dsp_tick,38000);tick=38000;dsp_report_tick();assert(capture_logs==first_capture_logs+9);
    s_dsp_diagnostics.fault.sequence++;
    atomic_store(&s_dsp_tick,43000);tick=43000;dsp_report_tick();assert(capture_logs==first_capture_logs+12);
    puts("PASS raw logging: exact content changes, 5-second coalescing, new faults, unchanged raw diagnostics");
    direct_dsp_log_gate_t gate={0};direct_dsp_rx_capture_t wrap={.length=1,.sequence=1};
    assert(direct_dsp_log_due(&gate,&wrap,UINT32_MAX-1000U,false));
    wrap.bytes[0]=1;assert(!direct_dsp_log_due(&gate,&wrap,2000U,false));
    assert(direct_dsp_log_due(&gate,&wrap,4000U,false));
    wrap.sequence++;wrap.at_ms++;assert(!direct_dsp_log_due(&gate,&wrap,10000U,false));
    wrap.reason=DIRECT_DSP_CAPTURE_CRC;assert(direct_dsp_log_due(&gate,&wrap,10000U,true));
    wrap.sequence++;assert(!direct_dsp_log_due(&gate,&wrap,10001U,true));
    assert(direct_dsp_log_due(&gate,&wrap,15000U,true));
    wrap.length=sizeof(wrap.bytes)+1;assert(!direct_dsp_log_due(&gate,&wrap,25000U,true));
#ifndef DIRECT_DSP_OBSERVATION_AVAILABLE
    assert(!"Independent DSP observation must not read the desired store");
#else
    {
    reset();direct_config_observation_t observed={0};
    assert(!direct_config_observed_snapshot_wire(&observed));
    uint8_t observation[43]={0};observation[3]=4;observation[4]=3;observation[12]=1;
    word(observation,7,2200);word(observation,9,5000);crc_frame(observation,sizeof(observation),0x25);
    tick=100;dsp_accept_frame(&s_dsp_diagnostics,observation,sizeof(observation));
    assert(direct_config_observed_snapshot_wire(&observed));
    assert(observed.sequence==1 && observed.received_ms==100 && observed.age_ms==0);
    assert(observed.valid[7] && observed.values[7]==2200 && !observed.valid[41]);
    uint16_t desired;assert(direct_config_get_wire(7,&desired) && desired==2300);
    assert(s_dsp_diagnostics.config_frame.length==43 && s_dsp_diagnostics.config_frame.sequence==1);
    assert(!memcmp(s_dsp_diagnostics.config_frame.bytes,observation,43));
    tick=7000;assert(direct_config_observed_snapshot_wire(&observed) && observed.age_ms==6900);
    assert(observed.values[7]==2200 && observed.sequence==1);
    uptime_epoch=UINT64_C(1)<<32;
    assert(direct_config_observed_snapshot_wire(&observed) && observed.age_ms==(UINT64_C(1)<<32)+6900);
    uptime_epoch=0;
    uint8_t current_status[22],current_measurement[69];
    status_frame(current_status);measurement_frame(current_measurement);
    dsp_accept_frame(&s_dsp_diagnostics,current_status,sizeof(current_status));
    dsp_accept_frame(&s_dsp_diagnostics,current_measurement,sizeof(current_measurement));
    assert(direct_config_observed_snapshot_wire(&observed) && observed.sequence==1 && observed.received_ms==100);
    assert(direct_config_write(7,2400)==0);dsp_params_persist_tick();
    assert(direct_config_get_wire(7,&desired) && desired==2400);
    assert(direct_config_observed_snapshot_wire(&observed) && observed.values[7]==2200 && observed.sequence==1);
    observation[42]^=1;dsp_accept_frame(&s_dsp_diagnostics,observation,sizeof(observation));
    assert(direct_config_observed_snapshot_wire(&observed) && observed.sequence==1 && observed.received_ms==100);
    s_write.pending=false;uint8_t imported[64];parameters(imported);
    assert(direct_dsp_params_install(imported,64,g_device_sn)==0);
    assert(!direct_config_observed_snapshot_wire(&observed));
    crc_frame(observation,sizeof(observation),0x25);tick=8000;
    dsp_accept_frame(&s_dsp_diagnostics,observation,sizeof(observation));
    assert(direct_config_observed_snapshot_wire(&observed) && observed.sequence==2 && observed.age_ms==0);
    atomic_store(&s_dsp_boot,true);assert(!direct_config_observed_snapshot_wire(&observed));
    atomic_store(&s_dsp_boot,false);atomic_store(&s_iap_busy,true);assert(!direct_config_observed_snapshot_wire(&observed));
    puts("PASS independent DSP observations: CRC-valid 25 only, raw capture/age/sequence, desired save and 03/04 do not refresh, import and Boot/IAP isolation");
    }
#endif
    reset();deadline=3000;run_owner();
    unsigned periodic[5]={0};
    for(unsigned i=0;i<outgoing_count;i++){
        unsigned cmd=outgoing[i].bytes[2];if(cmd<5)periodic[cmd]++;
        if(i)assert(outgoing[i].at-outgoing[i-1].at>=110);
    }
    for(unsigned cmd=0;cmd<5;cmd++)assert(periodic[cmd]>=3);
    /* 04 实时测量必须每秒至少被请求一次: 00/01/02 同步再密集也不许挤掉它
     * (旧代码每次同步发完都写 step=3, 04 被饿到 ~8s 一次)。 */
    unsigned measurement_requests=0;uint32_t last_measurement=0,max_measurement_gap=0;
    for(unsigned i=0;i<outgoing_count;i++){
        if(outgoing[i].bytes[2]!=4U)continue;
        if(measurement_requests){
            uint32_t gap=outgoing[i].at-last_measurement;
            if(gap>max_measurement_gap)max_measurement_gap=gap;
        }
        last_measurement=outgoing[i].at;measurement_requests++;
    }
    assert(measurement_requests>=3 && max_measurement_gap<=1200U);
    reset();run_task();assert_read_only_polls();
    assert(s_dsp_diagnostics.tx_frames==outgoing_count && s_dsp_diagnostics.cmd04==0);
    assert(s_dsp_diagnostics.stack_min_free_bytes==3072U);
    reset();s_dsp_params_valid=false;run_owner();
    assert(outgoing_count==0 && read_calls>0); /* no polling before complete bootstrap */
    uint8_t bundle[64];parameters(bundle);
    reset();assert(direct_dsp_params_install(bundle,64,g_device_sn)==0 && saves==1);
    run_owner();assert(s_dsp_diagnostics.sync_replies>=3 && !s_dsp_sync_pending);
    for(unsigned i=0;i<3;i++){
        assert(outgoing[i].bytes[2]==i);
        assert(!memcmp(outgoing[i].bytes,bundle+(i==0?0:i==1?25:48),outgoing[i].length));
        if(i)assert(outgoing[i].at-outgoing[i-1].at>=110);
    }
    direct_dsp_params_t snapshot_params;bool durable=false;
    assert(direct_dsp_params_export(&snapshot_params,&durable) && durable);
    s_config_valid[7]=true;s_config[7]=2300;s_config_valid[4]=true;s_config[4]=1;
    s_charge_voltage[0]=580;g_arm_stat.connected=true;g_arm_stat.last_rx_ms=tick;
    assert(direct_config_write(4,4)==ARM_WRITE_PROFILE_REQUIRED); /* full profile needed for type transition */
    assert(direct_dsp_params_install(bundle,64,g_device_sn)==0);
    assert(!s_config_valid[7] && !s_config_valid[4] && !s_charge_voltage[0] && !g_arm_stat.connected);
    assert(direct_config_write(67,580)==ARM_WRITE_UNSUPPORTED); /* bulk is not present in three packets */
    fail_save=true;assert(direct_dsp_params_install(bundle,64,g_device_sn)==-3);
    assert(direct_dsp_params_export(&snapshot_params,&durable) && durable);
    assert(direct_dsp_params_install(bundle,64,"H1ZZX00139000081")==-2);
    reset();assert(direct_dsp_params_install(bundle,64,g_device_sn)==0);
    fail_tx=true;run_owner();assert(s_dsp_diagnostics.sync_tx_errors==1 && s_dsp_diagnostics.sync_replies>=3);
    reset();assert(direct_dsp_params_install(bundle,64,g_device_sn)==0);
    s_dsp_params_revision++;fail_save=true;arm_poll_tick();
    run_owner();assert(s_dsp_diagnostics.sync_replies==0 && s_dsp_sync_pending==7);
    fail_save=false;tick+=30001;arm_poll_tick();assert(s_dsp_params_saved_revision==s_dsp_params_revision);
    deadline=tick+600;run_owner();assert(s_dsp_diagnostics.sync_replies==3);
    reset();const uint8_t collision[]={1,18,0,0x21,0x90};
    enqueue(100,collision,5);run_owner();
    assert(s_dsp_diagnostics.sync_collision_hints==1 && s_dsp_diagnostics.rx.frames==0);
    assert(s_dsp_diagnostics.sync_replies>=3 && !g_arm_stat.connected);
    reset();s_dsp_params_revision++;atomic_store(&s_quiet,true);arm_poll_tick();assert(saves==0);

    reset();s_config_valid[1]=s_config_valid[10]=true;s_config[1]=140;s_config[10]=5;
    direct_dsp_params_t limits=s_dsp_params;
    assert(!direct_dsp_params_set(&limits,1,20)); /* persisted utility limit is 60A */
    assert(!direct_dsp_params_set(&limits,10,150)); /* persisted total limit is 140A */
    assert(direct_dsp_params_set(&limits,1,100) && direct_dsp_params_set(&limits,10,50));

    reset();queued_events=16;
    for(unsigned i=0;i<16;i++)hardware_events[i].type=UART_DATA;
    hardware_events[0].type=UART_FIFO_OVF;hardware_events[1].type=UART_BUFFER_FULL;
    hardware_events[2].type=UART_FRAME_ERR;hardware_events[3].type=UART_PARITY_ERR;
    hardware_events[4].type=UART_BREAK;
    run_task();
    assert(event_next==16 && s_dsp_diagnostics.uart_queue_full_observations==1);
    assert(s_dsp_diagnostics.uart_fifo_overflows==1 && s_dsp_diagnostics.uart_buffer_full==1);
    assert(s_dsp_diagnostics.uart_frame_errors==1 && s_dsp_diagnostics.uart_parity_errors==1);
    assert(s_dsp_diagnostics.uart_breaks==1 && s_dsp_diagnostics.cmd04==0);

    reset();tick=100;s_bms_last_ms=50;s_bms.current_10ma=-123;
    s_bms.voltage_10mv=5230;s_bms.soc_percent=80;run_task();
    bool current_seen=false,voltage_seen=false;
    for(unsigned i=0;i<outgoing_count;i++){
        const uint8_t *frame=outgoing[i].bytes;assert(direct_dsp_validate(frame,outgoing[i].length));
        if(frame[2]==5){current_seen=true;assert(frame[3]==0xff && frame[4]==0xff && frame[5]==0xfb && frame[6]==0x32);}
        if(frame[2]==6){voltage_seen=true;assert(frame[3]==0x14 && frame[4]==0x6e && frame[5]==0 && frame[6]==80);}
    }
    assert(current_seen && voltage_seen);
    reset();tick=6000;deadline=6800;s_bms_last_ms=100;s_bms.current_10ma=-123;
    run_task();assert_read_only_polls();

    uint8_t status[22],measurement[69];status_frame(status);measurement_frame(measurement);
    reset();deadline=3100;enqueue_split(150,status,sizeof(status));run_task();
    assert(s_dsp_diagnostics.cmd03==1 && s_dsp_diagnostics.cmd04==0 && g_arm_stat.connected);
    assert(g_run_param.FaultValue==7 && s_status_valid && online_logs==0 && offline_logs>0);
    RunParamDef snapshot;uint32_t received;
    assert(!direct_dsp_get_measurement(&snapshot,&received));

    reset();enqueue_split(150,status,sizeof(status));enqueue_split(250,measurement,sizeof(measurement));
    run_task();assert_read_only_polls();
    assert(s_dsp_diagnostics.cmd03==1 && s_dsp_diagnostics.cmd04==1 && online_logs==1);
    assert(s_dsp_diagnostics.measurement_frame.length==69);
    assert(s_dsp_diagnostics.measurement_frame.sequence==1 && capture_logs>=7);
    assert(!memcmp(s_dsp_diagnostics.measurement_frame.bytes,measurement,69));
    assert(g_run_param.GridVolt==230 && g_run_param.ACOutputVolt==231 && (int16_t)g_run_param.OutputWatt==-234);
    assert(g_run_param.BatVolt==523 && g_run_param.BatChgCurr==0 && g_run_param.BatDischgCurr==125);
    assert(g_run_param.BatChrWatt==0 && g_run_param.BatDisChrWatt==654 && g_run_param.BatterySOC==80);
    assert(g_run_param.Ppv==9753 && (int16_t)g_run_param.InvertTemp==-123 && (g_run_param.SysStatus&0x0c)==8);
    assert(direct_dsp_get_measurement(&snapshot,&received) && received==s_measurement_last_ms);
    tick=received+2001;assert(!direct_dsp_get_measurement(&snapshot,&received));

    reset();uint8_t short04[68]={0};crc_frame(short04,sizeof(short04),4);
    enqueue_split(150,short04,sizeof(short04));run_task();
    assert(s_dsp_diagnostics.rx.frames==1 && s_dsp_diagnostics.rejected_frames==1 && s_dsp_diagnostics.cmd04==0);
    assert(!g_arm_stat.connected && !direct_dsp_get_measurement(&snapshot,&received));

    /* Save the desired record before any owner sends it; no 25 is required. */
    uint8_t config[43]={0};config[3]=4;config[4]=3;config[12]=1;
    word(config,9,5000);word(config,13,580);word(config,15,560);word(config,17,440);
    reset();s_write_cb[0]=write_result_callback;
    assert(direct_config_write(7,2400)==0);s_write.cloud=true;
    run_owner();assert(outgoing_count==0 && !s_write.persisted);
    arm_poll_tick();assert(saves==1 && s_write.persisted);
    uint16_t desired;assert(direct_config_get(7,&desired) && desired==2400);
    deadline=tick+700;run_owner();
    assert(s_write.pending && s_write.completed && s_write.result==0 && s_write.sync_remaining==0);
    assert(write_callback_count==0 && direct_config_write(7,2300)==-1);
    arm_poll_tick();assert(write_callback_count==1 && write_callback_result==0 && !s_write.pending);
    arm_poll_tick();assert(write_callback_count==1);
    direct_dsp_params_t factory_defaults;assert(direct_dsp_params_arm_defaults(&factory_defaults));
    assert(direct_dsp_params_get(&factory_defaults,7,&desired) && desired==2300);

    reset();fail_save=true;s_write_cb[0]=write_result_callback;
    assert(direct_config_write(7,2400)==0);s_write.cloud=true;
    arm_poll_tick();assert(s_write.completed && !s_write.persisted && s_write.result==ARM_WRITE_SAVE_FAILED);
    assert(direct_config_get(7,&desired) && desired==2300);
    run_owner();assert(outgoing_count==0);
    arm_poll_tick();assert(write_callback_count==1 && write_callback_result==ARM_WRITE_SAVE_FAILED && !s_write.pending);

    reset();s_write_cb[0]=write_result_callback;
    assert(direct_config_write(7,2400)==0);s_write.cloud=true;arm_poll_tick();
    deadline=tick+5800;fail_all_tx=true;run_owner();
    assert(s_write.completed && s_write.result==ARM_WRITE_SUBMIT_FAILED && s_write.persisted);
    arm_poll_tick();assert(write_callback_count==1 && write_callback_result==ARM_WRITE_SUBMIT_FAILED && !s_write.pending);
    assert(direct_config_get(7,&desired) && desired==2400); /* saved record retries through periodic cycle */

    /* Both remote routes use the same engineering-unit map and enqueue. */
    const uint16_t indexes[]={7,8,1,10,11,14,15,41},wire_values[]={2400,6000,1200,500,490,0,0,1},native_values[]={2400,6000,120,50,490,0,1,1};
    for(unsigned channel=0;channel<3;channel++)for(unsigned i=0;i<8;i++){
        reset();uint8_t bytes[2]={(uint8_t)wire_values[i],(uint8_t)(wire_values[i]>>8)};
        assert((channel==0?direct_config_write(indexes[i],native_values[i]):arm_poll_send_write(CS_CMD_WRITE_CTRLPARAM,indexes[i],2,bytes))==0);
        arm_poll_tick();assert(saves==1 && direct_config_get(indexes[i],&desired) && desired==native_values[i]);
        uint16_t exported;assert(direct_config_get_wire(indexes[i],&exported) && exported==wire_values[i]);
        run_owner();assert(s_write.completed && s_write.sync_remaining==0);
        arm_poll_tick();assert(!s_write.pending);
    }
    puts("PASS controller user settings: three ingress routes share durable save then sync; no CMD25 prerequisite; failed save preserves old record; wire units roundtrip; immutable factory defaults");
    reset();assert(direct_dsp_params_set(&s_dsp_params,14,0) && direct_dsp_params_set(&s_dsp_params,15,1) && direct_dsp_params_set(&s_dsp_params,41,0));
    assert(direct_config_reset_local()==0);arm_poll_tick();run_owner();assert(s_write.completed);
    assert(direct_config_get(14,&desired) && desired==1);
    assert(direct_config_get(15,&desired) && desired==0);
    assert(direct_config_get(41,&desired) && desired==1);
    assert(direct_config_get(7,&desired) && desired==2300);

    /* Repeated snapshots can coalesce while the consumer sleeps. Other known
     * local fields are prevalidated solely to exercise the existing full-
     * snapshot callback gate; actual 25 parsing stays production code. */
    reset();s_ctrl_cb=ctrl_snapshot_callback;expected_ctrl_voltage=2400;
    for(unsigned i=0;i<CTRL_PARAM_COUNT;i++)s_config_valid[i]=true;
    word(config,7,2300);crc_frame(config,sizeof(config),0x25);
    enqueue_split(150,config,sizeof(config));word(config,7,2400);crc_frame(config,sizeof(config),0x25);
    enqueue_split(250,config,sizeof(config));run_owner();
    assert(ctrl_callback_count==0 && s_config_revision==2 && s_ctrl_notification_revision==2);
    arm_poll_tick();assert(ctrl_callback_count==1 && !s_ctrl_notification_revision);
    arm_poll_tick();assert(ctrl_callback_count==1);

    reset();s_dsp_params_valid=false;deadline=3100;uint8_t sync[5]={0};crc_frame(sync,sizeof(sync),0);
    enqueue(150,sync,sizeof(sync));run_task();assert(outgoing_count==0);
    assert(s_dsp_diagnostics.sync_requests==1 && s_dsp_diagnostics.sync_command==0 && sync_logs>0);
    assert(!g_arm_stat.connected && !s_config_revision && !s_write.pending);

    reset();memset(config,0,sizeof(config));config[3]=4;config[4]=3;config[12]=1;
    word(config,7,2300);word(config,9,5000);word(config,13,580);
    word(config,15,560);word(config,17,440);crc_frame(config,sizeof(config),0x25);
    enqueue_split(150,config,sizeof(config));run_task();
    assert(s_dsp_diagnostics.cmd25==1 && s_config_revision==1 && s_config_valid[7]);
    assert(s_config[7]==2300 && g_ctrl_param[7]==2300 && g_ctrl_param[8]==5000);
    assert(s_charge_voltage[0]==580 && s_charge_voltage[1]==560 && s_charge_voltage[2]==440);
    assert(!g_arm_stat.connected && !direct_dsp_get_measurement(&snapshot,&received));

    reset();uint8_t unknown[5]={0};crc_frame(unknown,sizeof(unknown),0x7f);
    enqueue(100,unknown,sizeof(unknown));enqueue(150,measurement,10);
    enqueue_split(500,measurement,sizeof(measurement));run_task();
    assert(s_dsp_diagnostics.unhandled_frames==1 && s_dsp_diagnostics.rx.timeouts==1);
    assert(s_dsp_diagnostics.cmd04==1 && g_run_param.GridVolt==230);

    reset();uint8_t noise[]={0x55,0x00,1,0xff,0x33};enqueue(100,noise,sizeof(noise));
    status[21]^=1;enqueue(150,status,sizeof(status));status[21]^=1;
    enqueue_split(250,measurement,sizeof(measurement));run_task();
    assert(s_dsp_diagnostics.rx.crc_errors>0 && s_dsp_diagnostics.rx.length_errors>0 && s_dsp_diagnostics.rx.noise_bytes>0);
    assert(s_dsp_diagnostics.cmd04==1 && g_run_param.GridVolt==230);

    reset();deadline=500;fail_tx=true;fail_rx=true;run_task();
    assert(s_dsp_diagnostics.tx_errors==1 && s_dsp_diagnostics.tx_frames+1==outgoing_count);
#ifdef DSP_RUNTIME_HAS_RX_ERRORS
    assert(s_dsp_diagnostics.rx_errors==1);
#else
    fputs("FAIL: actual DSP task must count uart_read_bytes failures separately from empty reads\n",stderr);
    return 1;
#endif
    reset();deadline=400;atomic_store(&s_quiet,true);run_task();assert(!outgoing_count && !read_calls);
    reset();deadline=400;atomic_store(&s_dsp_boot,true);run_task();assert(!outgoing_count && !read_calls);
    assert(!direct_config_writable(7) && direct_config_write(7,2400)==ARM_WRITE_UPGRADE_BUSY);
    assert(direct_dsp_params_install(s_dsp_params.frames,64,g_device_sn)==-1);
    reset();deadline=400;ota_until=400;run_task();assert(!outgoing_count && !read_calls);
    reset();deadline=1000;ota_until=400;run_task();assert_read_only_polls();
    assert(outgoing[0].at>=400 && outgoing[0].bytes[2]==0);
    puts("PASS continuous DSP synchronization: 00/01/02 repeat with 03/04, all slots >=110ms; no stale BMS injection");
    direct_dsp_diagnostics_t timing={0};tick=100;dsp_note_tx(&timing);tick=701;dsp_note_tx(&timing);
    assert(timing.tx_frames==2 && timing.last_tx_ms==701 && timing.max_tx_gap_ms==601 && timing.tx_gap_over_500_ms==1);
    atomic_store(&s_dsp_boot,true);assert(direct_dsp_get_diagnostics(&timing) && (timing.poll_hold_reason&4U));
    atomic_store(&s_dsp_boot,false);
    puts("PASS DSP TX timing: actual successful enqueue gaps and explicit Boot/poll hold reasons");
    puts("PASS: real DSP UART task transcripts: durable 00/01/02 before polling; missing/dirty params hold polls; TX failure retries; collision hint replays trusted profile without accepting bad bytes; full 04 capture and UART hardware event counters; no owner logging/callbacks; consumer dispatch preserves pending through callbacks; real 25 observations and coalesced snapshots; exact polls, fresh BMS, signed data, parser recovery, I/O errors, quiet/OTA, stack watermark bytes");
    return 0;
}
