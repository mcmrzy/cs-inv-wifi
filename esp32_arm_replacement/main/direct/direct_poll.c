/* Transitional implementation of the L10 arm_poll API. No ARM UART is used.
 * DSP and BMS each own one UART; cloud/telemetry consumers still read the
 * existing RunParamDef snapshot until their interfaces are migrated. */
#include "arm_poll.h"
#include "direct_protocol.h"
#include "direct_link.h"
#include "direct_dsp_params_store.h"
#include "direct_dsp_ota_transport.h"
#include "cs_protocol.h"
#include "config.h"
#include "mqtt_handler.h"

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
#include "freertos/queue.h"

#define DSP_PORT UART_NUM_0
#define BMS_PORT UART_NUM_1
#define DSP_BAUD 115200
#define DSP_POLL_INTERVAL_MS 110U /* App(10) ProcessDspCom: eleven 10ms ticks. */
/* 至少每秒向 DSP 要一次实时测量(04)。00/01/02 同步帧无论多密集都不能把它挤掉:
 * 旧代码把同步相位和轮询相位共用一个 step, 每次同步结束都写 step=3, 于是轮询
 * 永远只发 03, 04 实测被饿到 ~8s 一次, 屏幕因此长期判"数据过期"。 */
#define DSP_MEASUREMENT_POLL_MS 1000U
#define BMS_BAUD 9600
#define BMS_POLL_INTERVAL_MS 500U /* BBJ RS485 V1.6 section 3.1.1 */
#define LINK_STALE_MS 120000U

RunParamDef g_run_param;
OnlyReadDataDef g_only_read;
ReadWiteDataDef g_read_write;
uint16_t g_ctrl_param[CTRL_PARAM_COUNT];
arm_link_stat_t g_arm_stat;

static const char *TAG = "direct_link";
static SemaphoreHandle_t s_lock;
static atomic_bool s_quiet;
static atomic_bool s_dsp_started,s_bms_started;
static atomic_uint s_dsp_tick,s_bms_tick;
typedef struct {
    direct_dsp_control_t operation; /* 0=flash, otherwise owner-only control. */
    direct_iap_image_t image;
    direct_iap_io_t source;
    SemaphoreHandle_t done;
    direct_iap_result_t result;
    atomic_uint progress_bytes;
    atomic_bool progress_pending;
    /* 升级作业期间收到的原始字节: 失败时打出来, 直接回答"目标到底回没回、
     * 回的是什么"。本固件里这个 UART 只有本 owner 任务读(见 iap_receive),
     * 这里只统计本作业实际从驱动读到的字节, 不代表线上的全部波形。 */
    unsigned rx_bytes;
    uint8_t rx_head[DIRECT_IAP_RX_HEAD_SIZE];
    uint8_t rx_head_len;
    direct_iap_trace_t trace;
} iap_job_t;
/* s_lock protects publication/claim. Once claimed, only its target UART owner
 * accesses the job until giving done. The synchronous caller owns its lifetime. */
static iap_job_t *s_iap_pending;
static atomic_bool s_iap_busy;
static atomic_bool s_dsp_boot;
static TaskHandle_t s_dsp_owner,s_bms_owner;
static QueueHandle_t s_dsp_uart_events;
static direct_dsp_params_t s_dsp_params;
static uint32_t s_dsp_params_revision,s_dsp_params_saved_revision,s_dsp_params_retry_ms;
static uint8_t s_dsp_sync_pending;
static bool s_dsp_params_valid,s_dsp_params_importing;
#define DSP_UART_EVENT_QUEUE_LENGTH 16
static direct_bms_measurement_t s_bms;
static uint32_t s_bms_last_ms;
static direct_bms_identity_t s_bms_identity;
static bool s_bms_identity_pending;
static uint8_t s_bms_identity_next;
static uint32_t s_bms_identity_last_ms,s_bms_identity_generation;
static arm_write_result_cb_t s_write_cb[2];
static arm_ctrl_snapshot_cb_t s_ctrl_cb;
static uint32_t s_last_utc_ms;
static direct_dsp_status_t s_status;
static bool s_status_valid;
static uint32_t s_status_last_ms;
static uint32_t s_measurement_last_ms;
static direct_dsp_diagnostics_t s_dsp_diagnostics;
static uint32_t s_dsp_last_report;
static bool s_dsp_reported_online;
/* 直连升级失败时 owner 记下的原始接收证据, 由 dsp_report_tick(main_loop) 打印。
 * "rx=0 bytes" 表示本作业没有读到字节; "head=48 52" 表示读到了 HR。 */
