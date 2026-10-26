"""Exercise the real reservation and NVS task journal across simulated reboot."""
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
gcc = Path(r'C:\Users\29538\AppData\Local\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin\gcc.exe')

def function(text, signature):
    start = text.index(signature)
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]

storage = (root/'main/storage/nvs_storage.c').read_text(encoding='utf-8')
service = (root/'main/ota/ota_service.c').read_text(encoding='utf-8')
# Before implementation, a stub lets the new reservation invariant fail first.
claim = (function(storage, 'bool storage_claim_direct_ota_task(')
         if 'bool storage_claim_direct_ota_task(' in storage else
         'bool storage_claim_direct_ota_task(const char*t,const char*i,bool*d){(void)t;(void)i;*d=false;return true;}')
code = r'''
#include "ota_service.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdatomic.h>
#define pdTRUE 1
#define portMAX_DELAY 1
#define OTA_NAMESPACE "ota_sec"
#define NVS_READWRITE 1
#define ESP_OK 0
#define ESP_ERR_NVS_NOT_FOUND 1
typedef int esp_err_t;
typedef int nvs_handle_t;
static unsigned char committed[2][1024],pending[1024];
static size_t sizes[2],pending_size;
static unsigned sets,commits,notifications;
static int fail,slot;
int nvs_open(const char*n,int mode,nvs_handle_t*h){assert(!strcmp(n,"ota_sec")&&mode==1);*h=7;return fail==1?-9:0;}
static int key_slot(const char*k){assert(!strcmp(k,"cmd_dsp")||!strcmp(k,"cmd_bms"));return !strcmp(k,"cmd_bms");}
int nvs_get_blob(int h,const char*k,void*p,size_t*n){assert(h==7);slot=key_slot(k);if(fail==2)return -9;if(!sizes[slot])return 1;assert(*n>=sizes[slot]);memcpy(p,committed[slot],sizes[slot]);*n=sizes[slot];return 0;}
int nvs_set_blob(int h,const char*k,const void*p,size_t n){assert(h==7&&n<=1024);sets++;slot=key_slot(k);memcpy(pending,p,n);pending_size=n;return fail==3?-9:0;}
int nvs_commit(int h){assert(h==7);commits++;if(fail==4)return -9;memcpy(committed[slot],pending,pending_size);sizes[slot]=pending_size;return 0;}
void nvs_close(int h){assert(h==7);}
CLAIM
static int g_lock=1;
static bool locked,g_admin_busy,g_active,g_remote_cleanup_busy;
static OtaManifest g_manifest;
static OtaSnapshot g_snapshot;
static atomic_bool g_cancel_requested;
static int xSemaphoreTake(int h,unsigned t){(void)t;assert(h&&!locked);locked=true;return pdTRUE;}
static void xSemaphoreGive(int h){assert(h&&locked);locked=false;}
static size_t strlcpy(char*d,const char*s,size_t n){size_t l=strlen(s);if(n){size_t c=l<n-1?l:n-1;memcpy(d,s,c);d[c]=0;}return l;}
const char *ota_target_name(OtaTarget t){return t==OTA_TARGET_DSP?"dsp":"bms";}
static void disable_wifi_ps(void){}
static void notify_snapshot(const OtaSnapshot*s){assert(s&&g_active&&!locked);notifications++;}
RESERVE
static void reboot(void){assert(!locked);g_active=g_admin_busy=g_remote_cleanup_busy=false;memset(&g_snapshot,0,sizeof(g_snapshot));memset(&g_manifest,0,sizeof(g_manifest));}
int main(void){
 OtaManifest m={.target=OTA_TARGET_DSP,.file_size=45928};strcpy(m.task_id,"task-A");
 assert(reserve_manifest(&m)==OTA_RESULT_OK&&g_active&&notifications==1);
 g_active=false; /* success, failure and cancellation all finish the same accepted task */
 unsigned before=notifications;
 assert(reserve_manifest(&m)==OTA_RESULT_TASK_REPLAY_BLOCKED&&!g_active&&notifications==before);
 reboot();assert(reserve_manifest(&m)==OTA_RESULT_TASK_REPLAY_BLOCKED&&!g_active);
 strcpy(m.task_id,"task-B");assert(reserve_manifest(&m)==OTA_RESULT_OK);reboot();
 strcpy(m.task_id,"task-A");assert(reserve_manifest(&m)==OTA_RESULT_TASK_REPLAY_BLOCKED);
 m.target=OTA_TARGET_BMS;assert(reserve_manifest(&m)==OTA_RESULT_OK);reboot();
 for(int f=1;f<=4;f++){
  fail=f;strcpy(m.task_id,"new-task");unsigned n=notifications;
  assert(reserve_manifest(&m)==OTA_RESULT_PERSIST_FAILED&&!g_active&&notifications==n);reboot();
 }
 fail=0;
 assert(reserve_manifest(&m)==OTA_RESULT_OK);reboot();
 assert(reserve_manifest(&m)==OTA_RESULT_TASK_REPLAY_BLOCKED);
 /* Eight remembered commands survive wrap and restart; duplicates do not write NVS. */
 memset(committed,0,sizeof(committed));memset(sizes,0,sizeof(sizes));reboot();
 m.target=OTA_TARGET_DSP;
 for(unsigned i=0;i<10;i++){
  snprintf(m.task_id,sizeof(m.task_id),"cycle-%u",i);
  assert(reserve_manifest(&m)==OTA_RESULT_OK);reboot();
 }
 unsigned before_sets=sets;
 for(unsigned i=2;i<10;i++){
  snprintf(m.task_id,sizeof(m.task_id),"cycle-%u",i);
  assert(reserve_manifest(&m)==OTA_RESULT_TASK_REPLAY_BLOCKED);reboot();
 }
 assert(sets==before_sets);
 /* ESP self-update remains outside this DSP/BMS history. */
 m.target=OTA_TARGET_ESP;assert(reserve_manifest(&m)==OTA_RESULT_OK);reboot();
 bool duplicate=false;
 assert(!storage_claim_direct_ota_task("esp","bad",&duplicate));
 assert(!storage_claim_direct_ota_task("dsp","",&duplicate));
 sizes[0]=1;committed[0][0]=0; /* Corrupt record cannot silently authorize a reflash. */
 m.target=OTA_TARGET_DSP;strcpy(m.task_id,"task-C");
 assert(reserve_manifest(&m)==OTA_RESULT_PERSIST_FAILED&&!g_active);
 assert(sets&&commits);
 puts("PASS real OTA reservation+NVS: task accepted once, blocked after completion/reboot, older recent task blocked, per-target isolation, storage failures/corruption fail closed before work");
}
'''.replace('CLAIM',claim).replace('RESERVE',function(service,'static OtaResult reserve_manifest('))
target=root/'tmp/direct_ota_replay_test.c'
target.write_text(code,encoding='utf-8')
exe=target.with_suffix('.exe')
subprocess.run([str(gcc),'-std=c11','-Wall','-Wextra','-Werror','-Imain/ota',str(target),'-o',str(exe)],cwd=root,check=True)
subprocess.run([str(exe)],cwd=root,check=True)
