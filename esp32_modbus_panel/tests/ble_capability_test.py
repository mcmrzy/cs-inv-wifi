"""Run the production BLE dispatcher and native panel API against a stub bus."""
from pathlib import Path
import re
root=Path(__file__).resolve().parents[1]
shared=root.parent/'esp32_direct'
script=(shared/'tests/ble_control_flow_test.py').read_text(encoding='utf-8')
script=script.replace('ROOT = Path(__file__).resolve().parents[1]',f'ROOT = Path({str(shared)!r})')
script=script.replace("target=ROOT/'tmp/ble_control_flow_test.c'",f"target=Path({str(root/'tmp/ble_capability_test.c')!r})")
panel=(root/'full/panel_ble.c').read_text(encoding='utf-8')
panel=re.sub(r'^#include "[^\n]+"\s*$', '', panel, flags=re.M)
bus_stubs=r'''
#include "panel_types.h"
#include "panel_catalog.h"
#include "panel_locale.h"
static panel_snapshot_t native_live;
static panel_command_t native_command;
static unsigned native_submits,native_refreshes;
static uint16_t battery_suggestion;
static bool native_ota;
bool ota_service_is_busy(void){return native_ota;}
bool panel_bus_snapshot(panel_snapshot_t *out){*out=native_live;return true;}
uint32_t panel_bus_context_generation(void){return native_live.context_generation;}
bool panel_bus_submit_in_context(const panel_command_t *command,uint32_t ctx,uint32_t *sequence){
 if(ctx!=native_live.context_generation || native_live.command_busy)return false;
 native_command=*command;
 native_live.command_busy=true;
 *sequence=++native_live.command_sequence;
 native_submits++;
 return true;
}
void panel_bus_request_ble_setting(uint16_t address){assert(address==0x4105);native_refreshes++;}
const char *panel_ble_aux_read(const char *action,const cJSON *params,cJSON *actual){
 (void)action;(void)params;(void)actual;return "UNSUPPORTED_ACTION";
}
int panel_ble_aux_write(const char *id,double value){(void)id;(void)value;return -100;}
uint16_t panel_ble_battery_suggestion(const panel_snapshot_t *s,uint16_t address){
 (void)s;return address==0x4106?battery_suggestion:0;
}
'''
script=script.replace("SOURCE\nstatic int write_param", "SOURCE\nPANEL_IMPL\nstatic int write_param")
script=script.replace(".replace('SOURCE', source)",f".replace('SOURCE', source).replace('PANEL_IMPL',{(bus_stubs+panel)!r})")
native_paths=[str(root/'main/panel_catalog.c'),str(root/'main/panel_model.c'),str(root/'main/panel_locale.c')]
script=script.replace("'-DMBEDTLS_CONFIG_FILE=",f"'-I{root.as_posix()}/main',*{native_paths!r},'-DMBEDTLS_CONFIG_FILE=")
extra=r'''
 request("receipt-only","set_param","{\"param_id\":\"set_output_voltage\",\"value\":230}",true);
 uint32_t receipt_token=ble_ct_cmd_write_token();
 ble_ct_cmd_on_write_protocol_accepted(receipt_token+1,"set_output_voltage");
 assert(s_pending.active);
 ble_ct_cmd_on_write_protocol_accepted(receipt_token,"set_output_voltage");
 assert(!s_pending.active && strstr(result,"PROTOCOL_ACCEPTED") && strstr(result,"\"submitted\":true"));
 assert(strstr(result,"\"saved\":null") && strstr(result,"\"observed\":false") && !strstr(result,"\"value\":"));
 unsigned receipt_writes=writes;
 request("receipt-only","set_param","{\"param_id\":\"set_output_voltage\",\"value\":230}",true);
 assert(writes==receipt_writes && strstr(result,"PROTOCOL_ACCEPTED"));
 unsigned before_unsupported=writes;
 ble_ct_cmd_set_battery_supported(false);
 request("bundle-get","get_battery_config","{}",true);
 assert(strstr(result,"PROTOCOL_UNSUPPORTED") && writes==before_unsupported && !s_pending.active);
 request("bundle-set","set_battery_config","{\"config\":{}}",true);
 assert(strstr(result,"PROTOCOL_UNSUPPORTED") && writes==before_unsupported && !s_pending.active);
 request("cap-open","open_session","{\"framing_v1\":true,\"direct_control_v1\":true}",true);
 assert(strstr(result,"\"battery_config_supported\":false") && strstr(result,"\"battery_config_max_message_bytes\":0"));
 ble_ct_cmd_set_battery_supported(true);
 request("cap-restore","open_session","{\"framing_v1\":true,\"direct_control_v1\":true}",true);
 assert(strstr(result,"\"battery_config_supported\":true"));
 puts("PASS BLE product capability: grouped format explicitly unsupported without pending/writes, original default restored");
 panel_ble_init();
 real_tokens=true;
 native_live.now_ms=clock_ms;
 native_live.context_generation=7;
 native_live.config.profile_override=PANEL_PROFILE_3KU;
 panel_store(&native_live,0x42,2,clock_ms);
 panel_store(&native_live,0x2a1,5000,clock_ms);
 request("n0000001","open_session","{\"framing_v1\":true,\"direct_control_v1\":true}",true);
 assert(strstr(result,"\"panel_api_v1\":true"));
 request("n0000002","panel_info","{}",true);
 assert(strstr(result,"\"model\":\"3KU\"") && strstr(result,"\"context\":7") && strstr(result,"\"power_on\":true"));
 request("n0000003","panel_setting","{\"index\":1}",true);
 assert(strstr(result,"\"id\":\"panel_4101\"") && strstr(result,"\"value\":0") && strstr(result,"\"observed\":true"));
 request("setting-en","panel_setting","{\"index\":1,\"en\":true}",true);
 assert(strstr(result,"\"label\":\"Output frequency\"") && strstr(result,"\"value\":0"));
 request("setting-zh","panel_setting","{\"index\":1}",true);
 assert(strstr(result,"\"label\":\"输出频率\""));
 request("n0000004","panel_step","{\"index\":5,\"value\":13,\"direction\":1,\"context\":7}",true);
 assert(strstr(result,"\"value\":14"));
 request("n0000005","panel_setting","{\"index\":5,\"refresh\":true}",true);
 assert(native_refreshes==1 && strstr(result,"\"reading\":true"));
 native_live.setting_reads[0]=(panel_setting_read_t){0x4105,14,clock_ms,PANEL_SETTING_VALUE,false};
 request("n0000006","panel_setting","{\"index\":5,\"refresh\":true}",true);
 assert(native_refreshes==1 && strstr(result,"\"reading\":false") && strstr(result,"\"value\":14"));
 request("n0000007","set_param","{\"param_id\":\"panel_4101\",\"value\":1,\"context\":6}",true);
 assert(!native_submits && strstr(result,"REVISION_CONFLICT"));
 request("n0000008","set_param","{\"param_id\":\"panel_4101\",\"value\":1,\"context\":7}",true);
 assert(native_submits==1 && native_command.address==0x4101 && native_command.value==1 && strstr(result,"\"accepted\""));
 panel_result_record(&native_live,native_live.command_sequence,PANEL_WRITE_CONFIRMED,0);
 native_live.command_busy=false;
 panel_ble_tick();
 assert(strstr(result,"\"status\":\"applied\"") && strstr(result,"\"observed\":true") && strstr(result,"\"value\":1"));
 request("n0000008","set_param","{\"param_id\":\"panel_4101\",\"value\":1,\"context\":7}",true);
 assert(native_submits==1 && strstr(result,"\"status\":\"applied\""));
 request("n0000009","set_param","{\"param_id\":\"panel_4105\",\"value\":14,\"context\":7}",true);
 assert(native_submits==2 && native_command.address==0x4105 && native_command.value==14);
 panel_result_record(&native_live,native_live.command_sequence,PANEL_WRITE_PROTOCOL_ACCEPTED,0);
 native_live.command_busy=false;
 panel_ble_tick();
 assert(strstr(result,"\"status\":\"submitted\"") && strstr(result,"\"observed\":false") && !strstr(result,"\"value\":"));
 request("n000000a","set_param","{\"param_id\":\"panel_4123\",\"value\":1,\"context\":7}",true);
 assert(native_submits==3 && native_command.address==0x4123 && native_command.value==1);
 panel_result_record(&native_live,native_live.command_sequence,PANEL_WRITE_PROCESSED,0);
 native_live.command_busy=false;
 panel_ble_tick();
 assert(strstr(result,"VALUE_MISMATCH"));
 native_ota=true;
 request("n000000b","set_param","{\"param_id\":\"panel_4101\",\"value\":1,\"context\":7}",true);
 assert(native_submits==3 && strstr(result,"UPGRADE_BUSY"));
 native_ota=false;
 battery_suggestion=7;
 panel_store(&native_live,0x179,6000,clock_ms);
 request("n000000c","panel_setting","{\"index\":6,\"refresh\":true}",true);
 assert(strstr(result,"\"suggestion\":7") && strstr(result,"\"suggested_text\":\"70 A\""));
 battery_suggestion=6;
 request("n000000d","set_param","{\"param_id\":\"panel_4106\",\"value\":7,\"context\":7,\"bms_suggestion\":true}",true);
 assert(native_submits==3 && strstr(result,"REVISION_CONFLICT"));
 battery_suggestion=7;
 request("n000000e","set_param","{\"param_id\":\"panel_4106\",\"value\":7,\"context\":7,\"bms_suggestion\":true}",true);
 assert(native_submits==4 && native_command.address==0x4106 && native_command.value==7);
 panel_result_record(&native_live,native_live.command_sequence,PANEL_WRITE_CONFIRMED,0);
 native_live.command_busy=false;
 panel_ble_tick();
 const panel_profile_t models[]={PANEL_PROFILE_2KU,PANEL_PROFILE_3KU,PANEL_PROFILE_6KU,PANEL_PROFILE_12KU};
 for(unsigned m=0;m<sizeof(models)/sizeof(models[0]);m++){
  native_live.config.profile_override=models[m];
  for(unsigned i=0;i<panel_setting_count();i++){
   char id[10],params[40];snprintf(id,sizeof(id),"p%02u%02u",m,i);snprintf(params,sizeof(params),"{\"index\":%u}",i);
   request(id,"panel_setting",params,true);
   assert(strlen(result)<=512 && strstr(result,"\"status\":\"applied\"") && strstr(result,"\"group\":"));
   id[0]='e';snprintf(params,sizeof(params),"{\"index\":%u,\"en\":true}",i);
   request(id,"panel_setting",params,true);
   assert(strlen(result)<=512 && strstr(result,"\"status\":\"applied\"") && strstr(result,"\"group\":"));
   for(const unsigned char *p=(const unsigned char *)result;*p;p++)assert(*p<128);
  }
 }
 puts("PASS native panel BLE: four-model catalog, current/step, stale context, applied/submitted/mismatch, power, OTA guard, and replay");
'''
marker=' printf("PASS actual dispatcher:'
assert marker in script
script=script.replace(marker,extra+marker)
exec(compile(script,str(shared/'tests/ble_control_flow_test.py'),'exec'),{'__file__':str(shared/'tests/ble_control_flow_test.py')})