static struct {
    bool pending;
    bool dsp;                /* true=DSP, false=BMS (避免依赖 direct_iap 类型) */
    int result;
    unsigned rx_bytes;
    uint8_t head[DIRECT_IAP_RX_HEAD_SIZE];
    uint8_t head_len;
    direct_iap_trace_t trace;
} s_iap_evidence;
static uint16_t s_ctrl_notification[CTRL_PARAM_COUNT];
static uint32_t s_ctrl_notification_revision;
static uint16_t s_config[DIRECT_DSP_CONFIG_COUNT];
static bool s_config_valid[DIRECT_DSP_CONFIG_COUNT];
static uint32_t s_config_revision;
static uint16_t s_charge_voltage[3];
static struct {
    bool pending, completed, dispatching;
    int result;
    uint16_t index, value;
    uint32_t start;
    bool cloud;
    bool persisted;
    uint8_t sync_remaining;
    direct_dsp_params_t candidate;
} s_write;
/* UI completion survives a subsequent cloud write until the next UI enqueue. */
static atomic_int s_ui_write_result = -3;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void iap_heartbeat(const iap_job_t *job)
{
    atomic_store(job->image.target==DIRECT_IAP_DSP?&s_dsp_tick:&s_bms_tick,now_ms());
}
static uart_port_t iap_port(const iap_job_t *job)
{
    return job->image.target==DIRECT_IAP_DSP?DSP_PORT:BMS_PORT;
}
static bool iap_read(void *ctx,size_t offset,uint8_t *bytes,size_t n)
{
    iap_job_t *job=ctx;iap_heartbeat(job);
    return job->source.read_image(job->source.context,offset,bytes,n);
}
static bool iap_send(void *ctx,const uint8_t *bytes,size_t n)
{
    iap_job_t *job=ctx;uart_port_t port=iap_port(job);
    unsigned baud=port==DSP_PORT?DSP_BAUD:BMS_BAUD;
    if(!n)return true;
    /* bmsBoot treats 5 ms idle as frame end and does not reassemble fragments.
     * Enqueue the entire frame once (BMS ring 8192 > maximum frame 4106), then
     * poll completion. Application scheduling must not insert inter-chunk gaps. */
    iap_heartbeat(job);
    uint32_t start=now_ms();
    unsigned timeout=(unsigned)(((uint64_t)n*10000U+baud-1U)/baud)+500U;
    job->trace.tx_frames++;job->trace.tx_len=(unsigned)n;
    memset(job->trace.tx_head,0,sizeof(job->trace.tx_head));
    memcpy(job->trace.tx_head,bytes,n<3U?n:3U);
    if(n>=2U)memcpy(job->trace.tx_crc,bytes+n-2U,2U);
    bool completed=false;
    bool written=uart_write_bytes(port,bytes,n)==(int)n;
    while(written){
        iap_heartbeat(job);
        unsigned elapsed=now_ms()-start;
        if(elapsed>=timeout)break;
        unsigned slice=timeout-elapsed;if(slice>25)slice=25;
        esp_err_t err=uart_wait_tx_done(port,pdMS_TO_TICKS(slice));
        if(err==ESP_OK){iap_heartbeat(job);completed=true;break;}
        if(err!=ESP_ERR_TIMEOUT)break;
    }
    unsigned elapsed=now_ms()-start;
    if(elapsed>job->trace.tx_max_ms)job->trace.tx_max_ms=elapsed;
    return completed;
}
static void iap_phase(void *ctx,direct_iap_phase_t phase)
{
    iap_job_t *job=ctx;
    if(job->trace.phase!=phase){
        memset(&job->trace,0,sizeof(job->trace));job->trace.phase=phase;
    }
}
static size_t iap_receive(void *ctx,uint8_t *bytes,size_t n,unsigned timeout_ms)
{
    iap_job_t *job=ctx;uint32_t start=now_ms();
    do{
        iap_heartbeat(job);
        unsigned elapsed=now_ms()-start;
        unsigned slice=elapsed>=timeout_ms?0:timeout_ms-elapsed;
        if(slice>25)slice=25;
        int got=uart_read_bytes(iap_port(job),bytes,n,pdMS_TO_TICKS(slice));
        if(got>0){
            job->rx_bytes+=(unsigned)got;
            job->trace.rx_bytes+=(unsigned)got;
            for(int i=0;i<got&&job->rx_head_len<sizeof(job->rx_head);++i)
                job->rx_head[job->rx_head_len++]=(uint8_t)bytes[i];
            for(int i=0;i<got&&job->trace.rx_head_len<sizeof(job->trace.rx_head);++i)
                job->trace.rx_head[job->trace.rx_head_len++]=(uint8_t)bytes[i];
            return (size_t)got;
        }
    }while(now_ms()-start<timeout_ms);
    return 0;
}
static void iap_delay(void *ctx,unsigned ms)
{
    iap_job_t *job=ctx;
    while(ms){unsigned slice=ms>25?25:ms;iap_heartbeat(job);
        vTaskDelay(pdMS_TO_TICKS(slice));ms-=slice;}
    iap_heartbeat(job);
}
static uint32_t iap_now(void *ctx){(void)ctx;return now_ms();}
static void iap_flush(void *ctx)
{
    iap_job_t *job=ctx;iap_heartbeat(job);uart_flush_input(iap_port(job));
}
static bool iap_cancelled(void *ctx)
{
    iap_job_t *job=ctx;
    return job->source.cancelled&&job->source.cancelled(job->source.context);
}
static bool iap_verify_identity(void *ctx,const direct_iap_image_t *image)
{
    iap_job_t *job=ctx;
    return job->source.verify_identity(job->source.context,image);
}
static bool iap_verify_version(void *ctx,const direct_iap_image_t *image)
{
    iap_job_t *job=ctx;
    return job->source.verify_version(job->source.context,image);
}
static void iap_progress(void *ctx,size_t done,size_t total)
{
    (void)total;iap_job_t *job=ctx;iap_heartbeat(job);
    atomic_store(&job->progress_bytes,(unsigned)done);
    atomic_store(&job->progress_pending,true);
}
static bool iap_owner_step(direct_iap_target_t target)
{
    iap_job_t *job=NULL;
    if(xSemaphoreTake(s_lock,pdMS_TO_TICKS(20))==pdTRUE){
        if(s_iap_pending&&s_iap_pending->image.target==target){
            job=s_iap_pending;s_iap_pending=NULL;
        }
        xSemaphoreGive(s_lock);
    }
    if(!job)return false;
    /* Normal transaction has finished before entering this function; no second
     * task reads this UART. Drain any previous TX before entering Boot. */
    bool drained=uart_wait_tx_done(iap_port(job),pdMS_TO_TICKS(1000))==ESP_OK;
    /* DSP normal polls are asynchronous; let their final response finish before
     * discarding it, otherwise a late application reply can prefix Boot HR. */
    if(drained&&target==DIRECT_IAP_DSP)iap_delay(job,250);
    iap_flush(job);
    direct_iap_io_t io={.read_image=iap_read,.send=iap_send,.receive=iap_receive,
        .delay=iap_delay,.cancelled=iap_cancelled,.flush=iap_flush,
        .progress=iap_progress,.context=job,.now_ms=iap_now,.phase=iap_phase,
        .probe_packet0=job->source.probe_packet0,
        .verify_identity=job->source.verify_identity?iap_verify_identity:NULL,
        .verify_version=job->source.verify_version?iap_verify_version:NULL};
    job->result=DIRECT_IAP_TX_FAILED;
    if(drained){
        if(target==DIRECT_IAP_DSP){
            /* Preflight reads only the staged file, before Boot/erase commands. */
            job->result=job->operation?DIRECT_IAP_OK:direct_dsp_ota_validate(job->image.size,&io);
            if(job->result==DIRECT_IAP_OK){
                atomic_store(&s_dsp_boot,true);
                if(job->operation==DIRECT_DSP_ENTER_BOOT || job->operation==DIRECT_DSP_REBOOT)
                    job->result=direct_dsp_ota_enter_boot(&io);
                if(!job->operation)job->result=direct_dsp_ota_flash(job->image.size,&io);
                else if(job->result==DIRECT_IAP_OK && job->operation!=DIRECT_DSP_ENTER_BOOT)
                    job->result=direct_dsp_ota_exit_boot(&io);
                if(job->result==DIRECT_IAP_OK && job->operation!=DIRECT_DSP_ENTER_BOOT)
                    atomic_store(&s_dsp_boot,false);
            }
        }else job->result=direct_iap_run_legacy(&job->image,&io);
    }
    iap_flush(job);
    /* Clear stale success timestamps after a target restart. A new application
     * response must re-establish data freshness; old config is not replayed. */
    if(xSemaphoreTake(s_lock,portMAX_DELAY)==pdTRUE){
        if(target==DIRECT_IAP_DSP){
            g_arm_stat.connected=false;s_status_valid=false;
            s_status_last_ms=0;s_measurement_last_ms=0;
            memset(s_config_valid,0,sizeof(s_config_valid));
            s_dsp_sync_pending|=7U;
        }else s_bms_last_ms=0;
        xSemaphoreGive(s_lock);
    }
    iap_heartbeat(job);
    /* Snapshot failure BEFORE recovery changes the phase to EOT. */
    if(job->result!=DIRECT_IAP_OK){
        if(s_lock&&xSemaphoreTake(s_lock,pdMS_TO_TICKS(50))==pdTRUE){
            s_iap_evidence.pending=true;
            s_iap_evidence.dsp=(target==DIRECT_IAP_DSP);
            s_iap_evidence.result=(int)job->result;
            s_iap_evidence.rx_bytes=job->rx_bytes;
            s_iap_evidence.head_len=job->rx_head_len;
            memcpy(s_iap_evidence.head,job->rx_head,sizeof(s_iap_evidence.head));
            s_iap_evidence.trace=job->trace;
            xSemaphoreGive(s_lock);
        }
    }
    /* 升级失败/取消后必须回到正常业务: 向目标发 EOT 结束 Boot 会话, 再清掉 Boot
     * 挂起, 否则轮询与遥测一直停着(旧策略"失败保持暂停"会让设备看起来像死了)。
     * 目标若已跑起旧 App, EOT 只是被忽略的 6 字节; 若停在 Bootloader, 它会在
     * 镜像头校验不通过时继续监听, 不影响下一次升级。 */
    if(target==DIRECT_IAP_DSP && job->operation==0 && job->result!=DIRECT_IAP_OK &&
       !job->source.probe_packet0 && atomic_load(&s_dsp_boot)){
        if(direct_dsp_ota_exit_boot(&io)==DIRECT_IAP_OK)io.delay(io.context,1000);
        atomic_store(&s_dsp_boot,false);
    }
    /* 失败证据只在这里记录, 由 dsp_report_tick(main_loop) 打印: owner 任务里
     * 任何 printf/vfprintf 都会走 newlib 锁路径, 4096B 栈装不下(0.6.8/0.6.21
     * 两次实机栈溢出都是这个原因)。 */
    /* Last access to job: caller may release it as soon as the signal arrives. */
    xSemaphoreGive(job->done);
    return true;
}

static direct_iap_result_t link_submit_iap(const direct_iap_image_t *image,
                                          const direct_iap_io_t *caller_io,
                                          direct_dsp_control_t operation)
{
    if(!image||!caller_io||(!operation&&!caller_io->read_image)||!s_lock||
       (image->target!=DIRECT_IAP_DSP&&image->target!=DIRECT_IAP_BMS)||
       xTaskGetCurrentTaskHandle()==s_dsp_owner||
       xTaskGetCurrentTaskHandle()==s_bms_owner)return DIRECT_IAP_INCOMPATIBLE;
    if(!atomic_load(image->target==DIRECT_IAP_DSP?&s_dsp_started:&s_bms_started))
        return DIRECT_IAP_NOT_READY;
    bool expected=false;
    if(!atomic_compare_exchange_strong(&s_iap_busy,&expected,true))return DIRECT_IAP_BUSY;
    iap_job_t job={.image=*image,.source=*caller_io,.operation=operation};
    atomic_init(&job.progress_bytes,0);atomic_init(&job.progress_pending,false);
    job.done=xSemaphoreCreateBinary();
    if(!job.done){atomic_store(&s_iap_busy,false);return DIRECT_IAP_NO_MEMORY;}
    xSemaphoreTake(s_lock,portMAX_DELAY);
    if(s_write.pending || s_dsp_params_importing){xSemaphoreGive(s_lock);vSemaphoreDelete(job.done);
        atomic_store(&s_iap_busy,false);return DIRECT_IAP_BUSY;}
    bool was_quiet=atomic_exchange(&s_quiet,true);
    s_iap_pending=&job;
    xSemaphoreGive(s_lock);
    for(;;){
        bool done=xSemaphoreTake(job.done,pdMS_TO_TICKS(25))==pdTRUE;
        if(atomic_exchange(&job.progress_pending,false)&&caller_io->progress)
            caller_io->progress(caller_io->context,atomic_load(&job.progress_bytes),image->size);
        if(done)break;
    }
    direct_iap_result_t result=job.result;
    vSemaphoreDelete(job.done);
    atomic_store(&s_quiet,was_quiet);atomic_store(&s_iap_busy,false);
    return result;
}
direct_iap_result_t direct_link_run_iap(const direct_iap_image_t *image,const direct_iap_io_t *io){
    return link_submit_iap(image,io,0);
}
direct_iap_result_t direct_link_dsp_control(direct_dsp_control_t operation){
    if(operation<DIRECT_DSP_ENTER_BOOT || operation>DIRECT_DSP_REBOOT)return DIRECT_IAP_INCOMPATIBLE;
    direct_iap_image_t image={.target=DIRECT_IAP_DSP};direct_iap_io_t io={0};
    return link_submit_iap(&image,&io,operation);
}
bool direct_dsp_boot_active(void){return atomic_load(&s_dsp_boot);}

