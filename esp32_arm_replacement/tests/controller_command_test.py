"""Compile actual remote command mapping/handlers with bounded host transports."""
from pathlib import Path
import re, subprocess
root=Path(__file__).resolve().parents[1]
source=(root/'main/telemetry/cmd_handler.c').read_text(encoding='utf-8')
def function(name):
    match=re.search(r'^(?:static )?(?:void|bool|cmd_handle_result_t|const cmd_map_t \*)\s*'+name+r'\([^;]*?\)\s*\{',source,re.M)
    assert match,name
    at=source.index('{',match.start());end=at+1;depth=1
    while depth:depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[match.start():end]
start=source.index('static const cmd_map_t s_cmd_map[]');end=source.index('};',start)+2
mapping=source[start:end]
start=source.index('typedef struct {',source.index('/* ---- 待确认命令'))
end=source.index('/* ARM 写应答回调',start)
state=source[start:end]
code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "cs_protocol_defs.h"
#include "cs_protocol.h"
#include "cmd_handler.h"
#include "arm_param.h"
#define DIRECT_DSP_CONFIG_COUNT 84
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
typedef struct {const char *name;uint16_t addr,scale;} cmd_map_t;
MAPPING
#define CMD_MAP_SIZE (sizeof(s_cmd_map)/sizeof(s_cmd_map[0]))
STATE
static unsigned successes,failures,acks,readbacks;
static uint16_t stored[84],queued_index,queued_value;static bool queued;
static int arm_poll_send_write(uint8_t cmd,uint16_t index,uint16_t n,const uint8_t *data){
 assert(cmd==CS_CMD_WRITE_CTRLPARAM && n==2);if(queued)return -1;
 queued=true;queued_index=index;queued_value=data[0]|((uint16_t)data[1]<<8);return 0;
}
static bool direct_config_get_wire(uint16_t index,uint16_t *out){if(index>=84)return false;*out=stored[index];return true;}
static void send_response(const char *cmd,const char *result,int err,const char *task){(void)cmd;(void)result;(void)task;assert(err<0);failures++;}
static void send_response_ok(const char *cmd,double value,const char *task){(void)cmd;(void)value;(void)task;successes++;}
static void ble_ct_cmd_on_write_ack(const char *id,int err){(void)id;(void)err;acks++;}
static void ble_ct_cmd_on_write_readback(const char *id,double value,uint32_t revision){(void)id;(void)value;assert(revision>0);readbacks++;}
static void on_ctrl_snapshot(const uint16_t *v,size_t n,uint32_t revision);
static void telemetry_request_config(void){uint16_t v[42]={0};on_ctrl_snapshot(v,42,2);}
FUNCTIONS
int main(void){
 const char *names[]={"set_output_voltage","set_output_frequency","set_max_charge_current","set_ac_charge_current","set_low_volt_return_utl","gen_rate_watt","set_alarm_control","set_backlight_ctrl","set_buzzer"};
 const double values[]={230,50,120,50,49,3000,0,0,0};
 const uint16_t raw[]={2300,5000,1200,500,490,3000,0,0,0};
 for(unsigned i=0;i<9;i++){
  handle_cmd(names[i],values[i],"task");assert(queued && queued_value==raw[i] && successes==i);
  stored[queued_index]=queued_value;queued=false;on_arm_write_done(CS_CMD_WRITE_CTRLPARAM,queued_index,2,0);assert(successes==i+1);
  assert(cmd_handler_write_param(names[i],values[i])==CMD_HANDLE_OK && queued_value==raw[i]);
  assert(cmd_handler_write_param("set_output_voltage",220)==CMD_HANDLE_BUSY);
  stored[queued_index]=queued_value;queued=false;on_arm_write_done(CS_CMD_WRITE_CTRLPARAM,queued_index,2,0);
  assert(!s_ble_pending.active && readbacks==i+1);double actual;assert(cmd_handler_get_param(names[i],&actual) && actual==values[i]);
 }
 handle_cmd("set_output_voltage",240,"fail");queued=false;on_arm_write_done(CS_CMD_WRITE_CTRLPARAM,queued_index,2,-2);assert(successes==9 && failures==1);
 assert(cmd_handler_write_param("set_output_voltage",NAN)==CMD_HANDLE_OUT_OF_RANGE);
 handle_cmd("set_output_voltage",INFINITY,"bad");assert(failures==2);
 puts("PASS actual cloud/BLE commands: same engineering units, no OK before durable completion, failure response, pending BLE preserved, controller readback including generator, non-finite rejection");
}
'''
code=code.replace('MAPPING',mapping).replace('STATE',state).replace('FUNCTIONS','\n'.join(function(n) for n in ('cmd_lookup','cmd_handler_write_param','cmd_handler_get_param','on_ctrl_snapshot','on_arm_write_done','handle_cmd')))
target=root/'tmp/controller_command_test.c';target.write_text(code,encoding='utf-8')
gcc=Path(r'C:\Users\29538\AppData\Local\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin\gcc.exe')
exe=root/'tmp/controller_command_test.exe'
subprocess.run([str(gcc),'-std=c11','-Wall','-Wextra','-Werror','-Imain/app','-Imain/telemetry',str(target),'-o',str(exe)],cwd=root,check=True)
subprocess.run([str(exe)],check=True)
