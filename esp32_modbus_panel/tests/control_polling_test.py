"""Run the actual UART-owner loop with fake queues/time and inverter IO."""
from pathlib import Path
import subprocess
import sys

root=Path(__file__).resolve().parents[1]
source=(root/'main/panel_bus.c').read_text(encoding='utf-8')
owner=source[source.rindex('typedef struct {',0,source.index('} background_block_t;')):source.index('esp_err_t panel_bus_start(')]
harness=r'''
#include "panel_control.h"
#include "panel_catalog.h"
#include <assert.h>
#include <setjmp.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)
#define ESP_LOGI(...) ((void)0)
typedef struct {panel_command_t command;uint32_t sequence,context;} queued_command_t;
static panel_snapshot_t s_live;
static panel_control_t s_control;
static atomic_uint s_tick;
static atomic_bool s_quiet,s_diag_request;
static unsigned s_active_sequence,s_commands,s_timeout;
static panel_command_type_t s_active_type;
static uint8_t s_target;
static bool s_recent_3ku_nak;
static uint32_t s_last_3ku_nak_ms;
static uint32_t clock_ms=100,max_core_gap,max_alarm_gap,write_ms;
static unsigned writes,completions,core_reads,alarm_reads,marker_reads,current_reads;
static jmp_buf done;
static uint32_t uptime(void *context){(void)context;return clock_ms;}
static void sleep_ms(void *context,uint32_t ms){(void)context;clock_ms+=ms;}
static void vTaskDelay(unsigned ms){clock_ms+=ms;if(clock_ms>=(TEST_MODE==3?9000U:6200U))longjmp(done,1);}
static void log_link_health(void){}
static void publish(void *context,const panel_snapshot_t *s){(void)context;(void)s;}
static bool service_settings_query(const panel_control_io_t *io){(void)io;return false;}
static void complete(void){
 completions++;
 panel_write_state_t expected=TEST_MODE==3?PANEL_WRITE_CONFIRMED:TEST_MODE==2?PANEL_WRITE_PROCESSED:PANEL_WRITE_UNCERTAIN;
 assert(s_live.write_state==expected);
 assert(alarm_reads>0 && core_reads>0); /* Polling already advanced during the write. */
 if(TEST_MODE==3)assert(clock_ms-write_ms>=5000U);
}
static void change_config(panel_config_t config){(void)config;assert(0);}
static int xQueueReceive(unsigned queue,queued_command_t *out,unsigned wait){
 (void)queue;(void)wait;static bool supplied;
 assert(!panel_control_active(&s_control));
 if(supplied)return 0;supplied=true;
 *out=(queued_command_t){.command={.type=PANEL_CMD_WRITE,
      .address=TEST_MODE==3?0x4123:0x4101,.value=TEST_MODE==3?2:1},.sequence=41};return pdTRUE;
}
static panel_io_result_t read_regs(void *context,uint16_t start,uint16_t count,uint16_t *values,uint8_t *exception){
 (void)context;(void)exception;
 /* 9600 8N1, 8-byte request, 5+2*n response, and the real owner's 7ms
  * quiet-gap floor. Ceiling each segment is conservative at millisecond scale. */
 clock_ms+=7U+(8U*10000U+9599U)/9600U;
 if(start==0x2a1 && count==1 && panel_control_active(&s_control)){
  current_reads++;
  if(TEST_MODE==0){*exception=2;return PANEL_IO_EXCEPTION;}
  if(TEST_MODE==1){clock_ms+=350U;return PANEL_IO_ERROR;}
 }
 if(start==0x4101 || start==0x4123){
  marker_reads++;
  if(TEST_MODE==1){clock_ms+=350U;return PANEL_IO_ERROR;}
 }
 clock_ms+=((5U+2U*count)*10000U+9599U)/9600U;
 for(unsigned i=0;i<count;i++)values[i]=start+i==0x42?5:start+i==0x150?4:start+i==0x2a1?5000:0;
 if(TEST_MODE==3 && start==0x50)values[0x58-0x50]=!writes || clock_ms-write_ms<4000U?2310:0;
 if(start==0x4101 && TEST_MODE==2)values[0]=0xffff;
 /* The inverter ACKs but the completion marker may still be pending. */
 return PANEL_IO_OK;
}
static panel_io_result_t write_reg(void *context,uint16_t reg,uint16_t value,uint8_t *exception){
 (void)context;(void)exception;
 assert(TEST_MODE==3?(reg==0x4123 && value==2):(reg==0x4101 && value==1));
 writes++;write_ms=clock_ms;
 if(TEST_MODE==3){clock_ms+=350U;return PANEL_IO_ERROR;}
 clock_ms+=7U+(11U*10000U+9599U)/9600U+(8U*10000U+9599U)/9600U;
 return PANEL_IO_OK;
}
static panel_io_result_t poll_block(uint16_t start,uint16_t count){
 static const uint16_t core[]={0x50,0x80,0x96,0x150,0x330};
 uint32_t previous=panel_stamp(&s_live,start);
 bool core_block=false;
 for(unsigned i=0;i<sizeof(core)/sizeof(core[0]);i++)if(start==core[i])core_block=true;
 if(start==0x50)core_reads++;
 if(start==0x46)alarm_reads++;
 uint16_t values[32];uint8_t exception=0;assert(count<=32);
 panel_io_result_t result=read_regs(NULL,start,count,values,&exception);
 if(result==PANEL_IO_OK){
  /* Peak age occurs immediately before replacing the old sample, including
   * wire time for the new response, rather than only at request submission. */
  uint32_t gap=clock_ms-previous;
  if(core_block && gap>max_core_gap)max_core_gap=gap;
  if(start==0x46 && gap>max_alarm_gap)max_alarm_gap=gap;
  for(unsigned i=0;i<count;i++)panel_store(&s_live,(uint16_t)(start+i),values[i],clock_ms);
 }
 return result;
}
OWNER
int main(void){
 s_live.now_ms=clock_ms;s_live.config=(panel_config_t){1,9600,0,PANEL_PROFILE_6KU};
 const uint16_t warm[]={0x50,0x80,0x96,0x150,0x330};
 for(unsigned i=0;i<sizeof(warm)/sizeof(warm[0]);i++)panel_store(&s_live,warm[i],0,clock_ms);
 panel_store(&s_live,0x42,5,clock_ms);panel_store(&s_live,0x50,2300,clock_ms);panel_store(&s_live,0x46,0,clock_ms);
 if(TEST_MODE==3)panel_store(&s_live,0x58,2310,clock_ms);
 if(!setjmp(done))bus_task(NULL);
 assert(writes==1 && completions==1 && !panel_control_active(&s_control));
 assert(core_reads>=3 && alarm_reads>=5);
 assert(current_reads==(TEST_MODE==3?0:1) && marker_reads==(TEST_MODE<2?1:0));
 assert(max_core_gap<2000 && max_alarm_gap<2000);
 printf("PASS actual UART owner: mode=%s at 9600 8N1, core_gap=%u ms alarm_gap=%u ms, marker_reads=%u, one write/terminal receipt\n",TEST_MODE==3?"power off by voltage after missing ACK":TEST_MODE==1?"marker timeout 350ms":TEST_MODE==2?"actual mismatch":"marker pending",max_core_gap,max_alarm_gap,marker_reads);
}
'''
gcc=sys.argv[1]
base=[gcc,'-std=c11','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',
 '-I'+str(root/'main'),str(root/'main/panel_control.c'),str(root/'main/panel_catalog.c'),str(root/'main/panel_model.c')]

# Verify that the old blocking owner arrangement fails the same freshness
# assertions, without changing the project's actual source files.
old=owner.replace('!panel_control_begin(&s_control,&s_live,command.address,command.value,&io)',
 '(panel_control_write(&s_control,&s_live,command.address,command.value,&io),true)')
for name,body,mode,expected in [('blocking',old,0,False),('incremental',owner,0,True),('timeout',owner,1,True),
                                ('mismatch',owner,2,True),('power_output',owner,3,True)]:
 path=root/f'tmp/control_owner_{name}.c';exe=path.with_suffix('.exe')
 path.write_text(harness.replace('OWNER',body),encoding='utf-8')
 subprocess.run(base+[f'-DTEST_MODE={mode}',str(path),'-o',str(exe)],check=True)
 result=subprocess.run([str(exe)],capture_output=True,text=True)
 if expected:
  assert result.returncode==0,result.stdout+result.stderr
  print(result.stdout,end='')
 else:
  assert result.returncode!=0,'Blocking owner unexpectedly preserves freshness'
  print('PASS regression RED: old blocking owner pauses live polling through completion')
