"""Run the actual panel command dispatcher, including scalar replay tests.

The shared harness supplies real cJSON/SHA256 and transport stubs. Only its
implementation source is redirected; the original controller stays unchanged.
"""
from pathlib import Path
import subprocess

panel = Path(__file__).resolve().parents[1]
shared = panel.parent / 'esp32_direct'
script = (shared/'tests/cloud_command_replay_test.py').read_text(encoding='utf-8')
script = script.replace("source = (ROOT / 'main/telemetry/cmd_handler.c').read_text(encoding='utf-8')",
                        "source = Path("+repr(str(panel/'full/cmd_handler.c'))+").read_text(encoding='utf-8')")
script = script.replace('#include "../tests/battery_command_host.h"', '''#define FULL_WRITE_UNCONFIRMED (-22)
#define FULL_WRITE_PROTOCOL_ACCEPTED (-23)
static void ble_ct_cmd_on_write_protocol_accepted(uint32_t token,const char *id){(void)token;(void)id;}''')
script = script.replace('230.05', '230.1').replace('valuedouble==230.0','valuedouble==230.1')
script = script.replace('quantized submitted value','exact submitted value')
script = script.replace("target = ROOT / 'tmp/cloud_command_replay_test.c'",
                        "target = Path("+repr(str(panel/'tmp/legacy_protocol_test.c'))+")")
extra = r'''
 assert(cmd_handler_get_battery_config()==NULL);
 assert(cmd_handler_write_battery_config(NULL,true)==-15);
 assert(cmd_handler_write_battery_config(NULL,false)==-15);
 cJSON *any=cJSON_Parse("{\"type\":1}");assert(any);
 assert(cmd_handler_write_battery_config(any,true)==-15);
 assert(cmd_handler_write_battery_config(any,false)==-15);cJSON_Delete(any);
 assert(!s_pending.active && !s_ble_pending.active && writes==0);
 uint16_t raw=0;
 assert(scalar_raw(cmd_lookup("set_output_voltage"),230.1,&raw)&&raw==2301);
 assert(!scalar_raw(cmd_lookup("set_output_voltage"),230.05,&raw));
 assert(cmd_handler_write_param("set_output_voltage",230.05)==CMD_HANDLE_OUT_OF_RANGE);
 request("{\"task_id\":\"precision\",\"cmd\":\"set_output_voltage\",\"args\":[230.05]}");
 expect("result","OUT_OF_RANGE");assert(!writes&&!s_pending.active&&!s_ble_pending.active);
 const char *unsupported[]={"get_battery_config","set_battery_config","validate_battery_config"};
 for(unsigned i=0;i<3;i++){
  char msg[180];snprintf(msg,sizeof(msg),"{\"task_id\":\"private\",\"cmd\":\"%s\",\"args\":{\"type\":1}}",unsupported[i]);
  request(msg);expect("result","PROTOCOL_UNSUPPORTED");expect("stage","failed");
  assert(writes==0 && !s_pending.active && !s_ble_pending.active);
 }
'''
script = script.replace(' cmd_handler_init();\n const char *first=',
                        ' cmd_handler_init();\n'+extra+'\n const char *first=', 1)
script = script.replace(' request(first);assert(writes==1);finish(0);request(first);',r'''
 request(first);assert(writes==1 && s_pending.active);
 request("{\"task_id\":\"a\",\"cmd\":\"set_battery_config\",\"args\":{}}");
 expect("result","PROTOCOL_UNSUPPORTED");assert(s_pending.active && writes==1);
 assert(cmd_handler_write_battery_config(NULL,true)==-15 && s_pending.active && !s_ble_pending.active);
 finish(0);request(first);''',1)
script = script.replace(' puts("PASS actual MQTT dispatcher:',
                        r'''
 cmd_handler_init();count=writes;
 const char *protocol_only="{\"task_id\":\"receipt-only\",\"cmd\":\"set_output_voltage\",\"args\":[230]}";
 unsigned queries_before_receipt=queries;
 request(protocol_only);assert(writes==count+1);
 finish(FULL_WRITE_PROTOCOL_ACCEPTED);expect("stage","submitted");expect("result","OK");
 assert(strstr(response,"\"submitted_args\":[230]") && !strstr(response,"applied_args"));
 assert(!strstr(response,"desired_revision") && queries==queries_before_receipt);
 request(protocol_only);expect("stage","submitted");assert(writes==count+1);
 request("{\"task_id\":\"missing-readback\",\"cmd\":\"set_output_voltage\",\"args\":[230]}");
 finish(FULL_WRITE_UNCONFIRMED);expect("result","VALUE_UNCONFIRMED");expect("stage","failed");
 puts("PASS private protocol rejection and protocol-only MQTT receipt: no reported value and no duplicate write");
 puts("PASS actual MQTT dispatcher:''')
exec(compile(script, str(shared/'tests/cloud_command_replay_test.py'), 'exec'),
     {'__file__':str(shared/'tests/cloud_command_replay_test.py'), '__name__':'__main__'})

# Exercise the actual factory refusal helper without USB/OTA/NVS stubs.
factory=(panel/'full/factory_service.c').read_text(encoding='utf-8')
start=factory.index('static bool factory_private_command(')
end=factory.index('static void serial_result(',start)
helper=factory[start:end]
assert 'direct_dsp_params_' not in factory and 'direct_link_dsp_control' not in factory
harness=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
typedef enum {ADMIN_SN_NONE,ADMIN_SN_VALID,ADMIN_SN_DSP_VALID,ADMIN_SN_DSP_STATUS,
 ADMIN_SN_DSP_BOOT,ADMIN_SN_DSP_RUN,ADMIN_SN_DSP_REBOOT} admin_sn_result_t;
static struct {unsigned char dsp_bundle[168];} admin_input;
static const char *g_device_sn="TEST";
static char response[256];
static int capture(const char *format,...){va_list args;va_start(args,format);int n=vsnprintf(response,sizeof(response),format,args);va_end(args);return n;}
#define printf capture
HELPER
#undef printf
int main(void){
 for(unsigned result=ADMIN_SN_DSP_VALID;result<=ADMIN_SN_DSP_REBOOT;result++){
  memset(admin_input.dsp_bundle,0xa5,sizeof(admin_input.dsp_bundle));
  assert(factory_private_command((admin_sn_result_t)result));
  assert(strstr(response,"PROTOCOL_UNSUPPORTED") && !strstr(response,"BUSY"));
  if(result==ADMIN_SN_DSP_VALID)for(unsigned i=0;i<sizeof(admin_input.dsp_bundle);i++)assert(admin_input.dsp_bundle[i]==0);
 }
 assert(!factory_private_command(ADMIN_SN_VALID));
 puts("PASS factory private bundle: actual import/export/Boot refusal, no ownership/save/codec dependency");
}
'''.replace('HELPER',helper)
target=panel/'tmp/factory_private_test.c';target.write_text(harness,encoding='utf-8')
exe=target.with_suffix('.exe')
gcc=r'C:\Users\29538\AppData\Local\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin\gcc.exe'
subprocess.run([gcc,'-std=c11','-Wall','-Wextra','-Werror',str(target),'-o',str(exe)],check=True)
subprocess.run([str(exe)],check=True)
