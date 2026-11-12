/* Only the dedicated diagnostic build includes this instrumentation. */
#include "bms_diagnostics.h"
static atomic_uint s_bms_diag_queries,s_bms_diag_good,s_bms_diag_bytes;
static atomic_uint s_bms_diag_last_length,s_bms_diag_last_reason,s_bms_diag_completed;
static atomic_uint s_bms_diag_frame_errors,s_bms_diag_parity_errors,s_bms_diag_overflows;
static atomic_uint s_bms_diag_tx_errors;
static atomic_uint s_bms_diag_stack_free;
enum { BMS_DIAG_TRACE_SLOTS=4 };
typedef struct {
    unsigned sequence,request_length,count,chunks,discarded,old_length;
    int written,tx_done,rx_idle;
    uint32_t first_read,elapsed,captured_ms;
    bool read_error,parsed,summary;
    bms_diag_result_t result;
    direct_bms_measurement_t sample;
    uint8_t request[8],received[256],old_received[256];
} bms_diag_trace_t;
/* One UART producer, one main-loop consumer. Never format or block the UART
 * owner on USB/newlib. Static slots also avoid large task-local snapshots. */
static bms_diag_trace_t s_bms_diag_traces[BMS_DIAG_TRACE_SLOTS],s_bms_diag_output;
static atomic_uint s_bms_diag_write,s_bms_diag_read,s_bms_diag_dropped;
static uint8_t s_bms_diag_old_received[256];
static unsigned s_bms_diag_old_length;
static void bms_diag_remember_old(const uint8_t *bytes,size_t count)
{
    s_bms_diag_old_length=count>256?256:(unsigned)count;
    if(s_bms_diag_old_length)memcpy(s_bms_diag_old_received,bytes,s_bms_diag_old_length);
}

