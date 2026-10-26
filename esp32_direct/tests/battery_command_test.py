"""Actual grouped MQTT/BLE dispatchers with host-only storage/UART boundaries."""
import ast
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
GCC = Path(r'C:\Users\29538\AppData\Local\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin\gcc.exe')
JSON = Path(r'D:\.espressif\v5.5.4\esp-idf\components\json\cJSON')
MBED = JSON.parents[1] / 'mbedtls/mbedtls'


def template(name):
    tree = ast.parse((ROOT / 'tests' / name).read_text(encoding='utf-8'))
    assignment = next(n for n in tree.body if isinstance(n, ast.Assign) and
                      any(isinstance(t, ast.Name) and t.id == 'harness' for t in n.targets))
    return ast.literal_eval(assignment.value.func.value)


def source(path):
    return re.sub(r'^#include "[^\n]+"\s*$', '', (ROOT / path).read_text(encoding='utf-8'), flags=re.M)


def run(name, code, ble=False):
    target = ROOT / 'tmp' / (name + '.c')
    target.write_text(code, encoding='utf-8')
    config = ROOT / 'tmp/battery_sha256_host_config.h'
    config.write_text('#define MBEDTLS_SHA256_C\n#define MBEDTLS_PLATFORM_C\n', encoding='utf-8')
    sources = [str(target), str(JSON / 'cJSON.c'), str(MBED / 'library/sha256.c'),
               str(MBED / 'library/platform_util.c'), str(MBED / 'library/platform.c')]
    policy = ROOT / 'main/direct/direct_battery_config.c'
    if policy.exists():
        sources.append(str(policy))
    if ble:
        sources.append('components/ble_ct/ble_ct_frames.c')
    exe = target.with_suffix('.exe')
    subprocess.run([str(GCC), '-std=c11', '-Wall', '-Wextra', '-Werror', '-pthread', '-static',
                    '-Wno-unused-function', '-Wno-unused-variable', '-Wno-unused-parameter',
                    '-Imain/app', '-Imain/telemetry', '-Imain/direct', '-Icomponents/ble_ct',
                    '-I'+str(JSON), '-I'+str(MBED/'include'), '-I'+str(target.parent),
                    '-DMBEDTLS_CONFIG_FILE="battery_sha256_host_config.h"', *sources, '-o', str(exe)],
                   cwd=ROOT, check=True)
    subprocess.run([str(exe)], check=True)


CONFIG = r'{"type":4,"protocol":5,"material":0,"cells":0,"bulk_dv":540,"float_dv":540,"cutoff_dv":450,"max_charge_a":120,"ac_charge_a":50,"back_utility_dv":480,"back_battery_dv":520,"soc_low":20,"soc_high":80,"soc_cutoff":10,"revision":5}'
host = r'''
#include "direct_battery_config.h"
static direct_battery_config_t battery={4,5,0,0,540,540,450,120,50,480,520,20,80,10,5};
static unsigned battery_writes;
static int battery_enqueue_error;
static bool battery_ready=true;
static bool direct_battery_config_get(direct_battery_config_t *out){if(!battery_ready)return false;*out=battery;return true;}
static int direct_battery_config_write(const direct_battery_config_t *c,bool cloud){
 assert(cloud);if(battery_enqueue_error)return battery_enqueue_error;
 if(c->revision!=battery.revision)return -14;
 battery_writes++;battery=*c;battery.revision++;queued=true;addr=4;raw=c->type==4?3:c->type==3?4:c->type;
 if(synchronous){queued=false;write_cb(CS_CMD_WRITE_CTRLPARAM,4,2,0);}return 0;
}
'''
cloud_source = source('main/telemetry/cmd_handler.c')
arm = (ROOT/'main/telemetry/arm_poll.h').read_text(encoding='utf-8')
cloud_source = re.search(r'typedef enum \{\s*ARM_WRITE_ACCEPTED.*?\} arm_write_result_t;', arm, re.S).group()+'\n'+cloud_source
base = template('cloud_command_replay_test.py')
prefix = base[:base.index('int main(void){')].replace('#include "../tests/battery_command_host.h"', '')
prefix = prefix.replace('static void ble_ct_cmd_on_write_saved(uint32_t token,const char *id,double value,uint32_t revision){(void)token;(void)id;(void)value;(void)revision;}',
                        'static unsigned group_ble_saved; static void ble_ct_cmd_on_write_saved(uint32_t token,const char *id,double value,uint32_t revision){assert(token==123 && !strcmp(id,"battery_config") && value==0 && revision==6);group_ble_saved++;}')
