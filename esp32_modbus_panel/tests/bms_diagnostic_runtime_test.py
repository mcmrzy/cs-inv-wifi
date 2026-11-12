"""Run the real diagnostic BMS UART owner with complete/partial/echo traffic."""
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
source = (root/'full/direct_poll.c').read_text(encoding='utf-8')
task = source[source.index('static uint32_t bms_query_interval('):source.index('bool onlyread_sn_to_str(')]
test = r'''
#include "direct_protocol.h"
#include "bms_bbj_frame.h"
#include "panel_bms_protocol.h"
#include <assert.h>
#include <stdatomic.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#define PANEL_BMS_DIAGNOSTIC 1
#define CONFIG_DIRECT_BMS_7C 1
#define CONFIG_DIRECT_BMS_TX_PIN 21
#define CONFIG_DIRECT_BMS_RX_PIN 20
#define CONFIG_DIRECT_BMS_RS485_DE_PIN -1
#define BMS_PORT 1
#define BMS_POLL_INTERVAL_MS 500U
#define ESP_OK 0
#define pdTRUE 1
#define pdMS_TO_TICKS(v) (v)
typedef int esp_err_t;
typedef void *QueueHandle_t;
typedef struct {int type;} uart_event_t;
enum {UART_FRAME_ERR=1,UART_PARITY_ERR,UART_FIFO_OVF,UART_BUFFER_FULL};
static const char *TAG="test";
static char log_text[32768];static size_t log_used;
static unsigned clock_ms=100,queries,mode,rx_used,rx_length,pending_event,identity_reads,query_start;
static unsigned flushes,measured_query,durations[12];static bool tx_active;
static uint8_t receive[256],good[110];
static atomic_uint s_bms_tick;
#if !PANEL_BMS_DIAGNOSTIC
static atomic_uint s_bms_mode;
static atomic_uint s_bms_failed_mode;
static atomic_uint s_bms_detection;
panel_bms_detection_t panel_bms_detection(void){return atomic_load(&s_bms_detection);}
static bool bms_protocol_save_locked(panel_bms_protocol_t protocol)
{(void)protocol;assert(false);return false;}
static bool bms_csergy_poll(unsigned mode,direct_bms_measurement_t *sample)
{(void)mode;(void)sample;assert(false);return false;}
#endif
panel_bms_protocol_t panel_bms_protocol_get(void){return PANEL_BMS_BBJ;}
static atomic_bool s_bms_started,s_quiet,s_iap_busy;
static QueueHandle_t s_bms_uart_events=(void *)1;
static unsigned s_lock=1,s_bms_last_ms;
static direct_bms_measurement_t s_bms;
static struct {uint8_t bytes[100];uint64_t received_ms;bool acquired;} s_bms_summary;
static bool s_bms_identity_pending;
static uint32_t s_bms_identity_generation;
static jmp_buf done;
static bool owner_active;
static void run_main_loop_diagnostics(void);
static void log_line(const char *tag,const char *fmt,...){
 assert(!owner_active); /* The capture task must never enter printf/newlib. */
 assert(tag && log_used+1024<sizeof(log_text));
 va_list args;va_start(args,fmt);int n=vsnprintf(log_text+log_used,sizeof(log_text)-log_used,fmt,args);va_end(args);
 assert(n>=0);log_used+=(size_t)n;log_text[log_used++]='\n';log_text[log_used]=0;
}
#define ESP_LOGI log_line
#define ESP_LOGW log_line
static uint32_t now_ms(void){return clock_ms;}
static unsigned uxTaskGetStackHighWaterMark(void *task){assert(!task);return 2048;}
static uint64_t esp_timer_get_time(void){return (uint64_t)clock_ms*1000;}
static void vTaskDelay(unsigned n){
 if(queries && measured_query!=queries){durations[mode]=clock_ms-query_start;measured_query=queries;}
 tx_active=false;
 clock_ms+=n;owner_active=false;run_main_loop_diagnostics();owner_active=true;
 if(queries>=12)longjmp(done,1);
}
static bool iap_owner_step(void){return false;}
static bool xSemaphoreTake(unsigned lock,unsigned timeout){(void)lock;(void)timeout;return true;}
static void xSemaphoreGive(unsigned lock){(void)lock;}
static int xQueueReceive(QueueHandle_t queue,uart_event_t *event,unsigned wait){
 assert(queue==(void *)1 && !wait);if(!pending_event)return 0;
 event->type=(int)pending_event;pending_event=0;return pdTRUE;
}
static int gpio_get_level(int pin){assert(pin==20);return 1;}
static void uart_get_buffered_data_len(unsigned port,size_t *count){assert(port==1);*count=queries==1?3:0;}
static void uart_flush_input(unsigned port){assert(port==1 && !tx_active);rx_used=0;flushes++;}
static int uart_write_bytes(unsigned port,const void *bytes,size_t count){
 static const uint8_t expected[]={0x7c,0x78,0x24,0x80,1,0,0};
 assert(port==1 && count==7 && !memcmp(bytes,expected,7));mode=queries++;query_start=clock_ms;tx_active=true;
 memcpy(receive,good,110);rx_length=110;
 switch(mode){
 case 0:rx_length=0;break;
 case 1:memcpy(receive,bytes,7);rx_length=7;pending_event=UART_PARITY_ERR;break;
 case 2:receive[109]^=1;pending_event=UART_FRAME_ERR;break;
 case 3:rx_length=30;pending_event=UART_FIFO_OVF;break;
 case 4:memcpy(receive,bytes,7);memcpy(receive+7,good,110);rx_length=117;break;
 case 5:break;
 case 6:rx_length=0;return -1;
 case 7:memcpy(receive,bytes,7);memcpy(receive+7,good,110);rx_length=117;break;
 case 8:memcpy(receive+3,good,110);receive[0]=0xff;receive[1]=0;receive[2]=0x7b;receive[113]=0xff;receive[114]=0xff;rx_length=115;break;
 case 9:receive[109]^=1;memcpy(receive+110,good,110);rx_length=220;break;
 case 10:receive[3]=0x81;{unsigned crc=direct_crc16(receive+3,107);receive[1]=(uint8_t)crc;receive[2]=(uint8_t)(crc>>8);}break;
 case 11:receive[4]=8;{unsigned crc=direct_crc16(receive+3,107);receive[1]=(uint8_t)crc;receive[2]=(uint8_t)(crc>>8);}break;
 default:assert(false);
 }
 return (int)count;
}
static int uart_wait_tx_done(unsigned port,unsigned timeout){assert(port==1 && timeout==50);clock_ms+=10;return mode==6?-1:0;}
static int uart_read_bytes(unsigned port,uint8_t *out,size_t count,unsigned wait){
 assert(port==1);
 if(!wait){assert(count>=3);memset(out,0xaa,3);return 3;}
 clock_ms+=wait;
 unsigned n=rx_length-rx_used;if(n>22)n=22;if(n>count)n=(unsigned)count;
 /* Real symptom: immediate seven-byte echo, then BMS starts at 150ms.
  * A 30ms silence delimiter would incorrectly return only the query. */
 if(mode==4){if(!rx_used)n=7;else if(clock_ms-query_start<150)return 0;}
 if(mode==7)n=rx_length-rx_used;
 memcpy(out,receive+rx_used,n);rx_used+=n;return (int)n;
}
static void bms_identity_step(void){identity_reads++;}
#include "bms_diagnostics_runtime.inc"
static void run_main_loop_diagnostics(void){bms_diag_tick();}
TASK
static void put16(uint8_t *p,unsigned n){p[0]=(uint8_t)n;p[1]=(uint8_t)(n>>8);}
int main(void){
 good[0]=0x7c;good[3]=0x80;good[4]=1;good[5]=103;good[7]=16;good[40]=4;
 for(unsigned i=0;i<16;i++)put16(good+8+2*i,3300+i);
 put16(good+51,520);put16(good+53,805);put16(good+55,990);
 put16(good+1,direct_crc16(good+3,107));
 bms_diag_tick();assert(strstr(log_text,"state=NOT_STARTED"));
 atomic_store(&s_bms_started,true);
 owner_active=true;if(!setjmp(done))bms_task(NULL);owner_active=false;
 assert(queries==12 && flushes==12 && atomic_load(&s_bms_diag_queries)==12);
 assert(atomic_load(&s_bms_diag_good)==5 && atomic_load(&s_bms_diag_bytes)==1046);
 assert(atomic_load(&s_bms_diag_frame_errors)==1 && atomic_load(&s_bms_diag_parity_errors)==1);
 assert(atomic_load(&s_bms_diag_overflows)==1 && atomic_load(&s_bms_diag_tx_errors)==1);
 assert(atomic_load(&s_bms_diag_stack_free)==2048);
 assert(s_bms.voltage_10mv==5200 && s_bms_summary.acquired && identity_reads==5);
 const char *wanted[]={"reason=NO_RX","reason=ECHO_ONLY","reason=BAD_CRC","reason=LENGTH_MISMATCH",
  "reason=FRAME_IN_BUFFER","reason=VALID","candidate_offset=7 candidate_length=110",
  "candidate_offset=7 candidate_length=110 parser_ok=1 summary_ok=1",
  "candidate_offset=3 candidate_length=110 parser_ok=1 summary_ok=1",
  "candidate_offset=110 candidate_length=110 parser_ok=1 summary_ok=1",
  "reason=BAD_ADDRESS","reason=BAD_FUNCTION",
  "BMS_DIAG_ACCEPT seq=5 offset=7 length=110 crc_checked=1",
  "BMS_DIAG_ACCEPT seq=8 offset=7 length=110 crc_checked=1",
  "BMS_DIAG_HEX OLD_RX","7C 78 24 80 01 00 00","tx=-1/7 tx_done=-1"};
 for(unsigned i=0;i<sizeof(wanted)/sizeof(*wanted);i++)assert(strstr(log_text,wanted[i]));
 clock_ms+=10000;atomic_store(&s_bms_tick,clock_ms);bms_diag_tick();assert(strstr(log_text,"state=RUNNING"));
 clock_ms+=10000;atomic_store(&s_quiet,true);bms_diag_tick();assert(strstr(log_text,"state=PAUSED"));
 clock_ms+=10000;atomic_store(&s_iap_busy,true);bms_diag_tick();assert(strstr(log_text,"state=IAP"));
 clock_ms+=10000;atomic_store(&s_iap_busy,false);atomic_store(&s_quiet,false);bms_diag_tick();assert(strstr(log_text,"state=STALLED"));
 /* A blocked/slow logger cannot block or format in the capture task. */
 for(unsigned wrap=0;wrap<2;wrap++){
  if(wrap){atomic_store(&s_bms_diag_read,UINT32_MAX-1);atomic_store(&s_bms_diag_write,UINT32_MAX-1);}
  unsigned dropped=atomic_load(&s_bms_diag_dropped);size_t before=log_used;
  owner_active=true;
  for(unsigned i=0;i<6;i++)bms_diag_report(100+i,NULL,0,0,0,0,1,NULL,0,0,UINT32_MAX,false,300,false,false,NULL);
  owner_active=false;
  assert(log_used==before && atomic_load(&s_bms_diag_dropped)==dropped+2);
  assert(atomic_load(&s_bms_diag_write)-atomic_load(&s_bms_diag_read)==BMS_DIAG_TRACE_SLOTS);
  bms_diag_tick();bms_diag_tick();
  assert(atomic_load(&s_bms_diag_write)==atomic_load(&s_bms_diag_read));
 }
 puts("PASS BMS diagnostic runtime: capture task never logs; bounded deferred queue, overflow and wrap; echo then delayed reply, raw bytes and UART/task states");
}
'''.replace('TASK', task)
path = root/'tmp/bms_diagnostic_runtime_test.c'
path.write_text(test,encoding='utf-8')
exe = path.with_suffix('.exe')
shared=root.parent/'esp32_direct/main/direct'
subprocess.run([sys.argv[1],'-save-temps=obj','-std=c11','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',
    '-I'+str(root/'full'),'-I'+str(shared),str(path),str(root/'full/bms_diagnostics.c'),str(root/'full/bms_bbj_frame.c'),
    str(shared/'direct_protocol.c'),'-o',str(exe)],check=True)
