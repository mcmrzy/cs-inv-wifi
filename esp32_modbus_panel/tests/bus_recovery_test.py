"""Exercise actual bus-owner recovery with faults and quiet transitions."""
from pathlib import Path
import subprocess
import sys
root=Path(__file__).resolve().parents[1]
source=(root/'main/panel_bus.c').read_text(encoding='utf-8')
owner=source[source.index('static void bus_task('):source.index('esp_err_t panel_bus_start(')]
setting_service=source[source.index('static uint16_t s_ble_setting;'):source.index('static void bus_task(')]
harness=r'''
#include "panel_control.h"
#include "panel_catalog.h"
#include "panel_settings_query.h"
#include <assert.h>
#include <setjmp.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)
#define ESP_LOGI(...) ((void)0)
#undef ESP_LOGI
static const char *TAG="test";
static void test_log(const char *fmt,...){(void)fmt;}
#define ESP_LOGI(tag,...) do{(void)(tag);test_log(__VA_ARGS__);}while(0)
#define portMAX_DELAY 0
typedef struct {panel_command_t command;uint32_t sequence,context;} queued_command_t;
static panel_snapshot_t s_live,s_shared;
static panel_control_t s_control;
static panel_settings_query_t s_setting_query;
static uint16_t s_setting_requested[PANEL_SETTING_READ_SLOTS];
static unsigned s_setting_count,s_setting_version,s_setting_applied,s_lock=1,setting_reads;
static void xSemaphoreTake(unsigned lock,unsigned timeout){(void)lock;(void)timeout;}
static void xSemaphoreGive(unsigned lock){(void)lock;}
static atomic_uint s_tick;
static atomic_bool s_quiet,s_diag_request;
static unsigned s_active_sequence,s_commands,s_timeout;
static panel_command_type_t s_active_type;
static uint8_t s_target;
static uint32_t clock_ms=100,test_start,max_core_age,max_alarm_age;
static bool s_recent_3ku_nak;
static uint32_t s_last_3ku_nak_ms;
static unsigned writes,completions,diagnostics,core_count,alarm_count;
static unsigned status_batch_naks,status_fallbacks,terminal_attempts,alarm_attempts,core_attempts[5];
static unsigned ac_full_naks,ac_power_reads,ac_grid_reads;
static unsigned background_reads,background_slots,core_groups,group_at_last_background;
static uint32_t max_terminal_age;
static uint32_t last_read_end;
static bool last_read_nak;
static unsigned optional_attempts[2],optional_recovered[2];
static uint32_t last_optional_attempt[2],min_optional_gap[2]={UINT32_MAX,UINT32_MAX};
static const uint16_t fast_regs[]={0x50,0x80,0x96,0x150,0x330};
static bool supplied,was_quiet,initial_nak;
static jmp_buf done;
static uint32_t uptime(void *context){(void)context;return clock_ms;}
static void sleep_ms(void *context,uint32_t ms){(void)context;clock_ms+=ms;}
static void vTaskDelay(unsigned ms){
 clock_ms+=ms;
 if(TEST_MODE==0){atomic_store(&s_quiet,clock_ms<1000);if(clock_ms>=1000)was_quiet=true;}
 if(TEST_MODE==1){atomic_store(&s_quiet,clock_ms>=500 && clock_ms<1300);if(clock_ms>=1300)was_quiet=true;}
 if(TEST_MODE==2)atomic_store(&s_diag_request,true);
 if(clock_ms-test_start>=12400U)longjmp(done,1);
}
static void log_link_health(void){s_live.now_ms=clock_ms;}
static void publish(void *context,const panel_snapshot_t *s){(void)context;s_shared=*s;}
static void complete(void){
 completions++;assert(s_live.write_state==(TEST_MODE==0?PANEL_WRITE_REJECTED:PANEL_WRITE_UNCERTAIN));
}
static void change_config(panel_config_t config){(void)config;assert(0);}
static int xQueueReceive(unsigned queue,queued_command_t *out,unsigned wait){
 (void)queue;(void)wait;
 if(supplied || TEST_MODE>=2)return 0;supplied=true;
 *out=(queued_command_t){.command={.type=PANEL_CMD_WRITE,.address=0x4101,.value=1},.sequence=41};return pdTRUE;
}
static panel_io_result_t read_regs(void *context,uint16_t start,uint16_t count,uint16_t *values,uint8_t *exception){
 (void)context;(void)exception;
 if(TEST_MODE>=7){
  uint32_t elapsed=clock_ms-last_read_end;
  uint32_t gap=last_read_nak?35U:7U;
  if(elapsed<gap)clock_ms+=gap-elapsed;
 }else clock_ms+=7U;
 clock_ms+=(8U*10000U+9599U)/9600U;
 if(start>=0x4100){setting_reads++;clock_ms+=s_timeout;return PANEL_IO_ERROR;}
 if(start>=0x4090){diagnostics++;clock_ms+=350;return PANEL_IO_ERROR;}
 if(TEST_MODE==9 && start==0x38 && !initial_nak){
  initial_nak=true;
  clock_ms+=40;
  last_read_end=s_last_3ku_nak_ms=clock_ms;
  last_read_nak=s_recent_3ku_nak=true;
  return PANEL_IO_NAK;
 }
 if(TEST_MODE==10 && start==0x50 && count==16){
  ac_full_naks++;
  clock_ms+=40;
  last_read_end=s_last_3ku_nak_ms=clock_ms;
  last_read_nak=s_recent_3ku_nak=true;
  return PANEL_IO_NAK;
 }
 if(TEST_MODE==10 && start==0x53 && count==12)ac_power_reads++;
 if(TEST_MODE==10 && start==0x50 && count==3)ac_grid_reads++;
 if(TEST_MODE==7 || TEST_MODE==8){
  if(TEST_MODE==8 && start==0x176 && background_reads%3U==1){
   clock_ms+=s_timeout;
   last_read_end=clock_ms;
   last_read_nak=false;
   return PANEL_IO_ERROR;
  }
  bool nak=start==0x19e || start==0x350 || start==0x2a0;
  if(start==0x41 && count==4){status_batch_naks++;nak=true;}
  if(start==0x42 && count==1)status_fallbacks++;
  if(TEST_MODE==8 && start==0x43 && terminal_attempts++%2U==0)nak=true;
  if(TEST_MODE==8 && (start==0x140 || start==0x91)){
   background_reads++;
   if(background_reads%2U==1)nak=true;
  }
  if(start==0x46 && alarm_attempts++%2U==0)nak=true;
  for(unsigned i=0;i<sizeof(fast_regs)/sizeof(fast_regs[0]);i++){
   if(start==fast_regs[i] && (core_attempts[i]++%2U)==0)nak=true;
  }
  last_read_nak=nak;
  if(nak){
   clock_ms+=40;
   last_read_end=s_last_3ku_nak_ms=clock_ms;
   s_recent_3ku_nak=true;
   return PANEL_IO_NAK;
  }
 }
 if(TEST_MODE>=4 && TEST_MODE<=6 && (start==0x176 || start==0x19e)){
  unsigned i=start==0x176?0:1;
  if(optional_attempts[i]){
   uint32_t gap=clock_ms-last_optional_attempt[i];
   if(gap<min_optional_gap[i])min_optional_gap[i]=gap;
  }
  last_optional_attempt[i]=clock_ms;optional_attempts[i]++;
  if(TEST_MODE==4 || clock_ms-test_start<4600U){clock_ms+=s_timeout;return PANEL_IO_ERROR;}
  optional_recovered[i]++;
 }
 clock_ms+=((5U+2U*count)*10000U+9599U)/9600U;
 last_read_end=clock_ms;
 for(unsigned i=0;i<count;i++){
  unsigned address=start+i;
  values[i]=address==0x42?5:address==0x150?4:address==0x2a1?5000:0;
  if(TEST_MODE==10 && address==0x5b)values[i]=276;
  if(TEST_MODE==10 && address==0x5e)values[i]=135;
 }
 return PANEL_IO_OK;
}
static panel_io_result_t write_reg(void *context,uint16_t reg,uint16_t value,uint8_t *exception){
 (void)context;(void)exception;assert(reg==0x4101 && value==1);assert(!was_quiet);writes++;return PANEL_IO_OK;
}
static panel_io_result_t poll_block(uint16_t start,uint16_t count){
 uint32_t previous=panel_stamp(&s_live,start),terminal=panel_stamp(&s_live,0x43);bool is_core=false;
 for(unsigned i=0;i<sizeof(fast_regs)/sizeof(fast_regs[0]);i++)if(start==fast_regs[i]){
  is_core=true;
  if((TEST_MODE==8 && core_attempts[i]%2U==0) || TEST_MODE==9)core_groups++;
 }
 if(TEST_MODE>=8 && !is_core && start!=0x53 && (start<0x41 || start>0x49) && core_groups){
  if(background_slots){
   unsigned measurement_gap=core_groups-group_at_last_background;
   if(TEST_MODE==8 || s_recent_3ku_nak)assert(measurement_gap>=3);
   else if(clock_ms-s_last_3ku_nak_ms>=3000U)assert(measurement_gap==1);
  }
  group_at_last_background=core_groups;
  background_slots++;
 }
 uint16_t values[32];uint8_t ex=0;assert(count<=32);
 panel_io_result_t result=read_regs(NULL,start,count,values,&ex);
 if(result==PANEL_IO_OK){
  if(start<=0x43 && start+count>0x43 && clock_ms-terminal>max_terminal_age)
   max_terminal_age=clock_ms-terminal;
  uint32_t age=clock_ms-previous;
  if(is_core){core_count++;if(age>max_core_age)max_core_age=age;}
  if(start==0x46){alarm_count++;if(age>max_alarm_age)max_alarm_age=age;}
  for(unsigned i=0;i<count;i++)panel_store(&s_live,start+i,values[i],clock_ms);
 }
 s_live.now_ms=clock_ms;
 return result;
}
OWNER
int main(void){
 if(TEST_MODE==6)clock_ms=UINT32_MAX-5500U;
 test_start=clock_ms;
 s_timeout=350;
 s_live.now_ms=clock_ms;s_live.config=(panel_config_t){1,9600,0,PANEL_PROFILE_6KU};
 if(TEST_MODE>=7)s_live.config.profile_override=PANEL_PROFILE_3KU;
 panel_store(&s_live,0x42,5,clock_ms);
 panel_store(&s_live,0x43,0,clock_ms);
 const uint16_t initial[]={0x50,0x80,0x96,0x150,0x330,0x46};
 for(unsigned i=0;i<sizeof(initial)/sizeof(initial[0]);i++)panel_store(&s_live,initial[i],0,clock_ms);
 if(TEST_MODE==0)atomic_store(&s_quiet,true);
 if(TEST_MODE>=3 && TEST_MODE<=6){
  uint16_t settings[]={0x4105,0x4106,0x4109,0x410c,0x410d};panel_bus_request_settings(settings,5);
 }
 if(!setjmp(done))bus_task(NULL);
 s_live.now_ms=clock_ms;
 assert(!panel_control_active(&s_control));
 if(TEST_MODE==10){
  uint16_t raw;
  assert(ac_full_naks>5 && ac_power_reads==ac_full_naks && ac_grid_reads==ac_full_naks);
  assert(panel_stamp(&s_live,0x53)==panel_stamp(&s_live,0x5b));
  assert(panel_stamp(&s_live,0x53)==panel_stamp(&s_live,0x5e));
  assert(panel_fresh(&s_live,0x5b) && panel_value(&s_live,0x5b,&raw) && raw==276);
  assert(panel_fresh(&s_live,0x5e) && panel_value(&s_live,0x5e,&raw) && raw==135);
  assert(max_core_age<2000 && max_alarm_age<1300);
 }else if(TEST_MODE==9){
  assert(initial_nak && !s_recent_3ku_nak && background_slots>20 && panel_online(&s_live));
  assert(panel_fresh(&s_live,0x140) && panel_fresh(&s_live,0x2a1));
 }else if(TEST_MODE>=7){
  assert(status_batch_naks>=2 && status_batch_naks<=3 && status_fallbacks>10);
  assert(panel_online(&s_live) && panel_fresh(&s_live,0x41) &&
         panel_fresh(&s_live,0x43) && panel_fresh(&s_live,0x44));
  fprintf(stderr,"3KU_RECOVERY status=%u measurements=%u alarms=%u core_age=%u alarm_age=%u terminal_age=%u background=%u\n",status_fallbacks,core_count,alarm_count,max_core_age,max_alarm_age,max_terminal_age,background_reads);
  if(TEST_MODE==8)assert(max_terminal_age<1200 && background_reads>=2 && background_slots>=5);
  assert(core_count>25 && alarm_count>10 && max_core_age<2000 && max_alarm_age<1300);
 }else if(TEST_MODE>=3){assert(writes==0 && completions==0 && setting_reads>=5);
  fprintf(stderr,"SETTING_BUDGET core=%u alarm=%u core_age=%u alarm_age=%u reads=%u\n",core_count,alarm_count,max_core_age,max_alarm_age,setting_reads);
  assert(core_count>25 && alarm_count>10 && max_core_age<2000 && max_alarm_age<1300);
  assert(panel_fresh(&s_live,0x140) && panel_fresh(&s_live,0x2a1) && panel_fresh(&s_live,0x2a0));
  if(TEST_MODE>=4){
   fprintf(stderr,"OPTIONAL_RETRY attempts=%u/%u recovered=%u/%u\n",optional_attempts[0],optional_attempts[1],optional_recovered[0],optional_recovered[1]);
   assert(optional_attempts[0]>=2 && optional_attempts[1]>=2);
   if(TEST_MODE==4)assert(min_optional_gap[0]>=2000 && min_optional_gap[1]>=2000);
   else assert(optional_recovered[0] && optional_recovered[1] && panel_fresh(&s_live,0x176) && panel_fresh(&s_live,0x19e));
  }
 }else if(TEST_MODE==2){assert(writes==0 && completions==0 && diagnostics>=3);
  assert(core_count>25 && alarm_count>10 && max_core_age<2000 && max_alarm_age<1300);
 }else{assert(writes==(TEST_MODE==0?0U:1U) && completions==1 && was_quiet);}
 printf("PASS bus recovery: mode=%u writes=%u completions=%u diagnostics=%u core_age=%u alarm_age=%u\n",TEST_MODE,writes,completions,diagnostics,max_core_age,max_alarm_age);
}
'''
for mode in range(11):
 cfile=root/f'tmp/bus_recovery_{mode}.c';exe=cfile.with_suffix('.exe')
 cfile.write_text(harness.replace('OWNER',setting_service+owner),encoding='utf-8')
 subprocess.run([sys.argv[1],'-std=c11','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',f'-DTEST_MODE={mode}',
  '-I'+str(root/'main'),str(cfile),str(root/'main/panel_control.c'),str(root/'main/panel_settings_query.c'),str(root/'main/panel_catalog.c'),str(root/'main/panel_model.c'),'-o',str(exe)],check=True)
 result=subprocess.run([str(exe)],capture_output=True,text=True)
 assert result.returncode==0,result.stdout+result.stderr
 print(result.stdout,end='')
 if mode>=4:print(result.stderr,end='')

