"""Exercise the actual cJSON command dispatcher and frame codec on the host."""
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
GCC = Path(r'C:\Users\29538\AppData\Local\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin\gcc.exe')
JSON = Path(r'D:\.espressif\v5.5.4\esp-idf\components\json\cJSON')
MBEDTLS = JSON.parents[1] / 'mbedtls/mbedtls'
source = (ROOT / 'components/ble_ct/ble_ct_cmd.c').read_text(encoding='utf-8')
source = re.sub(r'^#include "[^\n]+"\s*$', '', source, flags=re.M)
api = (ROOT / 'components/ble_ct/ble_ct_cmd.h').read_text(encoding='utf-8')
harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include "cJSON.h"
#include "ble_ct_cmd.h"
#include "ble_ct_frames.h"
#include "mbedtls/sha256.h"
static bool fail_hash;
static unsigned real_hash_calls;
static int command_sha256(const unsigned char *input,size_t len,unsigned char output[32],int is224){
 if(fail_hash)return -1;
 real_hash_calls++;
 return mbedtls_sha256(input,len,output,is224);
}
#define mbedtls_sha256 command_sha256
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define BLE_GATT_ACCESS_OP_WRITE_CHR 1
#define BLE_ATT_ERR_UNLIKELY 14
#define BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN 13
#define BLE_ATT_ERR_INSUFFICIENT_RES 17
#define BLE_HS_CONN_HANDLE_NONE 65535
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(p) ((void)(p))
#define portEXIT_CRITICAL(p) ((void)(p))
struct os_mbuf {unsigned len; unsigned char data[513];};
struct ble_gatt_access_ctxt {int op;struct os_mbuf *om;};
#define OS_MBUF_PKTLEN(p) ((p)->len)
static uint32_t clock_ms=1000,wall_clock;
static bool framed,host=true;
static uint16_t mtu=512;
 static char session[65]="0123456789abcdef0123456789abcdef";