prefix = prefix.replace('SOURCE', host+'\n'+cloud_source)
main = r'''
static const char *config="CONFIG";
static void group(const char *id,const char *cfg,bool retained){
 char msg[1025];snprintf(msg,sizeof(msg),"{\"task_id\":\"%s\",\"cmd\":\"set_battery_config\",\"args\":%s}",id,cfg);
 cmd_handler_on_mqtt_delivery("cs_inv/TEST/cmd",msg,(unsigned)strlen(msg),retained);cmd_handler_tick();
}
int main(void){
 cmd_handler_init();request("{\"cmd\":\"get_battery_config\"}");
 expect("result","OK");assert(strstr(response,"\"revision\":5") && strstr(response,"\"bulk_dv\":540"));
 group("group",config,false);assert(battery_writes==1 && writes==0);expect("stage","acknowledged");
 group("group",config,false);assert(battery_writes==1);finish(0);expect("stage","submitted");
 assert(strstr(response,"\"saved\":true") && strstr(response,"\"submitted\":true") && strstr(response,"\"observed\":false") && !strstr(response,"applied"));
 group("group",config,false);assert(battery_writes==1);expect("stage","submitted");
 group("stale",config,false);expect("result","REVISION_CONFLICT");assert(battery_writes==1);
 const char *bad[]={"{}","{\"revision\":5}","{\"revision\":1.5}","{\"type\":4,\"type\":5}","[]"};
 for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++){group("bad",bad[i],false);expect("result","INVALID_ARGS");assert(battery_writes==1);}
 cJSON *j=cJSON_Parse(config);assert(j);
 const char *fields[]={"type","protocol","material","cells","bulk_dv","float_dv","cutoff_dv","max_charge_a","ac_charge_a","back_utility_dv","back_battery_dv","soc_low","soc_high","soc_cutoff","revision"};
 for(unsigned i=0;i<15;i++){
  cJSON *copy=cJSON_Duplicate(j,true);cJSON_DeleteItemFromObjectCaseSensitive(copy,fields[i]);char *s=cJSON_PrintUnformatted(copy);group("missing",s,false);expect("result","INVALID_ARGS");cJSON_free(s);cJSON_Delete(copy);
  copy=cJSON_Duplicate(j,true);cJSON_AddNumberToObject(copy,fields[i],1);s=cJSON_PrintUnformatted(copy);group("duplicate",s,false);expect("result","INVALID_ARGS");cJSON_free(s);cJSON_Delete(copy);
  copy=cJSON_Duplicate(j,true);cJSON_SetNumberValue(cJSON_GetObjectItem(copy,fields[i]),1.5);s=cJSON_PrintUnformatted(copy);group("fractional",s,false);expect("result","INVALID_ARGS");cJSON_free(s);cJSON_Delete(copy);
 }
 cJSON_AddNumberToObject(j,"extra",1);char *s=cJSON_PrintUnformatted(j);group("unknown",s,false);expect("result","INVALID_ARGS");cJSON_free(s);cJSON_Delete(j);
 const double revisions[]={-1,4294967296.0};
 for(unsigned i=0;i<2;i++){j=cJSON_Parse(config);cJSON_SetNumberValue(cJSON_GetObjectItem(j,"revision"),revisions[i]);s=cJSON_PrintUnformatted(j);group("badrevision",s,false);expect("result","INVALID_ARGS");cJSON_free(s);cJSON_Delete(j);}
 group("retained",config,true);expect("result","RETAINED_REJECTED");
 enqueue_error=-16;request("{\"cmd\":\"set_battery_type\",\"value\":3}");expect("result","PROFILE_REQUIRED");
 assert((int)cmd_handler_write_param("battery_type",3)==-16 && battery_writes==1 && writes==0);
 enqueue_error=0;
 cmd_handler_init();battery.revision=5;
 j=cJSON_Parse(config);cJSON_SetNumberValue(cJSON_GetObjectItem(j,"protocol"),1);s=cJSON_PrintUnformatted(j);group("protocol",s,false);expect("result","PROTOCOL_UNSUPPORTED");cJSON_free(s);cJSON_Delete(j);
 const int errors[]={-14,-15,-16,-12,-13,-20,-21};const char *reasons[]={"REVISION_CONFLICT","PROTOCOL_UNSUPPORTED","PROFILE_REQUIRED","UPGRADE_BUSY","NOT_READY","STORAGE_FAILED","SUBMIT_FAILED"};
 for(unsigned i=0;i<7;i++){cmd_handler_init();battery_enqueue_error=errors[i];group("error",config,false);expect("result",reasons[i]);assert(!s_pending.active);}
 battery_enqueue_error=0;cmd_handler_init();synchronous=true;group("sync",config,false);expect("stage","submitted");assert(battery_writes==2);group("sync",config,false);assert(battery_writes==2);
 synchronous=false;cmd_handler_init();battery.revision=5;j=cJSON_Parse(config);
 assert(cmd_handler_write_battery_config(j,true)==0 && battery_writes==2);
 assert(cmd_handler_write_battery_config(j,false)==0 && battery_writes==3 && s_ble_pending.active);
 on_arm_write_done(CS_CMD_WRITE_CTRLPARAM,4,1,0);assert(s_ble_pending.active && group_ble_saved==0);
 assert(cmd_handler_write_param("set_output_voltage",230)==CMD_HANDLE_BUSY);
 finish(0);assert(!s_ble_pending.active && group_ble_saved==1);cJSON_Delete(j);
 puts("PASS grouped MQTT: complete schema, canonical units, required revision, atomic write, callback truth, replay, policy errors, scalar rejection");
}
'''.replace('CONFIG', CONFIG.replace('"', '\\"'))
run('battery_mqtt_command_test', prefix+main)

