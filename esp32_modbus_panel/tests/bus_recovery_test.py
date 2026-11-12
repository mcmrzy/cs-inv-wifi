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
typedef struct {panel_command_t command;uint32_t sequence;} queued_command_t;
static panel_snapshot_t s_live;
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
static uint32_t clock_ms=100,max_core_age,max_alarm_age;
static unsigned writes,completions,diagnostics,core_count,alarm_count;
static bool supplied,was_quiet;
static jmp_buf done;
static uint32_t uptime(void *context){(void)context;return clock_ms;}
static void sleep_ms(void *context,uint32_t ms){(void)context;clock_ms+=ms;}
static void vTaskDelay(unsigned ms){
 clock_ms+=ms;
 if(TEST_MODE==0){atomic_store(&s_quiet,clock_ms<1000);if(clock_ms>=1000)was_quiet=true;}
 if(TEST_MODE==1){atomic_store(&s_quiet,clock_ms>=500 && clock_ms<1300);if(clock_ms>=1300)was_quiet=true;}
 if(TEST_MODE==2)atomic_store(&s_diag_request,true);
 if(clock_ms>=12500)longjmp(done,1);
}
static void log_link_health(void){}
static void publish(void *context,const panel_snapshot_t *s){(void)context;(void)s;}
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
 clock_ms+=7U+(8U*10000U+9599U)/9600U;
 if(start>=0x4100){setting_reads++;clock_ms+=s_timeout;return PANEL_IO_ERROR;}
 if(start>=0x4090){diagnostics++;clock_ms+=350;return PANEL_IO_ERROR;}
 clock_ms+=((5U+2U*count)*10000U+9599U)/9600U;
 for(unsigned i=0;i<count;i++)values[i]=start+i==0x42?5:start+i==0x150?4:start+i==0x2a1?5000:0;
 return PANEL_IO_OK;
}
static panel_io_result_t write_reg(void *context,uint16_t reg,uint16_t value,uint8_t *exception){
 (void)context;(void)exception;assert(reg==0x4101 && value==1);assert(!was_quiet);writes++;return PANEL_IO_OK;
}
static void poll_block(uint16_t start,uint16_t count){
 static const uint16_t core[]={0x50,0x80,0x96,0x150,0x330,0x140,0x2a1,0x350,0x176};
 uint32_t previous=panel_stamp(&s_live,start);bool is_core=false;
 for(unsigned i=0;i<sizeof(core)/sizeof(core[0]);i++)if(start==core[i])is_core=true;
 uint16_t values[32];uint8_t ex=0;assert(count<=32);
 if(read_regs(NULL,start,count,values,&ex)==PANEL_IO_OK){
  uint32_t age=clock_ms-previous;
  if(is_core){core_count++;if(age>max_core_age)max_core_age=age;}
  if(start==0x46){alarm_count++;if(age>max_alarm_age)max_alarm_age=age;}
  for(unsigned i=0;i<count;i++)panel_store(&s_live,start+i,values[i],clock_ms);
 }
}
OWNER
int main(void){
 s_live.now_ms=clock_ms;s_live.config=(panel_config_t){1,9600,0,PANEL_PROFILE_6KU};
 panel_store(&s_live,0x42,5,clock_ms);
 const uint16_t core[]={0x50,0x80,0x96,0x150,0x330,0x140,0x2a1,0x350,0x176,0x46};
 for(unsigned i=0;i<sizeof(core)/sizeof(core[0]);i++)panel_store(&s_live,core[i],0,clock_ms);
 if(TEST_MODE==0)atomic_store(&s_quiet,true);
 if(TEST_MODE==3){
  uint16_t settings[]={0x4105,0x4106,0x4109,0x410c,0x410d};panel_bus_request_settings(settings,5);
 }
 if(!setjmp(done))bus_task(NULL);
 assert(!panel_control_active(&s_control));
 if(TEST_MODE==3){assert(writes==0 && completions==0 && setting_reads>=5);
  fprintf(stderr,"SETTING_BUDGET core=%u alarm=%u core_age=%u alarm_age=%u reads=%u\n",core_count,alarm_count,max_core_age,max_alarm_age,setting_reads);
  assert(core_count>50 && alarm_count>10 && max_core_age<2000 && max_alarm_age<1300);
 }else if(TEST_MODE==2){assert(writes==0 && completions==0 && diagnostics>=3);
  assert(core_count>50 && alarm_count>10 && max_core_age<2000 && max_alarm_age<1300);
 }else{assert(writes==(TEST_MODE==0?0U:1U) && completions==1 && was_quiet);}
 printf("PASS bus recovery: mode=%u writes=%u completions=%u diagnostics=%u core_age=%u alarm_age=%u\n",TEST_MODE,writes,completions,diagnostics,max_core_age,max_alarm_age);
}
'''
for mode in range(4):
 cfile=root/f'tmp/bus_recovery_{mode}.c';exe=cfile.with_suffix('.exe')
 cfile.write_text(harness.replace('OWNER',setting_service+owner),encoding='utf-8')
 subprocess.run([sys.argv[1],'-std=c11','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',f'-DTEST_MODE={mode}',
  '-I'+str(root/'main'),str(cfile),str(root/'main/panel_control.c'),str(root/'main/panel_settings_query.c'),str(root/'main/panel_catalog.c'),str(root/'main/panel_model.c'),'-o',str(exe)],check=True)
 result=subprocess.run([str(exe)],capture_output=True,text=True)
 assert result.returncode==0,result.stdout+result.stderr
 print(result.stdout,end='')