static unsigned writes,notifications,fail_notification;
static bool synchronous;
static int executor_result;
static char result[1025];
static ble_ct_frame_rx_t output;
static int64_t esp_timer_get_time(void){return (int64_t)clock_ms*1000;}
static uint32_t ble_ct_now(void){return wall_clock;}
static const char *ble_ct_get_session_id(void){return session;}
static uint16_t ble_ct_get_conn_handle(void){return 7;}
static uint16_t ble_ct_get_result_handle(void){return 12;}
static bool ble_ct_is_result_subscribed(void){return true;}
static bool ble_ct_framing_enabled(void){return framed;}
static void ble_ct_set_framing_enabled(bool v){framed=v;}
static bool ble_prov_host_acquire(void){return host;}
static void ble_prov_host_release(void){}
static uint16_t ble_att_mtu(uint16_t c){(void)c;return mtu;}
static int ble_hs_mbuf_to_flat(struct os_mbuf *m,void *p,unsigned n,void *x){(void)x;memcpy(p,m->data,n);return 0;}
static struct os_mbuf outbound;
static struct os_mbuf *ble_hs_mbuf_from_flat(const void *p,unsigned n){assert(n<=mtu-3u);outbound.len=n;memcpy(outbound.data,p,n);return &outbound;}
static int ble_gattc_notify_custom(uint16_t c,uint16_t h,struct os_mbuf *m){
 (void)c;(void)h;notifications++;if(fail_notification==notifications)return -1;
 if(framed){int rc=ble_ct_frames_accept(&output,m->data,m->len,clock_ms,5000,512);assert(rc>=0);if(rc==1){memcpy(result,output.data,output.len);result[output.len]=0;}}
 else {memcpy(result,m->data,m->len);result[m->len]=0;}return 0;
}
SOURCE
static int write_param(const char *id,double v){writes++;if(synchronous)ble_ct_cmd_on_write_saved(ble_ct_cmd_write_token(),id,v,8);return executor_result;}
static int read_param(const char *id,double *v){(void)id;*v=230;return BLE_CMD_OK;}
static int submit(const void *p,size_t n){struct os_mbuf m={0};assert(n<=512);m.len=n;memcpy(m.data,p,n);struct ble_gatt_access_ctxt ctx={1,&m};return ble_ct_cmd_access_cb(7,1,&ctx,NULL);}
static bool real_tokens;
static bool expired_timestamp;
static unsigned allocation_index,fail_allocation;
static bool allocation_failed;
static void *failing_malloc(size_t n){if(++allocation_index==fail_allocation){allocation_failed=true;return NULL;}return malloc(n);}
static void request(const char *id,const char *action,const char *params,bool use_frames){
 char json[513],message[70],operation[70];
 snprintf(message,sizeof(message),"m-%s",id);snprintf(operation,sizeof(operation),"o-%s",id);
 if(real_tokens){strcpy(message,"0123456789abcdef01234567");strcpy(operation,"89abcdef0123456789abcdef");}
 int n=snprintf(json,sizeof(json),"{\"v\":2,\"type\":\"control.request\",\"message_id\":\"%s\",\"session_id\":\"%s\",\"body\":{\"session_id\":\"%s\",\"operation_id\":\"%s\",\"sequence\":1,\"command_id\":\"%s\",\"action\":\"%s\",\"params\":%s}}",message,session,session,operation,id,action,params);
 assert(n>0 && n<=512);
 if(expired_timestamp){json[n-2]=0;strcat(json,",\"ts\":1}}");n=(int)strlen(json);}
 if(use_frames){size_t offset=0,len=0;uint8_t seq=0,frame[512];int rc;do{rc=ble_ct_frames_next((uint8_t*)json,n,mtu,&offset,&seq,frame,sizeof(frame),&len);assert(rc>=0);assert(submit(frame,len)==0);}while(rc==0);}
 else assert(submit(json,n)==0);
 ble_ct_cmd_tick();
}
int main(void){
 /* Eight full-sized replay results still fit; payload identity must not keep
  * another 512-byte JSON copy per entry. */
 assert(sizeof(s_cache)<=5u*1024u && CACHE_SIZE==8u);
 unsigned char digest[32];
 static const unsigned char abc_sha256[32]={
  0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
  0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad};
 assert(command_sha256((const unsigned char *)"abc",3,digest,0)==0 && !memcmp(digest,abc_sha256,32));
 ble_ct_cmd_set_executors(write_param,read_param);
 request("legacy-write","set_param","{\"param_id\":\"set_output_voltage\",\"value\":230}",false);
 assert(writes==0 && strstr(result,"UPDATE_APP_REQUIRED"));
#ifdef P0_API
 request("power","power_on","{}",false);assert(writes==0 && strstr(result,"UNSUPPORTED_ACTION"));
 mtu=23;request("open","open_session","{\"framing_v1\":true,\"direct_control_v1\":true}",true);
 assert(framed && strstr(result,"applied") && strstr(result,"direct_control_v1\":true"));
 CmdCache cache_before[CACHE_SIZE];memcpy(cache_before,s_cache,sizeof(s_cache));
 unsigned hashes_before=real_hash_calls;fail_hash=true;
 request("hash-fail","set_param","{\"param_id\":\"set_output_voltage\",\"value\":230}",true);
 assert(writes==0 && !s_pending.active && !memcmp(cache_before,s_cache,sizeof(s_cache)));
 assert(strstr(result,"RESULT_UNAVAILABLE") && real_hash_calls==hashes_before);fail_hash=false;
 request("write-1","set_param","{\"param_id\":\"set_output_voltage\",\"value\":230}",true);
 assert(writes==1 && strstr(result,"accepted"));
 uint32_t token=ble_ct_cmd_write_token();
 ble_ct_cmd_on_write_saved(token,"set_output_voltage",230,5);
 assert(strstr(result,"\"status\":\"saved\"") && strstr(result,"\"observed\":false") && !strstr(result,"\"applied\""));
 request("write-1","set_param","{\"value\":230,\"param_id\":\"set_output_voltage\"}",true);
 assert(writes==1 && strstr(result,"saved"));
 strcpy(session,"session-2");ble_ct_cmd_reset_connection();framed=false;
 request("open-2","open_session","{\"framing_v1\":true,\"direct_control_v1\":true}",true);
 request("write-1","set_param","{\"param_id\":\"set_output_voltage\",\"value\":230}",true);
 assert(writes==1 && strstr(result,"saved") && strstr(result,"session-2"));
 request("write-1","set_param","{\"param_id\":\"set_output_voltage\",\"value\":240}",true);
 assert(writes==1 && strstr(result,"IDEMPOTENCY_CONFLICT"));
 request("write-2","set_param","{\"param_id\":\"set_output_voltage\",\"value\":240}",true);
 assert(writes==2);uint32_t stale=ble_ct_cmd_write_token();clock_ms+=31000;ble_ct_cmd_tick();assert(strstr(result,"SYNC_TIMEOUT"));
 ble_ct_cmd_on_write_saved(stale,"set_output_voltage",240,6);assert(strstr(result,"SYNC_TIMEOUT"));
 request("write-3","set_param","{\"param_id\":\"set_output_voltage\",\"value\":250}",true);
 assert(writes==3);ble_ct_cmd_on_write_saved(stale,"set_output_voltage",240,6);assert(strstr(result,"accepted"));
 ble_ct_cmd_on_write_ack(ble_ct_cmd_write_token(),"set_output_voltage",-2);assert(strstr(result,"STORAGE_FAILED"));
 unsigned char partial[]={0x80,'{'};assert(submit(partial,sizeof(partial))==0);clock_ms+=6000;
 unsigned char last[]={0x41,'}'};assert(submit(last,sizeof(last))!=0);
 unsigned char nul[]={0xc0,'{',0,'}'};assert(submit(nul,sizeof(nul))!=0);
 request("get","get_param","{\"param_id\":\"set_output_voltage\"}",true);assert(strstr(result,"230"));
 synchronous=true;
 request("sync-complete","set_param","{\"param_id\":\"set_output_voltage\",\"value\":230}",true);
 assert(writes==4 && strstr(result,"saved"));
 request("sync-complete","set_param","{\"param_id\":\"set_output_voltage\",\"value\":230}",true);
 assert(writes==4 && strstr(result,"saved"));synchronous=false;
 request("disconnect-write","set_param","{\"param_id\":\"set_output_voltage\",\"value\":230}",true);
 uint32_t accepted_token=ble_ct_cmd_write_token();unsigned before=notifications;
 ble_ct_cmd_reset_connection();strcpy(session,"session-3");
 ble_ct_cmd_on_write_saved(accepted_token,"set_output_voltage",230,9);assert(notifications==before);
 request("open-3","open_session","{\"framing_v1\":true,\"direct_control_v1\":true}",true);
 request("disconnect-write","set_param","{\"param_id\":\"set_output_voltage\",\"value\":230}",true);
 assert(writes==5 && strstr(result,"saved") && strstr(result,"session-3"));
 request("sync-failure","set_param","{\"param_id\":\"set_output_voltage\",\"value\":230}",true);
 ble_ct_cmd_on_write_ack(ble_ct_cmd_write_token(),"set_output_voltage",-1);
 assert(strstr(result,"SYNC_TIMEOUT") && strstr(result,"\"saved\":true") && strstr(result,"\"submitted\":false"));
 before=notifications;fail_notification=before+1;ble_ct_cmd_notify_result("{\"long\":\"abcdefghijklmnopqrstuvwxyz\"}",37);
 assert(notifications==before+1);fail_notification=0;ble_ct_frames_reset(&output);
 /* Deep JSON must be rejected before cJSON can exhaust the controller stack. */
 char deep[80]="{\"p\":";memset(deep+5,'[',30);memset(deep+35,']',30);deep[65]='}';deep[66]=0;
 assert(submit(deep,66)!=0);
 /* Real App token lengths: sid32/message24/operation24/command8. */
 strcpy(session,"0123456789abcdef0123456789abcdef");ble_ct_cmd_reset_connection();real_tokens=true;
 request("12345678","open_session","{\"framing_v1\":true,\"direct_control_v1\":true}",true);
 assert(framed && strstr(result,"applied"));
 unsigned previous_writes=writes;
 request("89abcdef","set_param","{\"param_id\":\"set_output_voltage\",\"value\":230}",true);
 assert(writes==previous_writes+1 && strstr(result,"accepted"));
 ble_ct_cmd_on_write_saved(ble_ct_cmd_write_token(),"set_output_voltage",230,10);
 assert(strstr(result,"saved") && strlen(result)<=512);
 ble_ct_cmd_reset_connection();mtu=512;wall_clock=1000;expired_timestamp=true;
 request("expired1","open_session","{\"framing_v1\":true,\"direct_control_v1\":true}",true);
 assert(!framed && !s_direct_control && strstr(result,"TIMESTAMP_OUT_OF_RANGE"));
 wall_clock=0;expired_timestamp=false;
 request("oom-open","open_session","{\"framing_v1\":true,\"direct_control_v1\":true}",true);
 cJSON_Hooks hooks={failing_malloc,free};cJSON_InitHooks(&hooks);
 bool completed_sweep=false;
 for(unsigned fail=1;fail<400;fail++){
  memset(s_cache,0,sizeof(s_cache));s_pending.active=false;
  allocation_index=0;allocation_failed=false;fail_allocation=fail;unsigned start_writes=writes;
  result[0]=0;
  request("oomwrite","set_param","{\"param_id\":\"set_output_voltage\",\"value\":230}",true);
  if(allocation_failed)assert(writes==start_writes);
  else {assert(writes==start_writes+1 && strstr(result,"accepted") && strstr(result,"operation_id") && strstr(result,"control.result"));completed_sweep=true;break;}
 }
 assert(completed_sweep);completed_sweep=false;
 for(unsigned fail=1;fail<400;fail++){
  memset(s_cache,0,sizeof(s_cache));s_pending.active=false;ble_ct_cmd_reset_connection();
  allocation_index=0;allocation_failed=false;fail_allocation=fail;
  request("oomopen1","open_session","{\"framing_v1\":true,\"direct_control_v1\":true}",true);
  if(allocation_failed)assert(!framed && !s_direct_control);
  else {assert(framed && s_direct_control && strstr(result,"applied"));completed_sweep=true;break;}
 }
 assert(completed_sweep);
 cJSON_InitHooks(NULL);s_pending.active=false;
 memset(s_cache,0,sizeof(s_cache));s_cache_next=0;
 /* A pending result is protected even if reads wrap the entire replay ring. */
 previous_writes=writes;
 request("pending1","set_param","{\"param_id\":\"set_output_voltage\",\"value\":230}",true);
 accepted_token=ble_ct_cmd_write_token();
 request("pending1","set_param","{\"value\":230,\"param_id\":\"set_output_voltage\"}",true);
 assert(writes==previous_writes+1 && strstr(result,"accepted") && ble_ct_cmd_write_token()==accepted_token);
 for(unsigned i=0;i<20;i++){
  char id[16];snprintf(id,sizeof(id),"read-%u",i);
  request(id,"get_param","{\"param_id\":\"set_output_voltage\"}",true);
 }
 ble_ct_cmd_on_write_saved(accepted_token,"set_output_voltage",230,11);
 request("pending1","set_param","{\"value\":230,\"param_id\":\"set_output_voltage\"}",true);
 assert(writes==previous_writes+1 && strstr(result,"saved"));
 /* Retain the full eight-entry depth, then expire by the original ten-minute
  * TTL so a command can execute again after its replay window. */
 memset(s_cache,0,sizeof(s_cache));s_cache_next=0;synchronous=true;previous_writes=writes;
 for(unsigned pass=0;pass<2;pass++)for(unsigned i=0;i<8;i++){
  char id[16];snprintf(id,sizeof(id),"depth-%u",i);
  request(id,"set_param","{\"param_id\":\"set_output_voltage\",\"value\":230}",true);
  assert(strstr(result,"saved"));
 }
 assert(writes==previous_writes+8);
 clock_ms+=10u*60u*1000u+1u;
 request("depth-0","set_param","{\"param_id\":\"set_output_voltage\",\"value\":230}",true);
 assert(writes==previous_writes+9 && strstr(result,"saved"));synchronous=false;
 request("nested1","get_param","{\"param_id\":\"set_output_voltage\",\"meta\":{\"z\":[1,2],\"a\":{\"q\":true}}}",true);
 request("nested1","get_param","{\"meta\":{\"a\":{\"q\":true},\"z\":[1,2]},\"param_id\":\"set_output_voltage\"}",true);
 assert(strstr(result,"applied"));
 request("nested1","get_param","{\"meta\":{\"a\":{\"q\":true},\"z\":[2,1]},\"param_id\":\"set_output_voltage\"}",true);
 assert(strstr(result,"IDEMPOTENCY_CONFLICT"));
 memset(s_cache,0,sizeof(s_cache));
 for(int rc=5;rc<=7;rc++){
  char id[20];snprintf(id,sizeof(id),"reject-%d",rc);executor_result=rc;
  request(id,"set_param","{\"param_id\":\"set_output_voltage\",\"value\":230}",true);
  assert(!s_pending.active && strstr(result,rc==5?"UNSUPPORTED_PARAM":rc==6?"UPGRADE_BUSY":"NOT_READY"));
 }
 executor_result=0;
 request("savefail","set_param","{\"param_id\":\"set_output_voltage\",\"value\":230}",true);
 ble_ct_cmd_on_write_ack(ble_ct_cmd_write_token(),"set_output_voltage",-20);
 assert(strstr(result,"STORAGE_FAILED") && strstr(result,"\"saved\":false"));
 request("submitfail","set_param","{\"param_id\":\"set_output_voltage\",\"value\":230}",true);
 ble_ct_cmd_on_write_ack(ble_ct_cmd_write_token(),"set_output_voltage",-21);
 assert(strstr(result,"SUBMIT_FAILED") && strstr(result,"\"saved\":true") && strstr(result,"\"submitted\":false"));
 assert(real_hash_calls>1);
 printf("PASS actual dispatcher: cache=%zu bytes, real SHA256, eight-result TTL replay, pending protection, canonical payload/conflict, allocation failures, MTU23, saved truth, reconnect\n",sizeof(s_cache));
#endif
}
'''.replace('SOURCE', source)
if 'ble_ct_cmd_tick' in api:
    harness = '#define P0_API 1\n' + harness
else:
    harness = '#define ble_ct_cmd_tick() ((void)0)\n' + harness
target=ROOT/'tmp/ble_control_flow_test.c'
target.parent.mkdir(exist_ok=True)
target.write_text(harness,encoding='utf-8')
config=target.parent/'ble_sha256_host_config.h'
config.write_text('#define MBEDTLS_SHA256_C\n#define MBEDTLS_PLATFORM_C\n',encoding='utf-8')
exe=target.with_suffix('.exe')
subprocess.run([str(GCC),'-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-unused-variable','-Wno-unused-parameter',
    '-I'+str(JSON),'-Icomponents/ble_ct','-I'+str(MBEDTLS/'include'),'-I'+str(target.parent),
    '-DMBEDTLS_CONFIG_FILE="ble_sha256_host_config.h"',str(target),'components/ble_ct/ble_ct_frames.c',str(JSON/'cJSON.c'),
    str(MBEDTLS/'library/sha256.c'),str(MBEDTLS/'library/platform_util.c'),str(MBEDTLS/'library/platform.c'),'-o',str(exe)],cwd=ROOT,check=True)
subprocess.run([str(exe)],check=True)