/* Shared enqueue for cloud, BLE and keys. Called with s_lock held. */
static int queue_user_write(uint16_t index,uint16_t value,bool cloud){
    if(s_write.pending || s_dsp_params_importing || !s_dsp_params_valid ||
       s_dsp_params_revision!=s_dsp_params_saved_revision || s_dsp_params_revision==UINT32_MAX || atomic_load(&s_quiet) || atomic_load(&s_iap_busy) || atomic_load(&s_dsp_boot))return -1;
    direct_dsp_params_t candidate=s_dsp_params;
    if(!direct_dsp_params_set(&candidate,index,value))return -1;
    candidate.revision=s_dsp_params_revision+1U;
    memset(&s_write,0,sizeof(s_write));s_write.index=index;s_write.value=value;
    s_write.candidate=candidate;s_write.cloud=cloud;s_write.pending=true;
    if(!cloud)atomic_store(&s_ui_write_result,1);
    return 0;
}

static uint16_t collector_value(uint16_t index,uint16_t raw)
{
    static const uint8_t battery_type[6]={0,1,2,4,3,5};
    static const uint8_t charge_priority[5]={0,1,0,2,3};
    switch (index) {
        case 4: return raw<6 ? battery_type[raw] : 0;
        case 1: case 10: return (uint16_t)(raw*10U);
        case 15:return raw?0:1;
        case 13: return raw<5 ? charge_priority[raw] : 0;
        case 32: case 33: return (uint16_t)(raw<<8);
        default: return raw;
    }
}

static bool collector_to_dsp(uint16_t index,uint16_t *value)
{
    static const uint8_t battery_type[6]={0,1,2,4,3,5};
    static const uint8_t charge_priority[4]={0,1,3,4};
    switch (index) {
        case 4:
            if (*value>=6) return false;
            *value=battery_type[*value]; break;
        case 1: case 10:
            if (*value%10U)return false;
            *value=(uint16_t)(*value/10U);break;
        case 15:
            if(*value>1)return false;
            *value=(uint16_t)(1U-*value);break;
        case 13:
            if (*value<1 || *value>=4) return false;
            *value=charge_priority[*value]; break;
        default: break;
    }
    return true;
}

static bool uart_setup(uart_port_t port, int tx, int rx, int baud, int de)
{
    if (tx < 0 || rx < 0) return false;
    const uart_config_t cfg = {
        .baud_rate = baud,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    /* DSP: no software TX ring. uart_write_bytes fills hardware FIFO directly;
     * 9/13-byte commands fit entirely in its 128 bytes. Larger data still needs
     * FIFO refills, so this does not prove its on-wire inter-byte gap.
     * BMS retains whole-frame buffering for its 5ms idle delimiter. */
    int tx_ring=port==BMS_PORT?8192:0;
    esp_err_t err = uart_driver_install(port, 1024, tx_ring,
        port==DSP_PORT?DSP_UART_EVENT_QUEUE_LENGTH:0,
        port==DSP_PORT?&s_dsp_uart_events:NULL, 0);
    if (err == ESP_OK) err = uart_param_config(port, &cfg);
    if (err == ESP_OK) err = uart_set_pin(port, tx, rx, de, UART_PIN_NO_CHANGE);
    /* Original ARM PA3 is GPIO_MODE_IPU. The ESP GPIO-matrix UART RX
     * routing enables input but does not restore this idle-high bias. */
    if (err == ESP_OK && port == DSP_PORT) err = gpio_set_pull_mode(rx, GPIO_PULLUP_ONLY);
    if (err == ESP_OK && de >= 0) err = uart_set_mode(port, UART_MODE_RS485_HALF_DUPLEX);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART%d setup failed: %s", port, esp_err_to_name(err));
        uart_driver_delete(port);
        if(port==DSP_PORT)s_dsp_uart_events=NULL;
        return false;
    }
    if(port==DSP_PORT)ESP_LOGW(TAG,"DSP RX GPIO%d input pullup enabled (ARM PA3 equivalent)",rx);
    return true;
}

static bool apply_dsp04(const direct_dsp_measurement_t *m)
{
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) return false;
    RunParamDef *p = &g_run_param;
    p->GridVolt = m->grid_v;
    p->GridFreq = m->grid_hz_tenths;
    p->ACInWatt = m->grid_w;
    p->ACInVA = m->grid_va;
    p->ACChrWatt = m->grid_w;
    p->ACChrVA = m->grid_va;
    p->ACDisChrWatt = m->grid_w;
    p->ACDisChrVA = m->grid_va;
    p->ACChrCurr = m->grid_i;
    p->GridPercent = m->grid_percent;
    p->ACOutputVolt = m->load_v;
    p->ACOutputFreq = m->load_hz_tenths;
    p->OutputCurr = m->load_i;
    p->OutputWatt = m->load_w;
    p->OutputVA = m->load_va;
    p->LoadPercent = m->load_percent;
    p->VinvtA = m->inverter_v;
    p->IinvtA = m->inverter_i;
    p->InvtFreq = m->inverter_hz_tenths;
    p->Pinvt = m->inverter_w;
    p->Sinvt = m->inverter_va;
    p->InvtPercent = m->inverter_percent;
    p->BatVolt = m->battery_v_tenths;
    p->BatterySOC = m->battery_soc <= 100 ? m->battery_soc : 0;
    p->BatChgCurr = m->battery_i_tenths > 0 ? (uint16_t)m->battery_i_tenths : 0;
    p->BatDischgCurr = m->battery_i_tenths < 0 ?
        (uint16_t)(-(int32_t)m->battery_i_tenths) : 0;
    p->BatChrWatt = m->battery_w > 0 ? (uint16_t)m->battery_w : 0;
    p->BatDisChrWatt = m->battery_w < 0 ?
        (uint16_t)(-(int32_t)m->battery_w) : 0;
    p->BusVolt = m->bus_v_tenths;
    p->Ibus = m->bus_i;
    p->Vpv1 = m->pv1_v;
    p->Vpv2 = m->pv2_v;
    p->Buck1Curr = m->pv1_i;
    p->Buck2Curr = m->pv2_i;
    p->Ppv1 = m->pv1_w;
    p->Ppv2 = m->pv2_w;
    p->Ppv = m->pv_total_w;
    p->InvertTemp = m->inverter_temp_tenths;
    p->SysStatus = (uint16_t)((p->SysStatus & ~0x0cU) |
        (m->battery_i_tenths > 0 ? 0x04U : m->battery_i_tenths < 0 ? 0x08U : 0U));
    g_arm_stat.connected = true;
    g_arm_stat.last_rx_ms = now_ms();
    s_measurement_last_ms = g_arm_stat.last_rx_ms;
    g_arm_stat.poll_count++;
    xSemaphoreGive(s_lock);
    return true;
}

static bool handle_dsp_frame(const uint8_t *frame, size_t len)
{
    if (len < 5) return false;
    direct_dsp_measurement_t m;
    direct_dsp_status_t status;
    if (frame[2] == 0x04 && direct_dsp_parse_cmd04(frame, len, &m)) {
        return apply_dsp04(&m);
    } else if (direct_dsp_parse_cmd03(frame, len, &status)) {
        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) return false;
        s_status = status; s_status_valid = true; s_status_last_ms=now_ms();
        g_run_param.FaultValue = status.fault;
        g_run_param.Warning = status.warning[1];
        g_run_param.BmsWarning = status.warning[2] & 0x1fffU;
        g_run_param.BatOverCharge = (status.warning[2] >> 3) & 1U;
        const uint16_t bits = status.inverter_status[0];
        /* App(10) SystemManage.h bitfield, preserve current-based battery bits. */
        uint16_t sys = g_run_param.SysStatus & 0x0cU;
        if (status.mode == 0) sys |= 1;
        if (status.fault) sys |= 2;
        if (bits & 0x0080U) sys |= 0x0080U; /* RelayAcL: bypass */
        if (bits & 0x0008U) sys |= 0x0100U; /* LoadValid */
        if (bits & 0x0030U) sys |= 0x0200U; /* Pv1/Pv2Status */
        if (bits & 0x0001U) sys |= 0x0400U; /* GridValid */
        if (bits & 0x0002U) sys |= 0x0800U; /* GenValid */
        if ((bits & 0x0030U) && (bits & 0x4000U)) sys |= 0x10U;
        if ((bits & 0x0001U) && (bits & 0x4000U)) sys |= 0x20U;
        if ((bits & 0x0002U) && (bits & 0x4000U)) sys |= 0x40U;
        g_run_param.SysStatus = sys;
        g_arm_stat.connected = true; g_arm_stat.last_rx_ms = now_ms();
        g_arm_stat.poll_count++;
        xSemaphoreGive(s_lock);
        return true;
    } else if (frame[2] == 0x25) {
        bool complete_snapshot=true;
        uint32_t revision=0;
        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) return false;
        if (direct_dsp_parse_cmd25(frame,len,s_config,s_config_valid)) {
            for (unsigned i=0;i<3;++i)
                s_charge_voltage[i]=(uint16_t)(((uint16_t)frame[13+i*2]<<8)|frame[14+i*2]);
            for (unsigned i=0;i<CTRL_PARAM_COUNT;++i)
                g_ctrl_param[i]=collector_value((uint16_t)i,s_config[i]);
            g_arm_stat.last_ctrl_ms=now_ms();
            revision=++s_config_revision;
            for (unsigned i=0;i<CTRL_PARAM_COUNT;++i)
                if (!s_config_valid[i]) complete_snapshot=false;
            if(complete_snapshot && s_ctrl_cb){
                memcpy(s_ctrl_notification,g_ctrl_param,sizeof(s_ctrl_notification));
                s_ctrl_notification_revision=revision;
            }
            /* 25 is an observation only. It must not replace saved user
             * limits or authorize unsaved settings. */
        }
        xSemaphoreGive(s_lock);
        return revision!=0;
    }
    return false;
}

