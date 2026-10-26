"""Execute the actual dispatcher with the shared host UART/JSON transport."""
import ast
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
base = ast.parse((ROOT / 'tests/cloud_command_replay_test.py').read_text(encoding='utf-8'))
assignment = next(n for n in base.body if isinstance(n, ast.Assign) and any(isinstance(t, ast.Name) and t.id == 'harness' for t in n.targets))
template = ast.literal_eval(assignment.value.func.value)
import re
source = (ROOT / 'main/telemetry/cmd_handler.c').read_text(encoding='utf-8')
source = re.sub(r'^#include "[^\n]+"\s*$', '', source, flags=re.M)
arm_header = (ROOT / 'main/telemetry/arm_poll.h').read_text(encoding='utf-8')
source = re.search(r'typedef enum \{\s*ARM_WRITE_ACCEPTED.*?\} arm_write_result_t;', arm_header, re.S).group() + '\n' + source
prefix = template[:template.index('int main(void){')]
main = r'''
int main(void){
 cmd_handler_init();
 for(unsigned i=0;i<20;i++){
  char msg[160];snprintf(msg,sizeof(msg),"{\"task_id\":\"query-%u\",\"cmd\":\"query_info\"}",i);
  request(msg);expect("stage","completed");
 }
 const char *valid="{\"task_id\":\"write-1\",\"cmd\":\"set_output_voltage\",\"args\":[230]}";
 request(valid);assert(writes==1);finish(0);
 for(unsigned i=0;i<20;i++){
  char msg[192];snprintf(msg,sizeof(msg),"{\"task_id\":\"expired-%u\",\"cmd\":\"set_output_voltage\",\"args\":[230],\"expires_at\":1800000000}",i);
  request(msg);expect("result","EXPIRED");
  snprintf(msg,sizeof(msg),"{\"task_id\":\"invalid-%u\",\"cmd\":\"set_output_voltage\",\"args\":[-1]}",i);request(msg);expect("result","OUT_OF_RANGE");
  snprintf(msg,sizeof(msg),"{\"task_id\":\"unsupported-%u\",\"cmd\":\"ac_on\",\"args\":[1]}",i);request(msg);expect("result","NOT_SUPPORTED");
 }
 request("{\"task_id\":\"write-2\",\"cmd\":\"set_output_voltage\",\"args\":[240]}");assert(writes==2);finish(0);
 request(valid);assert(writes==2);expect("stage","submitted");
#ifndef CMD_HANDLER_QUEUED
 assert(!"MQTT callbacks must only enqueue bounded input");
#else
 cmd_handler_init();unsigned count=writes;
 char mutable[160];snprintf(mutable,sizeof(mutable),"%s",valid);
 cmd_handler_on_mqtt_message("cs_inv/TEST/cmd",mutable,(unsigned)strlen(mutable));
 assert(writes==count);memset(mutable,'x',strlen(mutable));cmd_handler_tick();assert(writes==count+1);finish(0);
 request("{\"task_id\":\"write-1\",\"cmd\":\"set_output_voltage\",\"args\":[230],\"expires_at\":1}");assert(writes==count+1);expect("stage","submitted");
 cmd_handler_on_mqtt_delivery("cs_inv/TEST/cmd",valid,(unsigned)strlen(valid),true);assert(writes==count+1);cmd_handler_tick();expect("result","RETAINED_REJECTED");assert(writes==count+1);
 const char *query="{\"task_id\":\"read\",\"cmd\":\"query_info\"}";
 cmd_handler_on_mqtt_delivery("cs_inv/TEST/cmd",query,(unsigned)strlen(query),true);cmd_handler_tick();expect("stage","completed");
 const char *later="{\"task_id\":\"later\",\"cmd\":\"set_output_voltage\",\"args\":[240],\"expires_at\":1800000001}";
 cmd_handler_on_mqtt_message("cs_inv/TEST/cmd",later,(unsigned)strlen(later));wall+=2;cmd_handler_tick();expect("result","EXPIRED");assert(writes==count+1);wall-=2;
 cmd_handler_init();
 for(unsigned i=0;i<CMD_HANDLER_QUEUE_DEPTH+1;i++){
  char msg[128];snprintf(msg,sizeof(msg),"{\"task_id\":\"queue-%u\",\"cmd\":\"query_info\"}",i);
  cmd_handler_on_mqtt_message("cs_inv/TEST/cmd",msg,(unsigned)strlen(msg));
 }
 cmd_handler_stats_t stats;cmd_handler_get_stats(&stats);assert(stats.queue_depth==CMD_HANDLER_QUEUE_DEPTH && stats.queue_full==1);
 unsigned reads=queries;
 for(unsigned i=0;i<CMD_HANDLER_QUEUE_DEPTH;i++){char id[24];snprintf(id,sizeof(id),"queue-%u",i);cmd_handler_tick();expect("task_id",id);}
 cmd_handler_tick();assert(queries==reads+CMD_HANDLER_QUEUE_DEPTH);cmd_handler_get_stats(&stats);assert(stats.queue_depth==0);
 const int rejects[]={ARM_WRITE_INVALID,ARM_WRITE_UNSUPPORTED,ARM_WRITE_UPGRADE_BUSY,ARM_WRITE_NOT_READY};
 const char *results[]={"INVALID_ARGS","NOT_SUPPORTED","UPGRADE_BUSY","NOT_READY"};
 const cmd_handle_result_t local_results[]={CMD_HANDLE_OUT_OF_RANGE,CMD_HANDLE_UNSUPPORTED,CMD_HANDLE_UPGRADE_BUSY,CMD_HANDLE_NOT_READY};
 for(unsigned i=0;i<4;i++){
  enqueue_error=rejects[i];request(valid);expect("result",results[i]);
  assert(cmd_handler_write_param("set_output_voltage",230)==local_results[i]);
 }
 enqueue_error=0;
 request(valid);finish(ARM_WRITE_SAVE_FAILED);expect("result","STORAGE_FAILED");
 request("{\"task_id\":\"submit-failure\",\"cmd\":\"set_output_voltage\",\"value\":230}");finish(ARM_WRITE_SUBMIT_FAILED);expect("result","SUBMIT_FAILED");
 cmd_handler_init();
 for(unsigned i=0;i<8;i++){
  char msg[160];snprintf(msg,sizeof(msg),"{\"task_id\":\"write-%u\",\"cmd\":\"set_output_voltage\",\"args\":[230]}",i);request(msg);finish(0);
 }
 request(query);expect("stage","completed");
 count=writes;request("{\"task_id\":\"overflow\",\"cmd\":\"set_output_voltage\",\"args\":[230]}");expect("result","BUSY");assert(writes==count);
 puts("PASS actual admission: read/invalid/expired isolation, pending/result replay, callback copy+defer, retained write refusal, bounded overflow, dequeue deadline, write TTL retention");
#endif
}
'''
code = (prefix + main).replace('SOURCE', source)
target = ROOT / 'tmp/cloud_command_admission_test.c'
target.parent.mkdir(exist_ok=True)
target.write_text(code, encoding='utf-8')
json = Path(r'D:\.espressif\v5.5.4\esp-idf\components\json\cJSON')
mbed = json.parents[1] / 'mbedtls/mbedtls'
(target.parent / 'cloud_sha256_host_config.h').write_text('#define MBEDTLS_SHA256_C\n#define MBEDTLS_PLATFORM_C\n', encoding='utf-8')
gcc = r'C:\Users\29538\AppData\Local\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin\gcc.exe'
exe = target.with_suffix('.exe')
subprocess.run([gcc, '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-misleading-indentation', '-Wno-unused-function', '-Wno-unused-variable', '-pthread', '-static',
    '-Imain/app', '-Imain/telemetry', '-I'+str(json), '-I'+str(mbed/'include'), '-I'+str(target.parent), '-DMBEDTLS_CONFIG_FILE="cloud_sha256_host_config.h"',
    str(target), str(json/'cJSON.c'), str(mbed/'library/sha256.c'), str(mbed/'library/platform_util.c'), str(mbed/'library/platform.c'), '-o', str(exe)], cwd=ROOT, check=True)
subprocess.run([str(exe)], check=True)
