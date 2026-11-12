"""Run the actual independent BMS task during absence and reconnection."""
from pathlib import Path
import subprocess
import sys
root=Path(__file__).resolve().parents[1]
source=(root/'full/direct_poll.c').read_text(encoding='utf-8')
task=source[source.index('static uint32_t bms_query_interval('):source.index('bool onlyread_sn_to_str(')]
health=source[source.index('bool direct_link_software_healthy('):source.index('bool direct_bms_identity_request(')]
harness=r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include "../full/panel_bms_protocol.h"
#define BMS_PORT 1
#define BMS_POLL_INTERVAL_MS 500U
#define CONFIG_DIRECT_BMS_7C 1
#define CONFIG_DIRECT_BMS_TX_PIN (TEST_BMS_DISABLED?-1:21)
#define CONFIG_DIRECT_BMS_RX_PIN (TEST_BMS_DISABLED?-1:20)
#define DIRECT_BMS_SUMMARY_SIZE 16
#define pdMS_TO_TICKS(ms) (ms)
#define pdTRUE 1
#define ESP_LOGW(...) ((void)0)
typedef struct {unsigned value;} direct_bms_measurement_t;
static direct_bms_measurement_t s_bms;
static struct {uint8_t bytes[16];uint64_t received_ms;bool acquired;} s_bms_summary;
static atomic_uint s_bms_tick;
static atomic_uint s_bms_mode;
static atomic_uint s_bms_failed_mode=UINT32_MAX;
static atomic_uint s_bms_detection;
static unsigned scenario,csergy_queries,commits;
panel_bms_protocol_t panel_bms_protocol_get(void){return atomic_load(&s_bms_mode)&1U;}
panel_bms_detection_t panel_bms_detection(void){return atomic_load(&s_bms_detection);}
static bool bms_protocol_save_locked(panel_bms_protocol_t protocol){
 if(panel_bms_protocol_get()==protocol)return true;
 ++commits;
 if(scenario==4)return false;
 atomic_fetch_add(&s_bms_mode,1U);return true;
}
static bool bms_csergy_poll(unsigned mode,direct_bms_measurement_t *sample)
{
 assert(scenario && mode==atomic_load(&s_bms_mode));
 csergy_queries++;sample->value=456;
 return scenario==2 || scenario==4;
}
static atomic_uint s_bridge_tick;
static atomic_bool s_bridge_started,s_bms_started;
static atomic_bool s_quiet;
static unsigned s_lock,s_bms_last_ms;
static bool s_bms_identity_pending=true;
static uint32_t s_bms_identity_generation;
static uint32_t clock_ms=100,query_started,last_query;
static unsigned queries,early_queries,late_queries,identity_reads,owner_checks;
static uint32_t longest_query_gap,shortest_absent_gap=UINT32_MAX,first_recovery;
static jmp_buf done;
static uint32_t now_ms(void){return clock_ms;}
static uint64_t esp_timer_get_time(void){return (uint64_t)clock_ms*1000;}
static void vTaskDelay(unsigned ms){clock_ms+=ms;
 assert(clock_ms-atomic_load(&s_bms_tick)<400);
 if(clock_ms>=23000)longjmp(done,1);
}
static bool iap_owner_step(void){
 owner_checks++;
 if(scenario && panel_bms_detection()!=PANEL_BMS_DETECT_RUNNING)longjmp(done,1);
 return false;
}
static bool panel_bus_software_healthy(void){return true;}
static size_t direct_bms_make_7c_query(uint8_t *request){request[0]=0x7c;return 1;}
static void uart_flush_input(unsigned port){assert(port==1);}
static int uart_write_bytes(unsigned port,const void *request,size_t length){
 (void)request;assert(port==1 && length==1);queries++;
 uint32_t gap=clock_ms-last_query;if(last_query && gap>longest_query_gap)longest_query_gap=gap;
 if(last_query>=6000 && clock_ms<15000 && gap<shortest_absent_gap)shortest_absent_gap=gap;
 last_query=query_started=clock_ms;
 if(clock_ms<6000)early_queries++;if(clock_ms>=18000)late_queries++;
 return (int)length;
}
static int uart_read_bytes(unsigned port,uint8_t *out,size_t size,unsigned wait){
 (void)out;(void)size;assert(port==1);clock_ms+=wait;return 0;
}
static bool direct_bms_parse_7c(const uint8_t *bytes,size_t count,direct_bms_measurement_t *sample){
 (void)bytes;(void)count;sample->value=123;
 if(scenario)return scenario==1 || scenario==5;
 if(query_started>=15000){if(!first_recovery)first_recovery=query_started;return true;}return false;
}
static bool bms_bbj_find_response(const unsigned char *bytes,size_t count,size_t *offset,size_t *length){
 (void)bytes;*offset=0;*length=count;
 return scenario?scenario==1 || scenario==5:query_started>=15000;
}
static bool direct_bms_encode_summary_7c(const uint8_t *bytes,size_t count,uint8_t *summary){
 (void)bytes;(void)count;memset(summary,1,16);return true;
}
static bool xSemaphoreTake(unsigned lock,unsigned timeout){(void)lock;(void)timeout;return true;}
static void xSemaphoreGive(unsigned lock){(void)lock;}
static void panel_mb_hex_head(const uint8_t *b,size_t n,char *out,size_t cap){(void)b;(void)n;assert(cap);out[0]=0;}
static void bms_identity_step(void){identity_reads++;assert(scenario || query_started>=15000);}
TASK
HEALTH
int main(void){
 atomic_store(&s_bridge_started,true);atomic_store(&s_bridge_tick,clock_ms);atomic_store(&s_bms_tick,clock_ms);
 assert(direct_link_software_healthy()==(bool)TEST_BMS_DISABLED);
 atomic_store(&s_bms_started,true);assert(direct_link_software_healthy());
 atomic_store(&s_bms_tick,clock_ms-2001U);assert(direct_link_software_healthy()==(bool)TEST_BMS_DISABLED);
 assert(bms_query_interval(0)==500 && bms_query_interval(9)==500 && bms_query_interval(10)==5000);
 if(!setjmp(done))bms_task(NULL);
 assert(early_queries>=10 && queries<40 && late_queries>=7);
 assert(shortest_absent_gap>=5000 && longest_query_gap<=5050);
 assert(first_recovery>=15000 && first_recovery<=20000);
 assert(identity_reads && s_bms.value==123 && s_bms_summary.acquired);
 assert(atomic_load(&s_bms_failed_mode)==atomic_load(&s_bms_mode));
 assert(!s_bms_identity_pending && s_bms_identity_generation==1);
 assert(owner_checks>300 && clock_ms-atomic_load(&s_bms_tick)<=50);
 printf("PASS BMS recovery: absent backoff=%u ms, queries=%u, recovered=%u ms, healthy owner/IAP checks=%u, normal 500ms resumed\n",shortest_absent_gap,queries,first_recovery,owner_checks);
 for(scenario=1;scenario<=5;scenario++){
   queries=csergy_queries=commits=0;clock_ms=100;last_query=0;
   s_bms.value=999;s_bms_last_ms=15;
   atomic_store(&s_bms_mode,scenario==5?PANEL_BMS_CSERGY:PANEL_BMS_BBJ);
   atomic_store(&s_bms_detection,PANEL_BMS_DETECT_RUNNING);
   if(!setjmp(done))bms_task(NULL);
   if(scenario==1){
     assert(queries==1 && !csergy_queries && !commits && s_bms.value==123);
     assert(panel_bms_detection()==PANEL_BMS_DETECT_MATCHED);
   }else if(scenario==2){
     assert(queries==2 && csergy_queries==1 && commits==1 && s_bms.value==456);
     assert(panel_bms_protocol_get()==PANEL_BMS_CSERGY && panel_bms_detection()==PANEL_BMS_DETECT_MATCHED);
   }else if(scenario==3){
     assert(queries==2 && csergy_queries==2 && !commits && s_bms.value==999 && s_bms_last_ms==15);
     assert(panel_bms_protocol_get()==PANEL_BMS_BBJ && panel_bms_detection()==PANEL_BMS_DETECT_MISSING);
   }else if(scenario==4){
     assert(queries==2 && csergy_queries==1 && commits==1 && s_bms.value==999 && s_bms_last_ms==15);
     assert(panel_bms_protocol_get()==PANEL_BMS_BBJ && panel_bms_detection()==PANEL_BMS_DETECT_SAVE_FAILED);
   }else{
     assert(csergy_queries==2 && queries==1 && commits==1 && s_bms.value==123);
     assert(panel_bms_protocol_get()==PANEL_BMS_BBJ && panel_bms_detection()==PANEL_BMS_DETECT_MATCHED);
   }
 }
 puts("PASS BMS auto detect: current match, switch both ways, no reply and save failure retain previous selection");
}
'''
cfile=root/'tmp/bms_recovery_test.c';exe=cfile.with_suffix('.exe')
cfile.write_text(harness.replace('TASK',task).replace('HEALTH',health),encoding='utf-8')
for disabled in (0,1):
 subprocess.run([sys.argv[1],'-std=c11','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',f'-DTEST_BMS_DISABLED={disabled}',str(cfile),'-o',str(exe)],check=True)
 result=subprocess.run([str(exe)],capture_output=True,text=True)
 assert result.returncode==0,result.stdout+result.stderr
 print(f'Configured BMS disabled={disabled}: '+result.stdout,end='')