static void dsp_accept_frame(void *context,const uint8_t *frame,size_t length)
{
    direct_dsp_diagnostics_t *d=context;
    d->last_frame_ms=now_ms();d->last_command=frame[2];d->last_length=(uint8_t)length;
    d->prefix_length=(uint8_t)(length<sizeof(d->prefix)?length:sizeof(d->prefix));
    memcpy(d->prefix,frame,d->prefix_length);
    if(frame[2]<=2){
        if(length!=5){d->rejected_frames++;return;}
        d->sync_requests++;d->sync_command=frame[2];
        if(xSemaphoreTake(s_lock,pdMS_TO_TICKS(20))==pdTRUE){
            s_dsp_sync_pending|=(uint8_t)(1U<<frame[2]);xSemaphoreGive(s_lock);
        }
        return;
    }
    if(frame[2]!=3 && frame[2]!=4 && frame[2]!=0x25){d->unhandled_frames++;return;}
    if(!handle_dsp_frame(frame,length)){d->rejected_frames++;return;}
    if(frame[2]==3){d->cmd03++;d->last_status_ms=now_ms();}
    else if(frame[2]==4){
        d->cmd04++;d->last_measurement_ms=now_ms();
        memcpy(d->measurement_frame.bytes,frame,length);
        d->measurement_frame.length=(uint16_t)length;
        d->measurement_frame.expected_length=(uint16_t)length;
        d->measurement_frame.at_ms=d->last_measurement_ms;
        d->measurement_frame.sequence=d->cmd04;
    }
    else d->cmd25++;
}

/* Bounded 16-byte lines keep formatting away from the UART owner. */
static void dsp_dump_capture(const char *kind,const direct_dsp_rx_capture_t *capture)
{
    if(!capture->sequence)return;
    ESP_LOGW(TAG,"DSP capture kind=%s seq=%lu time_ms=%lu",
        kind,(unsigned long)capture->sequence,(unsigned long)capture->at_ms);
    ESP_LOGW(TAG,"DSP capture got=%u expected=%u reason=%u",
        capture->length,capture->expected_length,capture->reason);
    static const char hex[]="0123456789ABCDEF";
    for(size_t offset=0;offset<capture->length;offset+=16){
        char text[49];size_t used=0;
        for(size_t i=offset;i<capture->length && i<offset+16;i++){
            text[used++]=hex[capture->bytes[i]>>4];
            text[used++]=hex[capture->bytes[i]&15];text[used++]=' ';
        }
        text[used]=0;
        ESP_LOGW(TAG,"DSP capture %s offset=%u: %s",kind,(unsigned)offset,text);
    }
}

static void dsp_uart_event_tick(direct_dsp_diagnostics_t *d)
{
    if(!s_dsp_uart_events)return;
    if(uxQueueMessagesWaiting(s_dsp_uart_events)>=DSP_UART_EVENT_QUEUE_LENGTH)
        d->uart_queue_full_observations++;
    uart_event_t event;
    for(unsigned i=0;i<DSP_UART_EVENT_QUEUE_LENGTH &&
        xQueueReceive(s_dsp_uart_events,&event,0)==pdTRUE;i++){
        switch(event.type){
        case UART_FIFO_OVF:d->uart_fifo_overflows++;break;
        case UART_BUFFER_FULL:d->uart_buffer_full++;break;
        case UART_FRAME_ERR:d->uart_frame_errors++;break;
        case UART_PARITY_ERR:d->uart_parity_errors++;break;
        case UART_BREAK:d->uart_breaks++;break;
        default:break;
        }
    }
}

int direct_dsp_params_install(const uint8_t *bundle,size_t length,const char *sn)
{
    direct_dsp_params_t next;
    if(!sn || strcmp(sn,g_device_sn) || !direct_dsp_params_decode(bundle,length,&next))return -2;
    if(!s_lock || xSemaphoreTake(s_lock,pdMS_TO_TICKS(50))!=pdTRUE)return -1;
    if(s_write.pending || s_dsp_params_importing || s_dsp_params_revision==UINT32_MAX || atomic_load(&s_quiet) || atomic_load(&s_iap_busy) || atomic_load(&s_dsp_boot)){
        xSemaphoreGive(s_lock);return -1;
    }
    next.buzzer=s_dsp_params_valid?s_dsp_params.buzzer:true;next.revision=s_dsp_params_revision+1U;
    s_dsp_params_importing=true;xSemaphoreGive(s_lock);
    /* Flash work is performed by the authorized USB caller, never UART owner. */
    bool saved=direct_dsp_params_save(sn,&next);
    xSemaphoreTake(s_lock,portMAX_DELAY);
    if(saved){s_dsp_params=next;s_dsp_params_valid=true;s_dsp_params_revision++;
        s_dsp_params_saved_revision=s_dsp_params_revision;s_dsp_sync_pending|=7U;
        g_read_write.CtrlParamAlterTime=s_dsp_params_revision;
        g_arm_stat.last_ctrl_ms=now_ms();
        /* No old active-type cache may be used for writes after an import. */
        memset(s_config_valid,0,sizeof(s_config_valid));memset(s_charge_voltage,0,sizeof(s_charge_voltage));
        g_arm_stat.connected=false;s_status_valid=false;s_status_last_ms=0;s_measurement_last_ms=0;
    }
    s_dsp_params_importing=false;xSemaphoreGive(s_lock);
    return saved?0:-3;
}
bool direct_dsp_params_export(direct_dsp_params_t *params,bool *durable)
{
    if(!params || !durable || !s_lock || xSemaphoreTake(s_lock,pdMS_TO_TICKS(50))!=pdTRUE)return false;
    bool valid=s_dsp_params_valid;
    if(valid){*params=s_dsp_params;*durable=s_dsp_params_revision==s_dsp_params_saved_revision;}
    xSemaphoreGive(s_lock);return valid;
}
static void dsp_params_persist_tick(void)
{
    if(atomic_load(&s_quiet) || atomic_load(&s_iap_busy))return;
    direct_dsp_params_t snapshot;uint32_t revision=0,now=now_ms();bool user_write=false;
    if(!s_lock || xSemaphoreTake(s_lock,pdMS_TO_TICKS(10))!=pdTRUE)return;
    if(s_write.pending && !s_write.persisted && !s_write.completed && !s_dsp_params_importing){
        snapshot=s_write.candidate;user_write=true;s_dsp_params_importing=true;
    }else if(s_dsp_params_valid && !s_dsp_params_importing &&
       s_dsp_params_revision!=s_dsp_params_saved_revision &&
       (!s_dsp_params_retry_ms || now-s_dsp_params_retry_ms>=30000U)){
        snapshot=s_dsp_params;revision=s_dsp_params_revision;snapshot.revision=revision;
        s_dsp_params_importing=true;s_dsp_params_retry_ms=now;
    }
    xSemaphoreGive(s_lock);if(!revision && !user_write)return;
    bool saved=direct_dsp_params_save(g_device_sn,&snapshot);
    xSemaphoreTake(s_lock,portMAX_DELAY);
    if(user_write){
        if(saved){
            s_dsp_params=snapshot;s_dsp_params_revision++;s_dsp_params_saved_revision=s_dsp_params_revision;
            g_read_write.CtrlParamAlterTime=s_dsp_params_revision;
            s_dsp_sync_pending|=7U;s_write.persisted=true;s_write.sync_remaining=7U;s_write.start=now;
            s_config_revision++;g_arm_stat.last_ctrl_ms=now;
            for(unsigned i=0;i<CTRL_PARAM_COUNT;i++){uint16_t value;if(direct_dsp_params_get(&s_dsp_params,(uint16_t)i,&value))g_ctrl_param[i]=collector_value((uint16_t)i,value);}
        }else{s_write.completed=true;s_write.result=-2;if(!s_write.cloud)atomic_store(&s_ui_write_result,-2);}
    }else if(saved){s_dsp_params.revision=revision;s_dsp_params_saved_revision=revision;s_dsp_params_retry_ms=0;g_read_write.CtrlParamAlterTime=revision;}
    s_dsp_params_importing=false;xSemaphoreGive(s_lock);
    if(!saved)ESP_LOGW(TAG,"DSP user parameter save failed; previous record retained");
}