exchange_source=source[source.index('static panel_io_result_t exchange('):source.index('static panel_io_result_t read_regs(')]
exchange_harness=r'''
#include "panel_control.h"
#include "panel_modbus.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#define UART_NUM_0 0
#define ESP_OK 0
#define pdMS_TO_TICKS(ms) (ms)
typedef struct {
    uint8_t address,function,exception,head[24],head_length;
    uint16_t start,count;
    uint32_t bytes,at_ms;
    const char *result;
    bool valid,nak;
} exchange_trace_t;
static exchange_trace_t s_rx,s_failed_rx;
static panel_snapshot_t s_live;
static atomic_uint s_tick;
static uint32_t now,s_last_end,s_timeout=350,sent_at;
static bool s_recent_3ku_nak;
static uint32_t s_last_3ku_nak_ms;
static uint8_t s_target=1;
static unsigned reads;
static bool valid_after_nak;
static uint32_t uptime(void *ctx){(void)ctx;return now;}
static void sleep_ms(void *ctx,uint32_t delay){(void)ctx;now+=delay;}
static void uart_flush_input(int uart){(void)uart;}
static int uart_write_bytes(int uart,const uint8_t *data,size_t length){
 (void)uart;(void)data;sent_at=now;return (int)length;
}
static int uart_wait_tx_done(int uart,uint32_t wait){(void)uart;(void)wait;return ESP_OK;}
static int uart_read_bytes(int uart,uint8_t *out,size_t length,uint32_t wait){
 (void)uart;(void)length;now+=wait;
 switch(reads++){
 case 0: memcpy(out,"NA",2);return 2;
 case 1: memcpy(out,"K\r",2);return 2;
 case 3: if(valid_after_nak){
   const uint8_t status[]={1,3,2,0,5,0x78,0x47};
   memcpy(out,status,sizeof(status));return sizeof(status);
 }
 }
 return 0;
}
EXCHANGE
static panel_io_result_t run(panel_profile_t profile,panel_profile_t detected,bool scan,uint8_t function,bool valid){
 now=100;s_last_end=now;reads=0;valid_after_nak=valid;
 s_live.config=(panel_config_t){1,9600,0,profile};s_live.scanning=scan;
 s_live.detected_profile=detected;
 panel_mb_reply_t reply={0};const uint8_t request[8]={0};
 panel_io_result_t result=exchange(request,sizeof(request),function,0x42,1,&reply);
 if(result==PANEL_IO_OK)assert(reply.values[0]==5);
 return result;
}
int main(void){
 assert(run(PANEL_PROFILE_3KU,PANEL_PROFILE_UNKNOWN,false,3,false)==PANEL_IO_NAK);
 assert(!strcmp(s_failed_rx.result,"NAK") && s_failed_rx.bytes==4);
 assert(sent_at-100<35 && now-sent_at<100);
 assert(run(PANEL_PROFILE_UNKNOWN,PANEL_PROFILE_3KU,false,3,false)==PANEL_IO_NAK);
 assert(sent_at-100>=35 && now-sent_at<100);
 assert(run(PANEL_PROFILE_3KU,PANEL_PROFILE_UNKNOWN,false,3,true)==PANEL_IO_OK);
 assert(run(PANEL_PROFILE_6KU,PANEL_PROFILE_3KU,false,3,false)==PANEL_IO_ERROR);
 assert(sent_at-100<35 && now-sent_at>=350);
 assert(run(PANEL_PROFILE_3KU,PANEL_PROFILE_UNKNOWN,true,3,false)==PANEL_IO_ERROR && now-sent_at>=350);
 assert(run(PANEL_PROFILE_3KU,PANEL_PROFILE_UNKNOWN,false,0x10,false)==PANEL_IO_ERROR && now-sent_at>=350);
 puts("PASS 3KU UART: split NAK, valid RTU after NAK, normal timeouts for scan, write and other models");
}
'''
cfile=root/'tmp/bus_recovery_exchange.c';exe=cfile.with_suffix('.exe')
cfile.write_text(exchange_harness.replace('EXCHANGE',exchange_source),encoding='utf-8')
subprocess.run([sys.argv[1],'-std=c11','-Wall','-Wextra','-Werror','-I'+str(root/'main'),
 str(cfile),str(root/'main/panel_modbus.c'),'-o',str(exe)],check=True)
subprocess.run([str(exe)],check=True)
