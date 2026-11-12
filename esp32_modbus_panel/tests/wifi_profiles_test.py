"""Execute production profile storage and retry/provision workers with fake NVS/radio."""
from pathlib import Path
import re
import subprocess
import sys

root=Path(__file__).resolve().parents[1]
storage=(root.parent/'esp32_direct/main/storage/nvs_storage.c').read_text(encoding='utf-8')
storage=storage[storage.index('#ifdef PANEL_FULL_DIRECT'):storage.index('bool storage_get_mqtt(')]
app=(root/'full/app_main.c').read_text(encoding='utf-8')
def function(name):
    m=re.search(r'^static (?:void|bool) '+name+r'\([^;]*?\)\s*\{',app,re.M)
    assert m,name
    end=app.index('{',m.start())+1
    depth=1
    while depth:
        depth+=(app[end]=='{')-(app[end]=='}');end+=1
    return app[m.start():end]

harness=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "panel_wifi.h"
#define PANEL_FULL_DIRECT 1
#define KEY_WIFI_SSID "ssid"
#define KEY_WIFI_PASS "pass"
#define ESP_OK 0
#define ESP_ERR_NVS_NOT_FOUND 1
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define WIFI_DEFAULT_SSID ""
#define WIFI_DEFAULT_PASS ""
#define WIFI_CONNECT_TIMEOUT_MS 30000U
#define PROV_WIFI_TIMEOUT_MS 30000U
#define WIFI_REASON_NO_AP_FOUND 201
typedef int esp_err_t;
static int s_nvs_handle,set_fail,commit_fail;
static unsigned commits;
static bool blob_exists,legacy_exists;
static size_t flash_size;
static unsigned char flash_blob[294],pending_blob[294];
static char legacy_ssid[33]="legacy",legacy_pass[65]="old-pass";
static int nvs_get_blob(int h,const char *key,void *out,size_t *size){
 (void)h;assert(!strcmp(key,"wifi_profiles1"));
 if(!blob_exists)return 1;
 if(!out){*size=flash_size;return 0;}
 if(*size<flash_size)return 2;
 memcpy(out,flash_blob,flash_size);*size=flash_size;return 0;
}
static int nvs_set_blob(int h,const char *key,const void *in,size_t size){
 (void)h;assert(!strcmp(key,"wifi_profiles1")&&size==sizeof(pending_blob));
 if(set_fail)return 2;memcpy(pending_blob,in,size);return 0;
}
static int nvs_get_str(int h,const char *key,char *out,size_t *size){
 (void)h;if(!legacy_exists)return 1;
 const char *value=!strcmp(key,KEY_WIFI_SSID)?legacy_ssid:legacy_pass;
 size_t needed=strlen(value)+1;
 if(out){if(*size<needed)return 2;memcpy(out,value,needed);}
 *size=needed;return 0;
}
static int nvs_erase_key(int h,const char *key){
 (void)h;if(!strcmp(key,"wifi_profiles1"))blob_exists=false;
 else if(!strcmp(key,KEY_WIFI_SSID))legacy_exists=false;
 return 0;
}
static int nvs_commit(int h){
 (void)h;commits++;if(commit_fail)return 2;
 memcpy(flash_blob,pending_blob,sizeof(flash_blob));flash_size=sizeof(flash_blob);
 /* Erase-only commit does not recreate the removed key. */
 return 0;
}
STORAGE
static char chosen[33],chosen_pass[65];
static unsigned connects,deletes,disconnects;
static uint32_t clock_ms=1000,s_state_enter_ms;
static atomic_uint s_wifi_attempt_ms,s_panel_wifi_status;
static atomic_bool s_prov_busy;
static unsigned s_wifi_profile;
static char s_prov_ssid[33],s_prov_pass[65];
static bool s_prov_panel,radio_ok=true,got_ip;
static unsigned disconnect_reason;
static uint32_t now_ms(void){return clock_ms;}
static size_t strlcpy(char *d,const char *s,size_t n){size_t l=strlen(s);if(n){size_t c=l<n-1?l:n-1;memcpy(d,s,c);d[c]=0;}return l;}
static bool wifi_sta_connect(const char *s,const char *p){
 connects++;strlcpy(chosen,s,sizeof(chosen));strlcpy(chosen_pass,p,sizeof(chosen_pass));return radio_ok;
}
static bool wifi_sta_wait_connected(unsigned timeout){assert(timeout==30000);clock_ms+=got_ip?500:30000;return got_ip;}
static unsigned wifi_sta_get_last_disconnect_reason(void){return disconnect_reason;}
static void wifi_sta_force_disconnect(void){disconnects++;}
static void ble_prov_notify_status(const char *s){(void)s;}
static void vTaskDelete(void *t){assert(!t);deletes++;}
static void setup_wifi(void);
FUNCTIONS
static void saved(unsigned i,const char *name,const char *password){
 char s[33],p[65];assert(storage_get_wifi_at(i,s,sizeof(s),p,sizeof(p)));
 assert(!strcmp(s,name)&&!strcmp(p,password));
}
static void save(const char *s,const char *p){
 assert(storage_set_wifi(s,p));blob_exists=true;
}
int main(void){
 char s[33],p[65];assert(!storage_has_wifi_config());
 legacy_exists=true;assert(storage_has_wifi_config());saved(0,"legacy","old-pass");
 assert(!storage_get_wifi_at(1,s,sizeof(s),p,sizeof(p)));
 save("A","password-A");saved(0,"A","password-A");saved(1,"legacy","old-pass");
 save("B","password-B");save("C","password-C");
 saved(0,"C","password-C");saved(1,"B","password-B");saved(2,"A","password-A");
 save("D","password-D");saved(2,"B","password-B");
 save("C","updated-C");saved(0,"C","updated-C");saved(1,"D","password-D");saved(2,"B","password-B");
 unsigned writes=commits;save("C","updated-C");assert(commits==writes);
 unsigned char before[294];memcpy(before,flash_blob,sizeof(before));
 set_fail=1;assert(!storage_set_wifi("bad","password"));set_fail=0;
 commit_fail=1;assert(!storage_set_wifi("bad","password"));commit_fail=0;
 assert(!memcmp(before,flash_blob,sizeof(before)));saved(0,"C","updated-C");
 assert(!storage_get_wifi_at(3,s,sizeof(s),p,sizeof(p)));
 assert(!storage_get_wifi_at(0,s,1,p,sizeof(p)));
 /* Restart begins with the newest successfully saved network; wrap all three. */
 assert(start_saved_wifi(false)&&!strcmp(chosen,"C"));s_state_enter_ms=clock_ms;
 clock_ms+=29999;retry_saved_wifi();assert(connects==1);
 clock_ms++;retry_saved_wifi();assert(connects==2&&!strcmp(chosen,"D"));
 clock_ms+=30000;retry_saved_wifi();assert(connects==3&&!strcmp(chosen,"B"));
 clock_ms+=30000;retry_saved_wifi();assert(connects==4&&!strcmp(chosen,"C"));
 /* Scanning/provisioning owns the radio, even when the fallback deadline expires. */
 atomic_store(&s_prov_busy,true);clock_ms+=30000;retry_saved_wifi();assert(connects==4);
 atomic_store(&s_prov_busy,false);retry_saved_wifi();assert(connects==5&&!strcmp(chosen,"D"));
 /* Failed manual setup restores the prior profile and gives it a full attempt. */
 strcpy(s_prov_ssid,"wrong");strcpy(s_prov_pass,"bad-pass");s_prov_panel=true;
 atomic_store(&s_prov_busy,true);got_ip=false;prov_connect_task(NULL);
 assert(atomic_load(&s_panel_wifi_status)==PANEL_WIFI_FAILED&&!atomic_load(&s_prov_busy));
 assert(!strcmp(chosen,"D")&&!s_prov_pass[0]);saved(0,"C","updated-C");
 writes=connects;retry_saved_wifi();assert(connects==writes);
 clock_ms+=30000;retry_saved_wifi();assert(connects==writes+1&&!strcmp(chosen,"B"));
 /* Only a real IP plus successful persistence adds a new profile. */
 strcpy(s_prov_ssid,"new");strcpy(s_prov_pass,"new-pass");got_ip=true;set_fail=1;
 atomic_store(&s_prov_busy,true);prov_connect_task(NULL);set_fail=0;
 assert(atomic_load(&s_panel_wifi_status)==PANEL_WIFI_SAVE_FAILED);saved(0,"C","updated-C");
 strcpy(s_prov_pass,"new-pass");atomic_store(&s_prov_busy,true);prov_connect_task(NULL);
 assert(atomic_load(&s_panel_wifi_status)==PANEL_WIFI_CONNECTED&&s_wifi_profile==0);
 saved(0,"new","new-pass");saved(1,"C","updated-C");saved(2,"D","password-D");
 /* Full-length SSID, raw 64-hex PSK and open networks survive reboot readback. */
 save("12345678901234567890123456789012","0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
 saved(0,"12345678901234567890123456789012","0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
 save("open","");saved(0,"open","");
 memset(flash_blob,'x',sizeof(flash_blob));legacy_exists=false;
 assert(!storage_has_wifi_config()&&!storage_get_wifi_at(0,s,sizeof(s),p,sizeof(p)));
 flash_size=1;assert(!storage_has_wifi_config());
 /* Clear removes all three and legacy keys rather than exposing an old AP. */
 save("clean","password");assert(storage_clear_wifi());assert(!storage_has_wifi_config());
 assert(deletes==3&&disconnects==2);
 puts("PASS Wi-Fi profiles: three slots, legacy migration, replacement, save failure, reboot, 30s failover, busy admission and provisioning recovery");
}
'''
harness=harness.replace('STORAGE',storage).replace('FUNCTIONS','\n'.join(function(n) for n in ['provision_report','prov_connect_task','setup_wifi','start_saved_wifi','retry_saved_wifi']))
path=root/'tmp/wifi_profiles_test.c';path.write_text(harness,encoding='utf-8')
exe=path.with_suffix('.exe')
subprocess.run([sys.argv[1],'-std=c11','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-I'+str(root/'full'),str(path),'-o',str(exe)],check=True)
subprocess.run([str(exe)],check=True)