static void dsp_report(const direct_dsp_diagnostics_t *d,uint32_t now)
{
    ESP_LOGW(TAG,"DSP TX health age_ms=%lu max_gap_ms=%lu gaps_over_500=%lu lock_misses=%lu hold=0x%02X",
        (unsigned long)(d->last_tx_ms?now-d->last_tx_ms:UINT32_MAX),
        (unsigned long)d->max_tx_gap_ms,(unsigned long)d->tx_gap_over_500_ms,
        (unsigned long)d->poll_lock_misses,d->poll_hold_reason);
    bool online=d->last_measurement_ms && now-d->last_measurement_ms<=2000U;
    ESP_LOGW(TAG,"DSP link=%s tx=%lu rx_bytes=%lu frames=%lu",online?"ONLINE":"NO_FRESH_DATA",
        (unsigned long)d->tx_frames,(unsigned long)d->rx.rx_bytes,(unsigned long)d->rx.frames);
    ESP_LOGW(TAG,"DSP frames 03=%lu 04=%lu 25=%lu age04_ms=%lu",(unsigned long)d->cmd03,
        (unsigned long)d->cmd04,(unsigned long)d->cmd25,
        (unsigned long)(d->last_measurement_ms?now-d->last_measurement_ms:UINT32_MAX));
    ESP_LOGW(TAG,"DSP errors tx=%lu rx=%lu crc=%lu len=%lu timeout=%lu",
        (unsigned long)d->tx_errors,(unsigned long)d->rx_errors,(unsigned long)d->rx.crc_errors,
        (unsigned long)d->rx.length_errors,(unsigned long)d->rx.timeouts);
    ESP_LOGW(TAG,"DSP rejected=%lu unknown=%lu last_cmd=0x%02X last_len=%u",
        (unsigned long)d->rejected_frames,(unsigned long)d->unhandled_frames,
        d->last_command,d->last_length);
    ESP_LOGW(TAG,"DSP stack_min_free=%lu consumer_stack_min_free=%lu (bytes)",
        (unsigned long)d->stack_min_free_bytes,(unsigned long)uxTaskGetStackHighWaterMark(NULL));
    ESP_LOGW(TAG,"DSP UART fifo_overflow=%lu buffer_full=%lu frame_error=%lu",
        (unsigned long)d->uart_fifo_overflows,(unsigned long)d->uart_buffer_full,
        (unsigned long)d->uart_frame_errors);
    ESP_LOGW(TAG,"DSP UART parity_error=%lu break=%lu queue_full_seen=%lu",
        (unsigned long)d->uart_parity_errors,(unsigned long)d->uart_breaks,
        (unsigned long)d->uart_queue_full_observations);
    dsp_dump_capture("04",&d->measurement_frame);
    dsp_dump_capture("FAULT",&d->fault);
    if(d->sync_requests)ESP_LOGW(TAG,"DSP parameter sync requested: cmd=0x%02X count=%lu",d->sync_command,(unsigned long)d->sync_requests);
    ESP_LOGW(TAG,"DSP sync ready=%u durable=%u pending=0x%02X",d->sync_params_ready,d->sync_params_durable,d->sync_pending_mask);
    /* sent = ESP 实际发出的 00/01/02 帧数; requests = DSP 发来的同步请求数。 */
    ESP_LOGW(TAG,"DSP sync sent=%lu requests=%lu tx_errors=%lu",
        (unsigned long)d->sync_replies,(unsigned long)d->sync_requests,(unsigned long)d->sync_tx_errors);
    ESP_LOGW(TAG,"DSP sync collision_hints=%lu",(unsigned long)d->sync_collision_hints);
    if(d->prefix_length){
        static const char hex[]="0123456789ABCDEF";
        char text[sizeof(d->prefix)*3+1];size_t used=0;
        for(unsigned i=0;i<d->prefix_length;i++){
            text[used++]=hex[d->prefix[i]>>4];text[used++]=hex[d->prefix[i]&15];text[used++]=' ';
        }
        text[used]=0;
        ESP_LOGW(TAG,"DSP RX prefix: %s",text);
    }
    RunParamDef run;uint32_t received;
    if(online && direct_dsp_get_measurement(&run,&received)){
        ESP_LOGW(TAG,"DSP data raw: grid_V=%u output_V=%u output_W=%d",run.GridVolt,run.ACOutputVolt,(int16_t)run.OutputWatt);
        ESP_LOGW(TAG,"DSP battery_dV=%u charge_dA=%u discharge_dA=%u",run.BatVolt,run.BatChgCurr,run.BatDischgCurr);
        ESP_LOGW(TAG,"DSP pv_W=%u temp_dC=%d",run.Ppv,(int16_t)run.InvertTemp);
    }
}

/* Called only by arm_poll_tick in main_loop. No formatting or network/BLE
 * callback is permitted in the DSP UART owner. */
static void dsp_report_tick(void)
{
    direct_dsp_diagnostics_t d;
    /* 升级失败证据优先打印: 它不依赖 DSP 是否在线, 且必须在 main_loop 里格式化。 */
    if(s_lock&&xSemaphoreTake(s_lock,pdMS_TO_TICKS(50))==pdTRUE){
        bool pending=s_iap_evidence.pending,dsp=s_iap_evidence.dsp;
        int result=s_iap_evidence.result;
        unsigned rx_bytes=s_iap_evidence.rx_bytes,head_len=s_iap_evidence.head_len;
        uint8_t raw_head[DIRECT_IAP_RX_HEAD_SIZE];memcpy(raw_head,s_iap_evidence.head,sizeof(raw_head));
        direct_iap_trace_t trace=s_iap_evidence.trace;
        s_iap_evidence.pending=false;
        xSemaphoreGive(s_lock);
        if(pending){
            char head[3*DIRECT_IAP_TRACE_HEAD_SIZE+1];int used=0;
            for(unsigned i=0;i<head_len&&used+3<(int)sizeof(head);++i)
                used+=snprintf(head+used,sizeof(head)-used,"%02X ",raw_head[i]);
            if(used)head[used-1]='\0';
            ESP_LOGW(TAG,"IAP %s %s (%d): rx=%u bytes head=%s",
                     dsp?"dsp":"bms",result==DIRECT_IAP_DIAGNOSTIC_COMPLETE?"diagnostic":"failed",
                     result,rx_bytes,head_len?head:"(none)");
            static const char *const phases[]={"NONE","HR","ERASE_ACK","ERASE_CR","DATA_ACK","EOT"};
            used=0;
            for(unsigned i=0;i<trace.rx_head_len&&used+3<(int)sizeof(head);++i)
                used+=snprintf(head+used,sizeof(head)-used,"%02X ",trace.rx_head[i]);
            if(used)head[used-1]='\0';
            ESP_LOGW(TAG,"IAP phase=%s tx_frames=%u tx_len=%u tx_max_ms=%u rx=%u head=%s",
                     (unsigned)trace.phase<sizeof(phases)/sizeof(phases[0])?phases[trace.phase]:"UNKNOWN",
                     trace.tx_frames,trace.tx_len,trace.tx_max_ms,trace.rx_bytes,
                     trace.rx_head_len?head:"(none)");
            ESP_LOGW(TAG,"IAP tx_head=%02X %02X %02X tx_crc=%02X %02X (send completion is not flash verification)",
                     trace.tx_head[0],trace.tx_head[1],trace.tx_head[2],trace.tx_crc[0],trace.tx_crc[1]);
        }
    }
    if(!direct_dsp_get_diagnostics(&d) || !d.task_running || d.quiet)return;
    uint32_t now=now_ms();
    bool online=d.last_measurement_ms && now-d.last_measurement_ms<=2000U;
    if((online!=s_dsp_reported_online && (online || s_dsp_last_report)) ||
       (!s_dsp_last_report && now>=3000U) || now-s_dsp_last_report>=30000U){
        dsp_report(&d,now);s_dsp_last_report=now;s_dsp_reported_online=online;
    }
}

static void dsp_dispatch_notifications(void)
{
    uint16_t ctrl[CTRL_PARAM_COUNT],index=0;
    uint32_t revision=0;bool completed=false,cloud=false;int result=0;
    if(!s_lock || xSemaphoreTake(s_lock,pdMS_TO_TICKS(50))!=pdTRUE)return;
    if(s_ctrl_notification_revision){
        memcpy(ctrl,s_ctrl_notification,sizeof(ctrl));revision=s_ctrl_notification_revision;
        s_ctrl_notification_revision=0;
    }
    if(s_write.pending && s_write.completed && !s_write.dispatching){
        s_write.dispatching=true;completed=true;
        cloud=s_write.cloud;index=s_write.index;result=s_write.result;
    }
    xSemaphoreGive(s_lock);
    if(revision && s_ctrl_cb)s_ctrl_cb(ctrl,CTRL_PARAM_COUNT,revision);
    if(completed){
        if(result==0)ESP_LOGI(TAG,"User config index=%u saved on ESP and three sync frames submitted; DSP application unconfirmed",index);
        else ESP_LOGW(TAG,"User config index=%u save or sync failed, result=%d",index,result);
        if(cloud)for(unsigned i=0;i<2;++i)
            if(s_write_cb[i])s_write_cb[i](CS_CMD_WRITE_CTRLPARAM,index,2,result);
        /* pending remains true through callbacks, so no new same-index write
         * can be mistaken for this completion. */
        xSemaphoreTake(s_lock,portMAX_DELAY);
        s_write.pending=false;s_write.completed=false;s_write.dispatching=false;
        xSemaphoreGive(s_lock);
    }
}

