"""Execute bus startup and command admission with resource-allocation faults."""
from pathlib import Path
import subprocess
import sys
root=Path(__file__).resolve().parents[1]
source=(root/'main/panel_bus.c').read_text(encoding='utf-8')
startup=source[source.index('esp_err_t panel_bus_start('):]
admission=source[source.index('static bool submit_sequence('):source.index('static void complete(')]
config=source[source.index('static bool config_store('):source.index('static esp_err_t configure_uart(')]
test=r'''
#include "panel_types.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
typedef int esp_err_t;
typedef int nvs_handle_t;
typedef struct {panel_command_t command;uint32_t sequence;} queued_command_t;
#define CONFIG_PANEL_SLAVE_ADDRESS 1
#define CONFIG_PANEL_BAUD 9600
#define CONFIG_PANEL_FACTORY_PROFILE 4
#define ESP_OK 0
#define ESP_ERR_NO_MEM -1
#define ESP_ERR_INVALID_ARG -2
#define ESP_ERR_INVALID_SIZE -3
#define NVS_READWRITE 1
#define NVS_READONLY 0
#define UART_NUM_0 0
#define UART_PIN_NO_CHANGE -1
#define pdPASS 1
#define pdTRUE 1
#define portMAX_DELAY 999
#define ESP_LOGI(...) ((void)0)
static panel_snapshot_t s_live,s_shared;
static atomic_uint s_context_generation;
static atomic_bool s_quiet;
static bool s_running,s_reserved;
static uint8_t s_target;
static uint32_t s_sequence;
static void *s_lock,*s_commands;
static unsigned fail,driver,mutexes,queues,tasks,submitted;
static esp_err_t configure_uart(const panel_config_t *c){(void)c;return ESP_OK;}
static uint8_t stored_config[]={1,7,2,PANEL_PROFILE_4KL,0,75,0,0};
static esp_err_t nvs_open(const char *name,int mode,nvs_handle_t *h){(void)name;(void)mode;*h=1;return ESP_OK;}
static esp_err_t nvs_get_blob(nvs_handle_t h,const char *key,void *data,size_t *len){
 (void)h;(void)key;assert(*len==sizeof(stored_config));memcpy(data,stored_config,*len);return ESP_OK;
}
static esp_err_t nvs_set_blob(nvs_handle_t h,const char *key,const void *data,size_t len){
 (void)h;(void)key;assert(len==sizeof(stored_config));memcpy(stored_config,data,len);return ESP_OK;
}
static esp_err_t nvs_commit(nvs_handle_t h){(void)h;return ESP_OK;}
static void nvs_close(nvs_handle_t h){(void)h;}
static int uart_set_pin(int p,int tx,int rx,int rts,int cts){(void)p;(void)rts;(void)cts;assert(tx==1&&rx==0);return ESP_OK;}
static int uart_driver_install(int p,int rx,int tx,int n,void *q,int f){(void)p;(void)rx;(void)tx;(void)n;(void)q;(void)f;driver++;return ESP_OK;}
static void uart_driver_delete(int p){(void)p;assert(driver);driver--;}
static void *xSemaphoreCreateMutex(void){if(fail==1)return NULL;mutexes++;return (void*)1;}
static void *xQueueCreate(unsigned count,unsigned size){assert(count==1&&size==sizeof(queued_command_t));if(fail==2)return NULL;queues++;return (void*)2;}
static void vQueueDelete(void *q){assert(q&&queues);queues--;}
static void vSemaphoreDelete(void *s){assert(s&&mutexes);mutexes--;}
static void publish(void *context,const panel_snapshot_t *s){(void)context;s_shared=*s;}
static void bus_task(void *arg){(void)arg;}
static int xTaskCreate(void (*task)(void*),const char *name,unsigned stack,void *arg,int priority,void *handle){
 (void)task;(void)name;(void)stack;(void)arg;(void)priority;(void)handle;if(fail==3)return 0;tasks++;return pdPASS;
}
static void xSemaphoreTake(void *m,unsigned wait){assert(m);(void)wait;}
static void xSemaphoreGive(void *m){assert(m);}
static int xQueueSend(void *q,const queued_command_t *cmd,unsigned wait){assert(q);(void)wait;assert(cmd->command.type<=PANEL_CMD_SCAN_CANCEL);submitted++;return pdTRUE;}
STARTUP
ADMISSION
int main(void){
 panel_config_t loaded={0};assert(config_store(&loaded,false));
 assert(loaded.address==7 && loaded.baud==19200 && loaded.parity==2 && loaded.profile_override==PANEL_PROFILE_UNKNOWN);
 loaded.profile_override=PANEL_PROFILE_5KU;assert(!panel_config_valid(&loaded));
 for(fail=1;fail<=3;fail++){
  assert(panel_bus_start()==ESP_ERR_NO_MEM);
  assert(!s_running&&!s_lock&&!s_commands&&!driver&&!mutexes&&!queues&&!tasks);
 }
 fail=0;assert(panel_bus_start()==ESP_OK);assert(driver==1&&mutexes==1&&queues==1&&tasks==1);
 assert(s_live.profile_locked && s_live.config.profile_override==PANEL_PROFILE_12KU && s_live.config.address==7);
 assert(panel_bus_start()==ESP_OK);assert(driver==1&&mutexes==1&&queues==1&&tasks==1);
 panel_command_t invalid={.type=(panel_command_type_t)99};assert(!panel_bus_submit_sequence(&invalid,NULL));assert(!submitted&&!s_reserved);
 panel_command_t valid={.type=PANEL_CMD_WRITE,.address=0x4101,.value=1};uint32_t seq=0;
 assert(!panel_bus_submit_in_context(&valid,0,&seq) && !submitted && !s_reserved);
 assert(panel_bus_submit_sequence(&valid,&seq)&&seq==1&&submitted==1&&s_reserved);
 assert(!panel_bus_submit_sequence(&valid,NULL)&&submitted==1);
 puts("PASS bus startup: mutex/queue/task failures release all allocations, idempotent start, invalid/busy commands rejected");
}
'''
cfile=root/'tmp/bus_startup_test.c';exe=cfile.with_suffix('.exe')
cfile.write_text(test.replace('STARTUP',config+'\n'+startup).replace('ADMISSION',admission),encoding='utf-8')
subprocess.run([sys.argv[1],'-std=c11','-Wall','-Wextra','-Werror','-I'+str(root/'main'),str(cfile),str(root/'main/panel_model.c'),'-o',str(exe)],check=True)
result=subprocess.run([str(exe)],capture_output=True,text=True)
assert result.returncode==0,result.stdout+result.stderr
print(result.stdout,end='')