subprocess.run([str(exe)],check=True)

# The same actual UART owner also runs in ordinary product mode. Check that
# it accepts the response early without the diagnostic full-window policy.
normal=test.replace('#define PANEL_BMS_DIAGNOSTIC 1','#define PANEL_BMS_DIAGNOSTIC 0')
normal=normal.replace('#define ESP_LOGW log_line','#define ESP_LOGW(...) ((void)0)')
normal=normal.replace('#include "bms_diagnostics_runtime.inc"','''
static void panel_mb_hex_head(const uint8_t *bytes,size_t count,char *out,size_t cap){(void)bytes;(void)count;assert(cap);out[0]=0;}
static void bms_diag_tick(void){}
''')
normal=normal[:normal.index('int main(void){')]+r'''
int main(void){
 good[0]=0x7c;good[3]=0x80;good[4]=1;good[5]=103;good[7]=16;good[40]=4;
 for(unsigned i=0;i<16;i++)put16(good+8+2*i,3300+i);
 put16(good+51,520);put16(good+53,805);put16(good+55,990);
 put16(good+1,direct_crc16(good+3,107));
 owner_active=true;if(!setjmp(done))bms_task(NULL);owner_active=false;
 assert(queries==12 && flushes==12 && s_bms_summary.acquired && identity_reads==5);
 assert(s_bms.voltage_10mv==5200);
 assert(durations[7]<100 && durations[4]>=250 && durations[4]<=300);
 assert(durations[1]>=300); /* Echo alone never closes the response window. */
 puts("PASS normal BMS UART owner: five real samples, quick/late/fragmented/noisy replies, strict rejection, no post-TX flush and early complete-frame return");
}
'''
path=root/'tmp/bms_normal_receive_test.c';path.write_text(normal,encoding='utf-8')
exe=path.with_suffix('.exe')
subprocess.run([sys.argv[1],'-save-temps=obj','-std=c11','-Wall','-Wextra','-Werror',
    '-Wno-misleading-indentation','-Wno-unused-function','-Wno-unused-variable',
    '-I'+str(root/'full'),'-I'+str(shared),str(path),str(root/'full/bms_bbj_frame.c'),
    str(shared/'direct_protocol.c'),'-o',str(exe)],check=True)
subprocess.run([str(exe)],check=True)