static void dsp_note_tx(direct_dsp_diagnostics_t *d)
{
    uint32_t current=now_ms();
    if(d->tx_frames){
        uint32_t gap=current-d->last_tx_ms;
        if(gap>d->max_tx_gap_ms)d->max_tx_gap_ms=gap;
        if(gap>500U)d->tx_gap_over_500_ms++;
    }
    d->last_tx_ms=current;d->tx_frames++;
}
static void dsp_task(void *arg)
{
    (void)arg;
    direct_dsp_stream_t stream={0};
    direct_dsp_diagnostics_t diagnostics={0};
    uint8_t request[9];
    uint8_t step = 0;             /* 0..4: 3,4 = 03/04 轮询; 0..2 = 常态轮发 00/01/02 */
    uint32_t last_measurement_poll = 0;
    uint32_t last_tx = 0;
    uint32_t fault_seen=0,last_collision_sync=0;
    for (;;) {
        atomic_store(&s_dsp_tick,now_ms());
        dsp_uart_event_tick(&diagnostics);
        if(iap_owner_step(DIRECT_IAP_DSP)){
            stream.used=0;last_tx=now_ms();step=0;
            last_measurement_poll=now_ms();continue;
        }
        if (atomic_load(&s_quiet) || atomic_load(&s_dsp_boot)) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        const uint32_t now = now_ms();
        direct_dsp_stream_feed(&stream,NULL,0,now,dsp_accept_frame,&diagnostics);
        if (now - last_tx >= DSP_POLL_INTERVAL_MS) {
            uint8_t write_frame[25]; size_t write_len=0;uint8_t sync_cmd=3;bool allow_poll=false;
            if (xSemaphoreTake(s_lock,pdMS_TO_TICKS(20)) == pdTRUE) {
                allow_poll=s_dsp_params_valid && !s_dsp_params_importing &&
                    s_dsp_params_revision==s_dsp_params_saved_revision && (!s_write.pending || s_write.persisted);
                if(s_dsp_params_valid && !s_dsp_params_importing &&
                   s_dsp_params_revision==s_dsp_params_saved_revision && (!s_write.pending || s_write.persisted)){
                    /* 5 时隙循环: step 0/1/2 是同步槽, 3/4 是轮询槽。同步请求只在
                     * 同步槽里被消耗, 绝不占用轮询槽, 也不重置轮询相位 —— 旧代码
                     * 每次同步发完都写 step=3, DSP 持续请求同步时 04 实测就被挤到
                     * ~8s 一次, 屏幕长期判"数据过期"。 */
                    if(allow_poll && step<3){
                        if(s_dsp_sync_pending){
                        for(sync_cmd=0;sync_cmd<3;sync_cmd++)if(s_dsp_sync_pending&(1U<<sync_cmd))break;
                        write_len=direct_dsp_params_reply(&s_dsp_params,sync_cmd,write_frame);
                        s_dsp_sync_pending&=(uint8_t)~(1U<<sync_cmd);
                        }else if(s_write.pending && !s_write.persisted){
                            step=3;
                        }else{
                            sync_cmd=step;write_len=direct_dsp_params_reply(&s_dsp_params,sync_cmd,write_frame);
                        }
                    }
                }
                if (s_write.pending && s_write.persisted && !s_write.completed && now-s_write.start>5000U) {
                    s_write.completed=true;s_write.result=-1;
                    if (!s_write.cloud) atomic_store(&s_ui_write_result,-1);
                    g_arm_stat.ctrl_err++;
                }
                xSemaphoreGive(s_lock);
            }else diagnostics.poll_lock_misses++;
            if (write_len) {
                if(uart_write_bytes(DSP_PORT,write_frame,write_len)==(int)write_len){
                    dsp_note_tx(&diagnostics);if(sync_cmd<3)diagnostics.sync_replies++;
                    if(sync_cmd<3){
                        xSemaphoreTake(s_lock,portMAX_DELAY);
                        if(s_write.pending && s_write.persisted && !s_write.completed){
                            s_write.sync_remaining&=(uint8_t)~(1U<<sync_cmd);
                            if(!s_write.sync_remaining){s_write.completed=true;s_write.result=0;if(!s_write.cloud)atomic_store(&s_ui_write_result,2);}
                        }
                        xSemaphoreGive(s_lock);
                    }
                }else{
                    diagnostics.tx_errors++;
                    if(sync_cmd<3){
                        diagnostics.sync_tx_errors++;
                        xSemaphoreTake(s_lock,portMAX_DELAY);s_dsp_sync_pending|=(uint8_t)(1U<<sync_cmd);xSemaphoreGive(s_lock);
                    }
                }
                /* 同步槽用掉一格: 0→1→2 之后进入轮询槽 3。 */
                if(sync_cmd<3)step=(uint8_t)(step<2U?step+1U:3U);
                last_tx=now;
                continue;
            }
            if(allow_poll){
            uint8_t cmd;
            int32_t current_ma = 0;
            uint16_t voltage = 0, soc = 0;
            bool bms_fresh = false;
            if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
                bms_fresh = s_bms_last_ms && now - s_bms_last_ms < 5000U;
                if (bms_fresh) {
                    current_ma = s_bms.current_10ma * 10;
                    voltage = s_bms.voltage_10mv;
                    soc = s_bms.soc_percent;
                }
                xSemaphoreGive(s_lock);
            }
            if(!bms_fresh && step>=5)step=3;
            cmd=step;
            /* 兜底: 距上次请求 04 超过 1s 就优先要实时测量, 同步帧挤不掉它。 */
            if((uint32_t)(now-last_measurement_poll)>=DSP_MEASUREMENT_POLL_MS)cmd=4U;
            /* Never inject a made-up BMS sample into the power controller. */
            if (bms_fresh || cmd <= 0x04) {
                size_t bytes=direct_dsp_make_request(cmd, current_ma, voltage, soc, request);
                if(uart_write_bytes(DSP_PORT, request, bytes)==(int)bytes){
                    dsp_note_tx(&diagnostics);
                    if(cmd==4U)last_measurement_poll=now;
                }else diagnostics.tx_errors++;
            }
            step = (uint8_t)((step + 1U) % (bms_fresh?7U:5U));
            }
            last_tx = now;
        }
        uint8_t chunk[64];
        const int n = uart_read_bytes(DSP_PORT, chunk, sizeof(chunk), pdMS_TO_TICKS(25));
        if(n>0){
            diagnostics.prefix_length=(uint8_t)((size_t)n<sizeof(diagnostics.prefix)?(size_t)n:sizeof(diagnostics.prefix));
            memcpy(diagnostics.prefix,chunk,diagnostics.prefix_length);
            direct_dsp_stream_feed(&stream,chunk,(size_t)n,now_ms(),dsp_accept_frame,&diagnostics);
        }else if(n<0)diagnostics.rx_errors++;
        diagnostics.rx=stream.stats;diagnostics.last_byte_ms=stream.last_byte_ms;
        diagnostics.fault=stream.fault;
        if(fault_seen!=stream.fault.sequence){
            fault_seen=stream.fault.sequence;
            if(direct_dsp_params_collision_hint(stream.fault.bytes,stream.fault.length)){
                diagnostics.sync_collision_hints++;
                if(!last_collision_sync || now_ms()-last_collision_sync>=3000U){
                    if(xSemaphoreTake(s_lock,pdMS_TO_TICKS(20))==pdTRUE){
                        if(s_dsp_params_valid && !s_dsp_params_importing && !s_write.pending &&
                           s_dsp_params_revision==s_dsp_params_saved_revision){
                            s_dsp_sync_pending|=7U;last_collision_sync=now_ms();
                        }
                        xSemaphoreGive(s_lock);
                    }
                }
            }
        }
        diagnostics.stack_min_free_bytes=(uint32_t)uxTaskGetStackHighWaterMark(NULL);
        if(xSemaphoreTake(s_lock,0)==pdTRUE){
            diagnostics.sync_pending_mask=s_dsp_sync_pending;
            diagnostics.sync_params_ready=s_dsp_params_valid;
            diagnostics.sync_params_durable=s_dsp_params_valid && s_dsp_params_revision==s_dsp_params_saved_revision;
            s_dsp_diagnostics=diagnostics;
            g_arm_stat.err_count=stream.stats.crc_errors+stream.stats.length_errors+stream.stats.timeouts+diagnostics.rejected_frames;
            xSemaphoreGive(s_lock);
        }
    }
}