static void bms_diag_dump(const char *direction,unsigned sequence,const uint8_t *bytes,size_t count)
{
    static const char hex[]="0123456789ABCDEF";
    for(size_t offset=0;offset<count;offset+=16){
        char line[16*3+1];size_t used=0,end=count-offset<16?count-offset:16;
        for(size_t i=0;i<end;i++){
            unsigned value=bytes[offset+i];
            if(i)line[used++]=' ';
            line[used++]=hex[value>>4];line[used++]=hex[value&15];
        }
        line[used]=0;
        ESP_LOGI(TAG,"BMS_DIAG_HEX %s seq=%u offset=%u %s",direction,sequence,(unsigned)offset,line);
    }
}
static void bms_diag_events(void)
{
    uart_event_t event;
    for(unsigned i=0;s_bms_uart_events && i<16 && xQueueReceive(s_bms_uart_events,&event,0)==pdTRUE;i++){
        if(event.type==UART_FRAME_ERR)atomic_fetch_add(&s_bms_diag_frame_errors,1);
        else if(event.type==UART_PARITY_ERR)atomic_fetch_add(&s_bms_diag_parity_errors,1);
        else if(event.type==UART_FIFO_OVF || event.type==UART_BUFFER_FULL)
            atomic_fetch_add(&s_bms_diag_overflows,1);
    }
}
static void bms_diag_report(unsigned sequence,const uint8_t *request,size_t request_length,
                            int written,int tx_done,unsigned discarded,int rx_idle,
                            const uint8_t *frame,size_t count,unsigned chunks,
                            uint32_t first_read,bool read_error,uint32_t elapsed,
                            bool parsed,bool summary,const direct_bms_measurement_t *sample)
{
    bms_diag_result_t result=bms_diag_inspect(request,request_length,frame,count);
    bms_diag_events();
    atomic_fetch_add(&s_bms_diag_bytes,(unsigned)count);
    if(parsed && summary)atomic_fetch_add(&s_bms_diag_good,1);
    if(written!=(int)request_length || tx_done!=ESP_OK)atomic_fetch_add(&s_bms_diag_tx_errors,1);
    atomic_store(&s_bms_diag_last_length,(unsigned)count);
    atomic_store(&s_bms_diag_last_reason,result.code);atomic_store(&s_bms_diag_completed,now_ms());
    unsigned write=atomic_load_explicit(&s_bms_diag_write,memory_order_relaxed);
    unsigned read=atomic_load_explicit(&s_bms_diag_read,memory_order_acquire);
    if(write-read>=BMS_DIAG_TRACE_SLOTS){
        atomic_fetch_add(&s_bms_diag_dropped,1);s_bms_diag_old_length=0;return;
    }
    bms_diag_trace_t *t=&s_bms_diag_traces[write%BMS_DIAG_TRACE_SLOTS];
    t->sequence=sequence;t->written=written;t->tx_done=tx_done;t->discarded=discarded;
    t->rx_idle=rx_idle;t->chunks=chunks;t->first_read=first_read;t->elapsed=elapsed;
    t->read_error=read_error;t->parsed=parsed;t->summary=summary;t->result=result;
    t->captured_ms=now_ms();
    t->request_length=request_length>8?8:(unsigned)request_length;
    t->count=count>256?256:(unsigned)count;t->old_length=s_bms_diag_old_length;
    if(t->request_length)memcpy(t->request,request,t->request_length);
    if(t->count)memcpy(t->received,frame,t->count);
    if(t->old_length)memcpy(t->old_received,s_bms_diag_old_received,t->old_length);
    if(parsed)t->sample=*sample;
    s_bms_diag_old_length=0;
    atomic_store_explicit(&s_bms_diag_write,write+1,memory_order_release);
}
static void bms_diag_print(const bms_diag_trace_t *t)
{
    ESP_LOGI(TAG,"BMS_DIAG_QUERY seq=%u captured_ms=%lu tx=%d/%u tx_done=%d discarded=%u rx_idle=%d bytes=%u chunks=%u first_read_ms=%lu elapsed_ms=%lu read_error=%u",
        t->sequence,(unsigned long)t->captured_ms,t->written,t->request_length,t->tx_done,t->discarded,
        t->rx_idle,t->count,t->chunks,(unsigned long)t->first_read,(unsigned long)t->elapsed,t->read_error);
    ESP_LOGI(TAG,"BMS_DIAG_RESULT seq=%u reason=%s expected=%lu wire_crc=%04X calculated_crc=%04X candidate_offset=%u candidate_length=%u parser_ok=%u summary_ok=%u",
        t->sequence,bms_diag_reason(t->result.code),(unsigned long)t->result.expected_length,t->result.wire_crc,
        t->result.calculated_crc,t->result.candidate_offset,t->result.candidate_length,t->parsed,t->summary);
    bms_diag_dump("OLD_RX",t->sequence,t->old_received,t->old_length);
    bms_diag_dump("TX",t->sequence,t->request,t->request_length);
    bms_diag_dump("RX",t->sequence,t->received,t->count);
    if(t->parsed && t->summary)
        ESP_LOGI(TAG,"BMS_DIAG_ACCEPT seq=%u offset=%u length=%u crc_checked=1",
            t->sequence,t->result.candidate_offset,t->result.candidate_length);
    if(t->parsed)ESP_LOGI(TAG,"BMS_DIAG_SAMPLE seq=%u voltage_10mv=%u current_10ma=%ld soc_raw=%u soh_raw=%u percent_divisor=%u",
        t->sequence,(unsigned)t->sample.voltage_10mv,(long)t->sample.current_10ma,t->sample.soc_percent,t->sample.soh_percent,t->sample.percent_divisor);
}
static void bms_diag_tick(void)
{
    for(unsigned i=0;i<2;i++){
        unsigned read=atomic_load_explicit(&s_bms_diag_read,memory_order_relaxed);
        if(read==atomic_load_explicit(&s_bms_diag_write,memory_order_acquire))break;
        s_bms_diag_output=s_bms_diag_traces[read%BMS_DIAG_TRACE_SLOTS];
        atomic_store_explicit(&s_bms_diag_read,read+1,memory_order_release);
        bms_diag_print(&s_bms_diag_output);
    }
    static uint32_t next_report;
    uint32_t now=now_ms();
    if((int32_t)(now-next_report)<0)return;
    next_report=now+10000U;
    bool started=atomic_load(&s_bms_started),quiet=atomic_load(&s_quiet),iap=atomic_load(&s_iap_busy);
    unsigned age=now-atomic_load(&s_bms_tick),queries=atomic_load(&s_bms_diag_queries);
    const char *state=!started?"NOT_STARTED":iap?"IAP":quiet?"PAUSED":age>2000U?"STALLED":"RUNNING";
    unsigned completed=atomic_load(&s_bms_diag_completed);
    ESP_LOGW(TAG,"BMS_DIAG_STATE state=%s UART1 TX=%d RX=%d baud=9600 invert=none DE=%d task_age_ms=%u queries=%u valid=%u rx_total=%u last_bytes=%u last_reason=%s result_age_ms=%lu frame_errors=%u parity_errors=%u overflows=%u tx_errors=%u stack_free=%u trace_dropped=%u logger=main_loop",
        state,CONFIG_DIRECT_BMS_TX_PIN,CONFIG_DIRECT_BMS_RX_PIN,CONFIG_DIRECT_BMS_RS485_DE_PIN,
        age,queries,atomic_load(&s_bms_diag_good),atomic_load(&s_bms_diag_bytes),
        atomic_load(&s_bms_diag_last_length),queries?bms_diag_reason(atomic_load(&s_bms_diag_last_reason)):"NO_QUERY",
        (unsigned long)(completed?now-completed:UINT32_MAX),atomic_load(&s_bms_diag_frame_errors),
        atomic_load(&s_bms_diag_parity_errors),atomic_load(&s_bms_diag_overflows),atomic_load(&s_bms_diag_tx_errors),atomic_load(&s_bms_diag_stack_free),atomic_load(&s_bms_diag_dropped));
}