# Reuse the BLE transport, but execute the production schema/policy functions.
base = template('ble_control_flow_test.py')
prefix = base[:base.index('int main(void){')]
prefix = prefix.replace('char json[513]', 'char json[1025]').replace('n<=512', 'n<=1024')
prefix = prefix.replace('clock_ms,5000,512', 'clock_ms,5000,1024')
schema = cloud_source[cloud_source.index('static const struct { const char *name; size_t offset; } s_battery_fields'):cloud_source.index('static bool escape_json_string')]
schema = schema.replace('battery_error(', 'main_battery_error(')
schema_host = r'''
#include <stddef.h>
#include "direct_battery_config.h"
#define PROTO_ERR_INVALID_ARGS "INVALID_ARGS"
#define PROTO_ERR_NOT_SUPPORTED "NOT_SUPPORTED"
#define PROTO_ERR_BUSY "BUSY"
ARM_RESULTS
static direct_battery_config_t battery={4,5,0,0,540,540,450,120,50,480,520,20,80,10,5};
static bool direct_battery_config_get(direct_battery_config_t *out){*out=battery;return true;}
SCHEMA
'''.replace('ARM_RESULTS', re.search(r'typedef enum \{\s*ARM_WRITE_ACCEPTED.*?\} arm_write_result_t;', arm, re.S).group()).replace('SCHEMA', schema)
prefix = prefix.replace('SOURCE', schema_host+'\n'+source('components/ble_ct/ble_ct_cmd.c'))
main = r'''
static const char *config="CONFIG";
static unsigned battery_writes;
static int group_rc;
static int write_battery(const cJSON *json,bool validate_only){
 direct_battery_config_t c;int rc=battery_parse(json,&c);if(rc||validate_only)return rc;
 if(group_rc)return group_rc;
 if(c.revision!=battery.revision)return -14;
 assert(cache_find(s_pending.request.command) && strstr(cache_find(s_pending.request.command)->body,"accepted"));
 battery_writes++;battery=c;battery.revision++;
 if(synchronous)ble_ct_cmd_on_write_saved(ble_ct_cmd_write_token(),"battery_config",0,battery.revision);
 return 0;
}
static void group(const char *id,const char *cfg,bool wrapped){char p[600];snprintf(p,sizeof(p),wrapped?"{\"config\":%s}":"%s",cfg);request(id,"set_battery_config",p,true);}
int main(void){
 ble_ct_cmd_set_executors(write_param,read_param);
 ble_ct_cmd_set_battery_executors(write_battery,cmd_handler_get_battery_config);
 mtu=23;real_tokens=true;
 request("open","open_session","{\"framing_v1\":true,\"direct_control_v1\":true}",true);
 assert(framed && strstr(result,"battery_config_max_message_bytes\":1024"));
 request("getgroup","get_battery_config","{}",true);assert(strstr(result,"\"revision\":5") && strstr(result,"\"type\":4") && strstr(result,"completed") && !strstr(result,"applied"));
 group("writegrp",config,true);assert(battery_writes==1 && writes==0 && strstr(result,"accepted"));
 uint32_t token=ble_ct_cmd_write_token();
 ble_ct_cmd_on_write_saved(token,"battery_config",0,battery.revision);
 assert(strstr(result,"\"status\":\"saved\"") && strstr(result,"\"saved\":true") && strstr(result,"\"submitted\":true") && strstr(result,"\"observed\":false") && !strstr(result,"applied") && !strstr(result,"\"value\""));
 group("writegrp",config,false);assert(battery_writes==1 && strstr(result,"saved"));
 cJSON *j=cJSON_Parse(config);cJSON_SetNumberValue(cJSON_GetObjectItem(j,"revision"),6);char *changed=cJSON_PrintUnformatted(j);
 group("writegrp",changed,false);assert(battery_writes==1 && strstr(result,"IDEMPOTENCY_CONFLICT"));
 ble_ct_cmd_reset_connection();strcpy(session,"session-2");request("open2","open_session","{\"framing_v1\":true,\"direct_control_v1\":true}",true);
 group("writegrp",config,false);assert(battery_writes==1 && strstr(result,"saved") && strstr(result,"session-2"));
 group("stale",config,false);assert(battery_writes==1 && strstr(result,"REVISION_CONFLICT"));
 group("pending",changed,false);assert(battery_writes==2);token=ble_ct_cmd_write_token();
 group("busy",changed,false);assert(battery_writes==2 && strstr(result,"BUSY"));
 ble_ct_cmd_on_write_saved(token,"battery_config",0,battery.revision);cJSON_free(changed);cJSON_Delete(j);
 const int errors[]={-14,-15,-16,-12,-13,-20,-21};const char *reasons[]={"REVISION_CONFLICT","PROTOCOL_UNSUPPORTED","PROFILE_REQUIRED","UPGRADE_BUSY","NOT_READY","STORAGE_FAILED","SUBMIT_FAILED"};
 for(unsigned i=0;i<7;i++){memset(s_cache,0,sizeof(s_cache));group_rc=errors[i];group("error",config,true);assert(battery_writes==2 && !s_pending.active && strstr(result,reasons[i]));}
 group_rc=0;memset(s_cache,0,sizeof(s_cache));
 const char *bad[]={"{}","{\"type\":4,\"type\":4}","[]"};
 for(unsigned i=0;i<3;i++){group("invalid",bad[i],false);assert(battery_writes==2 && strstr(result,"INVALID_ARGS"));}
 char unknown[600];snprintf(unknown,sizeof(unknown),"{\"config\":%s,\"extra\":1}",config);request("unknown","set_battery_config",unknown,true);assert(battery_writes==2 && strstr(result,"INVALID_ARGS"));
 cJSON_Hooks hooks={failing_malloc,free};cJSON_InitHooks(&hooks);bool swept=false;
 battery.revision=5;
 for(unsigned fail=1;fail<600;fail++){
  memset(s_cache,0,sizeof(s_cache));s_pending.active=false;allocation_index=0;allocation_failed=false;fail_allocation=fail;unsigned before=battery_writes;
  group("oomgroup",config,true);
  if(allocation_failed)assert(battery_writes==before);
  else {assert(battery_writes==before+1 && strstr(result,"accepted"));swept=true;break;}
 }
 assert(swept);cJSON_InitHooks(NULL);s_pending.active=false;memset(s_cache,0,sizeof(s_cache));battery.revision=5;synchronous=true;
 group("sync",config,false);assert(strstr(result,"saved"));unsigned before=battery_writes;group("sync",config,true);assert(battery_writes==before && strstr(result,"saved"));
 synchronous=false;executor_result=-16;request("scalar","set_param","{\"param_id\":\"set_battery_type\",\"value\":3}",true);assert(strstr(result,"PROFILE_REQUIRED"));
 puts("PASS grouped BLE: real shared parser/policy, MTU23 full config, both params forms, session/replay/conflict, accepted before execution, all allocation failure points, synchronous completion, scalar errors");
}
'''.replace('CONFIG', CONFIG.replace('"', '\\"'))
run('battery_ble_command_test', prefix+main, ble=True)