static void bms_identity_step(void)
{
    uint8_t type;uint32_t generation;
    if(xSemaphoreTake(s_lock,pdMS_TO_TICKS(20))!=pdTRUE)return;
    if(!s_bms_identity_pending){xSemaphoreGive(s_lock);return;}
    type=s_bms_identity_next;generation=s_bms_identity_generation;xSemaphoreGive(s_lock);
    uint8_t query[8],rx[128];size_t have=0;
    direct_bms_identity_make_query(type,query);
    uart_flush_input(BMS_PORT);uart_write_bytes(BMS_PORT,query,sizeof(query));
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
            ESP_LOGI(TAG,"BMS optional identity scan completed, valid_mask=0x%02X; not upgrade authorization",s_bms_identity.valid_mask);
        }
    }
    xSemaphoreGive(s_lock);
}

static void bms_task(void *arg)
{
    (void)arg;
    uint8_t request[8], frame[256];
    const TickType_t period = pdMS_TO_TICKS(BMS_POLL_INTERVAL_MS);
    TickType_t last_wake = xTaskGetTickCount();
    uint32_t failed=0,last_warning=0;
    for (;;) {
        atomic_store(&s_bms_tick,now_ms());
        if(iap_owner_step(DIRECT_IAP_BMS)){
            last_wake = xTaskGetTickCount();
            continue;
        }
        if (atomic_load(&s_quiet)) {
            vTaskDelay(pdMS_TO_TICKS(100));
            last_wake = xTaskGetTickCount();
            continue;
        }
        size_t request_len;
#if CONFIG_DIRECT_BMS_7C
        request_len = direct_bms_make_7c_query(request);
#else
        request_len = direct_bms_make_query(1, 0, 0x25, request);
#endif
        uart_flush_input(BMS_PORT);
        uart_write_bytes(BMS_PORT, request, request_len);
        size_t have = 0;
        const uint32_t start = now_ms();
        uint32_t last_rx = start;
        while (have < sizeof(frame) && now_ms() - start < 300U &&
               (!have || now_ms() - last_rx < 30U)) {
            int n = uart_read_bytes(BMS_PORT, frame + have, sizeof(frame) - have,
                                    pdMS_TO_TICKS(25));
            if (n > 0) {
                have += (size_t)n;
                last_rx = now_ms();
#if !CONFIG_DIRECT_BMS_7C
                if (have >= 79U) break;
#endif
            }
        }
        direct_bms_measurement_t sample;
#if CONFIG_DIRECT_BMS_7C
        bool valid = direct_bms_parse_7c(frame, have, &sample);
#else
        bool valid = direct_bms_parse_modbus(frame, have, 1, &sample);
#endif
        if (valid) {
            if(failed)ESP_LOGW(TAG,"BMS communication recovered after %lu failed queries",(unsigned long)failed);
            failed=0;
            if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
                s_bms = sample;
                s_bms_last_ms = now_ms();
                xSemaphoreGive(s_lock);
            }
        } else {
            failed++;
            if(failed==1||now_ms()-last_warning>=30000U){
                ESP_LOGW(TAG,"BMS no valid reply: bytes=%u failures=%lu",(unsigned)have,(unsigned long)failed);last_warning=now_ms();
            }
        }
        if(!atomic_load(&s_quiet))bms_identity_step();
        /* Start-to-start period includes reception time. After a long pause,
         * discard the old deadline instead of sending catch-up queries. */
        if (xTaskGetTickCount() - last_wake >= period)
            last_wake = xTaskGetTickCount();
        vTaskDelayUntil(&last_wake, period);
    }
}

bool onlyread_sn_to_str(const OnlyReadDataDef *o, char *out, size_t len)
{
    if (!o || !out || len < 17) return false;
    memcpy(out, &o->Inverter_SN1, 16);
    out[16] = 0;
    return out[0] != 0;
}

bool arm_poll_sn_changed(void) { return false; }

void arm_poll_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        ESP_LOGE(TAG, "snapshot mutex allocation failed");
        return;
    }
    memset(&g_run_param, 0, sizeof(g_run_param));
    memset(&g_only_read, 0, sizeof(g_only_read));
    memset(&g_arm_stat, 0, sizeof(g_arm_stat));
    bool defaults_used=false;
    s_dsp_params_valid=direct_dsp_params_bootstrap(g_device_sn,&s_dsp_params,&defaults_used);
    s_dsp_params_revision=s_dsp_params_saved_revision=s_dsp_params_valid?s_dsp_params.revision:0U;
    g_read_write.CtrlParamAlterTime=s_dsp_params_revision;
    /* DSP ArmComm.c may overwrite 03/04 TX with a short sync request while
     * parameters are missing. Restore all three before ordinary polling. */
    s_dsp_sync_pending=s_dsp_params_valid?7U:0U;
    if(s_dsp_params_valid){s_config_revision=1;g_arm_stat.last_ctrl_ms=now_ms();}
    ESP_LOGW(TAG,"DSP persistent params=%s",s_dsp_params_valid?"READY":"INVALID_OR_SAVE_FAILED; import original ARM 00/01/02");
    if(defaults_used)ESP_LOGW(TAG,"DSP ARM defaults saved: output=230V/50Hz battery_type=1 utility_max=60A total_max=140A generator=OFF");
    /* Device identity now lives on the ESP, not on an ARM read-only record. */
    memcpy(&g_only_read.Inverter_SN1, g_device_sn,
           strnlen(g_device_sn, 16));
    /* Hardware model/version fields are not known from the DSP frames yet.
     * Leave info unavailable instead of publishing a fabricated ARM version. */
    if (uart_setup(DSP_PORT, CONFIG_DIRECT_DSP_TX_PIN,
                   CONFIG_DIRECT_DSP_RX_PIN, DSP_BAUD, -1)) {
        ESP_LOGW(TAG,"DSP UART%d TX=%d RX=%d 115200 8N1, poll=110ms, direct 03/04/25 protocol",
                 DSP_PORT,CONFIG_DIRECT_DSP_TX_PIN,CONFIG_DIRECT_DSP_RX_PIN);
        atomic_store(&s_dsp_started,xTaskCreate(dsp_task, "direct_dsp", 4096, NULL, 6, &s_dsp_owner)==pdPASS);
        if(!atomic_load(&s_dsp_started))ESP_LOGE(TAG,"DSP task creation failed");
    } else ESP_LOGW(TAG, "DSP disabled: configure board TX/RX pins");
    if (uart_setup(BMS_PORT, CONFIG_DIRECT_BMS_TX_PIN,
                   CONFIG_DIRECT_BMS_RX_PIN, BMS_BAUD,
                   CONFIG_DIRECT_BMS_RS485_DE_PIN)) {
        ESP_LOGI(TAG, "BMS RS485 UART%d TX=%d RX=%d 9600 8N1, protocol=%s, direction=%s",
                 BMS_PORT, CONFIG_DIRECT_BMS_TX_PIN, CONFIG_DIRECT_BMS_RX_PIN,
#if CONFIG_DIRECT_BMS_7C
                 "BBJ 0x7C",
#else
                 "Modbus 0x03",
#endif
                 CONFIG_DIRECT_BMS_RS485_DE_PIN >= 0 ? "RTS/DE GPIO" : "transceiver automatic");
        atomic_store(&s_bms_started,xTaskCreate(bms_task, "direct_bms", 4096, NULL, 5, &s_bms_owner)==pdPASS);
    } else ESP_LOGW(TAG, "BMS disabled: configure board TX/RX pins");
}

void arm_poll_tick(void)
{
    if (!s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(10)) != pdTRUE) return;
    if (g_arm_stat.last_rx_ms && now_ms() - g_arm_stat.last_rx_ms > LINK_STALE_MS)
        g_arm_stat.connected = false;
    xSemaphoreGive(s_lock);
    dsp_dispatch_notifications();
    dsp_params_persist_tick();
    dsp_report_tick();
}
void arm_poll_set_quiet(bool quiet) { atomic_store(&s_quiet, quiet); }
void arm_poll_request_info(void) { }
void arm_poll_request_config(void)
{
    /* No request-25 opcode exists in App(10). DSP sends synchronization
     * asynchronously; publish the currently confirmed snapshot only. */
    uint16_t snapshot[CTRL_PARAM_COUNT]; uint32_t revision;
    bool complete=true;
    if (!s_lock || xSemaphoreTake(s_lock,pdMS_TO_TICKS(50)) != pdTRUE) return;
    memcpy(snapshot,g_ctrl_param,sizeof(snapshot)); revision=s_config_revision;
    if(s_dsp_params_valid){
        complete=s_dsp_params_revision==s_dsp_params_saved_revision;revision=s_dsp_params_revision;
        for(unsigned i=0;i<CTRL_PARAM_COUNT;i++){uint16_t value;if(direct_dsp_params_get(&s_dsp_params,(uint16_t)i,&value))snapshot[i]=collector_value((uint16_t)i,value);}
    }else{
    for (unsigned i=0;i<CTRL_PARAM_COUNT;++i) if (!s_config_valid[i]) complete=false;
    }
    xSemaphoreGive(s_lock);
    if (revision && complete && s_ctrl_cb) s_ctrl_cb(snapshot,CTRL_PARAM_COUNT,revision);
}
int arm_poll_send_write(uint8_t cmd, uint16_t param1, uint16_t param2,
                        const uint8_t *data)
{
    if (cmd!=CS_CMD_WRITE_CTRLPARAM || param2!=2 || !data) return -1;
    /* Keep cloud ownership metadata in the same locked enqueue operation. */
    if (!s_lock || xSemaphoreTake(s_lock,pdMS_TO_TICKS(20)) != pdTRUE) return -1;
    int result=-1;
    {
        uint16_t value=(uint16_t)(data[0]|((uint16_t)data[1]<<8));
        if(collector_to_dsp(param1,&value))result=queue_user_write(param1,value,true);
    }
    xSemaphoreGive(s_lock); return result;
}
bool arm_poll_write_pending(void)
{
    if (!arm_poll_take_param()) return true;
    bool pending=s_write.pending; arm_poll_give_param(); return pending;
}
void arm_poll_mark_utc_done(void) { s_last_utc_ms = now_ms(); }
uint32_t arm_poll_last_utc_ms(void) { return s_last_utc_ms; }
bool arm_poll_take_param(void)
{
    return s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) == pdTRUE;
}
void arm_poll_give_param(void) { if (s_lock) xSemaphoreGive(s_lock); }
void arm_poll_set_write_cb(arm_write_result_cb_t cb)
{
    if (cb && !s_write_cb[0]) s_write_cb[0] = cb;
    else if (cb) s_write_cb[1] = cb;
}
void arm_poll_set_ctrl_snapshot_cb(arm_ctrl_snapshot_cb_t cb) { s_ctrl_cb = cb; }

bool direct_config_get(uint16_t index,uint16_t *value)
{
    if (!value || index>=DIRECT_DSP_CONFIG_COUNT || !arm_poll_take_param()) return false;
    bool known=s_dsp_params_valid && direct_dsp_params_get(&s_dsp_params,index,value);
    if(!known){known=s_config_valid[index];if(known)*value=s_config[index];}
    arm_poll_give_param(); return known;
}
bool direct_config_get_wire(uint16_t index,uint16_t *value){
    uint16_t native;if(!value || !direct_config_get(index,&native))return false;
    *value=collector_value(index,native);return true;
}
bool direct_config_snapshot_wire(uint16_t values[CTRL_PARAM_COUNT],
                                 bool valid[CTRL_PARAM_COUNT],uint32_t *revision)
{
    if(!values || !valid || !revision || !arm_poll_take_param())return false;
    for(unsigned i=0;i<CTRL_PARAM_COUNT;i++){
        uint16_t native=0;
        bool known=s_dsp_params_valid && direct_dsp_params_get(&s_dsp_params,(uint16_t)i,&native);
        if(!known){known=s_config_valid[i];if(known)native=s_config[i];}
        valid[i]=known;values[i]=known?collector_value((uint16_t)i,native):0;
    }
    *revision=g_read_write.CtrlParamAlterTime;
    arm_poll_give_param();return true;
}
void direct_config_snapshot(uint16_t values[DIRECT_DSP_CONFIG_COUNT],
                            bool valid[DIRECT_DSP_CONFIG_COUNT])
{
    if (!values || !valid) return;
    memset(values,0,sizeof(s_config)); memset(valid,0,sizeof(s_config_valid));
    if (!arm_poll_take_param()) return;
    memcpy(values,s_config,sizeof(s_config)); memcpy(valid,s_config_valid,sizeof(s_config_valid));
    if(s_dsp_params_valid)for(unsigned i=0;i<DIRECT_DSP_CONFIG_COUNT;i++)if(direct_dsp_params_get(&s_dsp_params,(uint16_t)i,&values[i]))valid[i]=true;
    arm_poll_give_param();
}
bool direct_config_writable(uint16_t index)
{
    if (index>=DIRECT_DSP_CONFIG_COUNT || !arm_poll_take_param()) return false;
    uint16_t value;direct_dsp_params_t candidate=s_dsp_params;
    bool writable=s_dsp_params_valid && !s_write.pending && !s_dsp_params_importing && !atomic_load(&s_quiet) && !atomic_load(&s_iap_busy) && !atomic_load(&s_dsp_boot) &&
        s_dsp_params_revision==s_dsp_params_saved_revision && direct_dsp_params_get(&candidate,index,&value) && direct_dsp_params_set(&candidate,index,value);
    arm_poll_give_param(); return writable;
}
int direct_config_write(uint16_t index,uint16_t value)
{
    if (!s_lock || xSemaphoreTake(s_lock,pdMS_TO_TICKS(20)) != pdTRUE) return -1;
    int result=queue_user_write(index,value,false);
    xSemaphoreGive(s_lock); return result;
}
int direct_config_last_write_result(void)
{
    return atomic_load(&s_ui_write_result);
}
int direct_config_reset_local(void){
    if(!s_lock || xSemaphoreTake(s_lock,pdMS_TO_TICKS(20))!=pdTRUE)return -1;
    int result=queue_user_write(41,1,false);
    if(result==0){
        direct_dsp_params_set(&s_write.candidate,14,1);
        direct_dsp_params_set(&s_write.candidate,15,0);
    }
    xSemaphoreGive(s_lock);return result;
}
bool direct_dsp_get_status(direct_dsp_status_t *status)
{
    if (!status || !arm_poll_take_param()) return false;
    bool valid=s_status_valid && now_ms()-s_status_last_ms<5000U;
    if (valid) *status=s_status;
    arm_poll_give_param(); return valid;
}
bool direct_dsp_get_status_sample(direct_dsp_status_t *status,uint32_t *received_ms)
{
    if(!status || !received_ms || !arm_poll_take_param())return false;
    bool valid=s_status_valid && now_ms()-s_status_last_ms<5000U;
    if(valid){*status=s_status;*received_ms=s_status_last_ms;}
    arm_poll_give_param();return valid;
}
bool direct_bms_get_snapshot(direct_bms_measurement_t *measurement,uint32_t *age_ms)
{
    if (!measurement || !age_ms || !arm_poll_take_param()) return false;
    bool valid=s_bms_last_ms!=0;
    if (valid) { *measurement=s_bms; *age_ms=now_ms()-s_bms_last_ms; }
    else { memset(measurement,0,sizeof(*measurement)); *age_ms=UINT32_MAX; }
    arm_poll_give_param(); return valid;
}
bool direct_dsp_get_measurement(RunParamDef *measurement,uint32_t *received_ms)
{
    if (!measurement || !received_ms || !arm_poll_take_param()) return false;
    bool valid=s_measurement_last_ms && now_ms()-s_measurement_last_ms<=2000U;
    if (valid) { *measurement=g_run_param; *received_ms=s_measurement_last_ms; }
    arm_poll_give_param(); return valid;
}

bool direct_dsp_get_diagnostics(direct_dsp_diagnostics_t *diagnostics)
{
    if(!diagnostics || !arm_poll_take_param())return false;
    *diagnostics=s_dsp_diagnostics;
    diagnostics->task_running=atomic_load(&s_dsp_started);
    diagnostics->quiet=atomic_load(&s_quiet) || atomic_load(&s_iap_busy);
    diagnostics->poll_hold_reason=(atomic_load(&s_quiet)?1U:0U) |
        (atomic_load(&s_iap_busy)?2U:0U) | (atomic_load(&s_dsp_boot)?4U:0U) |
        (!s_dsp_params_valid?8U:0U) | (s_dsp_params_revision!=s_dsp_params_saved_revision?16U:0U) |
        (s_dsp_params_importing?32U:0U) | (s_write.pending&&!s_write.persisted?64U:0U);
    arm_poll_give_param();return true;
}

bool direct_link_software_healthy(void){
    const uint32_t now=now_ms();
    return atomic_load(&s_dsp_started) && atomic_load(&s_bms_started) &&
           now-atomic_load(&s_dsp_tick)<=2000U && now-atomic_load(&s_bms_tick)<=2000U;
}

bool direct_bms_identity_request(void){
    if(!atomic_load(&s_bms_started)||atomic_load(&s_quiet)||!arm_poll_take_param())return false;
    bool accepted=!s_bms_identity_pending;
    if(accepted){memset(&s_bms_identity,0,sizeof(s_bms_identity));s_bms_identity_next=0;
        s_bms_identity_last_ms=0;s_bms_identity_generation++;s_bms_identity_pending=true;}
    arm_poll_give_param();return accepted;
}
bool direct_bms_identity_snapshot(direct_bms_identity_t *identity,bool *pending,uint8_t *queried,uint32_t *age_ms){
    if(!identity||!pending||!queried||!age_ms||!arm_poll_take_param())return false;
    *identity=s_bms_identity;*pending=s_bms_identity_pending;*queried=s_bms_identity_next;
    *age_ms=s_bms_identity_last_ms?now_ms()-s_bms_identity_last_ms:UINT32_MAX;
    arm_poll_give_param();return true;
}
