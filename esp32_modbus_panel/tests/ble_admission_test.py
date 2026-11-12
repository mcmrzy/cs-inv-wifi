"""Execute the full-panel application's BLE/cloud decisions with fake SDK calls.

This checks admission and lifecycle ownership, not reclaimed radio heap on board.
"""
from pathlib import Path
import os
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'full/app_main.c').read_text(encoding='utf-8')


def function(name):
    match = re.search(r'^(?:static\s+)?(?:void|bool)\s+' + name + r'\([^;]*?\)\s*\{', source, re.M)
    assert match, name
    end = source.index('{', match.start()) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end]


declarations = source[source.index('static TaskHandle_t s_main_loop_handle;'):
                      source.index('static void indicators_tick(')]
harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
typedef void *TaskHandle_t;
#define MALLOC_CAP_8BIT 1
#define MALLOC_CAP_INTERNAL 2
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define WIFI_DEFAULT_SSID ""
DECLARATIONS
static uint32_t clock_ms=20000;
static bool cloud=true,handshake,configured=true,peer,ota,admin,paused;
static bool failed,live,suspended,permit;
static unsigned starts,restores,ota_restores,cleanups,pauses,stops;
static unsigned free_bytes=44752,largest=27648;
static uint32_t now_ms(void){return clock_ms;}
static bool mqtt_is_connected(void){return cloud;}
static bool mqtt_is_connecting(void){return handshake;}
static bool mqtt_is_paused_for_local(void){return paused;}
static void mqtt_poll_connection(void){}
static void mqtt_set_connect_allowed(bool p){permit=p;}
static bool weather_busy;
static bool panel_weather_network_busy(void){return weather_busy;}
static bool storage_has_wifi_config(void){return configured;}
static bool ota_service_is_busy(void){return ota;}
static bool ota_service_admin_begin(void){if(ota||admin)return false;admin=true;return true;}
static void ota_service_admin_end(void){assert(admin);admin=false;}
static bool mqtt_pause_for_local(void){assert(admin);pauses++;paused=true;cloud=false;permit=false;return true;}
static bool mqtt_resume_after_local(void){assert(admin&&!live);paused=false;return true;}
static bool ble_prov_is_connected(void){return peer;}
static bool ble_prov_is_ready(void){return live;}
static bool ble_prov_is_suspended(void){return suspended;}
static bool ble_prov_start_failed(void){return failed;}
static bool ble_prov_recover_start_failure(void){assert(admin&&!suspended);cleanups++;failed=false;live=false;return true;}
static bool ble_prov_restore_after_ota(void){assert(admin);ota_restores++;suspended=false;failed=false;live=true;return true;}
static bool ble_prov_suspend_for_mqtt(void){assert(admin&&!peer);stops++;live=false;suspended=true;return true;}
static bool ble_prov_restore_after_mqtt(void){assert(admin);restores++;live=true;suspended=false;return true;}
static bool start_ble_services(void){assert(admin);starts++;live=true;return true;}
static unsigned esp_get_free_heap_size(void){return free_bytes;}
static unsigned heap_caps_get_free_size(int caps){(void)caps;return free_bytes;}
static unsigned heap_caps_get_largest_free_block(int caps){(void)caps;return largest;}
static unsigned deleted_tasks;
static void vTaskDelete(TaskHandle_t task){assert(task);deleted_tasks++;}
FUNCTIONS
int main(void){
 s_ble_start_deadline_ms=10000;
 /* Reproduce the user's 44752/27648 cloud-online state for two minutes. */
 for(unsigned i=0;i<120;i++){startup_deferred_tick();clock_ms+=1000;}
 assert(starts==0&&restores==0&&pauses==0&&cloud&&permit);
 /* A failed host is cleaned once; low memory does not trigger another start. */
 failed=true;startup_deferred_tick();assert(cleanups==1&&!failed);
 clock_ms+=BLE_START_RETRY_MS;startup_deferred_tick();assert(starts==0&&cloud&&permit);
 /* User enters Phone connection: release cloud memory before BLE starts. */
 app_request_local_ble(true);startup_deferred_tick();assert(paused&&s_local_ble_mode&&starts==0&&!permit);
 free_bytes=90000;largest=50000;clock_ms+=STARTUP_STACK_SETTLE_MS;
 startup_deferred_tick();assert(starts==1&&live&&paused&&!cloud);
 /* Preserve a connected phone even after closing the menu. */
 app_request_local_ble(false);peer=true;clock_ms+=60000;startup_deferred_tick();
 assert(live&&stops==0&&paused);
 peer=false;clock_ms+=LOCAL_BLE_IDLE_MS+1;startup_deferred_tick();assert(stops==1&&!live);
 clock_ms+=STARTUP_STACK_SETTLE_MS;startup_deferred_tick();assert(!paused&&permit);
 /* An already suspended host also needs the low-memory fallback on request. */
 cloud=true;free_bytes=44752;largest=27648;
 startup_deferred_tick();assert(restores==0&&cloud);
 app_request_local_ble(true);startup_deferred_tick();assert(paused&&s_local_ble_mode);
 free_bytes=90000;largest=50000;clock_ms+=STARTUP_STACK_SETTLE_MS;
 startup_deferred_tick();assert(restores==1&&live&&paused);
 /* OTA recovery retains its owner when deferred; explicit request frees cloud. */
 app_request_local_ble(false);s_local_ble_mode=false;paused=false;cloud=true;
 suspended=true;s_ble_suspended_for_mqtt=false;failed=true;live=false;
 free_bytes=44752;largest=27648;startup_deferred_tick();assert(ota_restores==0&&failed&&suspended);
 app_request_local_ble(true);startup_deferred_tick();assert(paused&&s_local_ble_mode);
 free_bytes=90000;largest=50000;clock_ms+=STARTUP_STACK_SETTLE_MS;
 startup_deferred_tick();assert(ota_restores==1&&!failed&&live);
 /* Sufficient memory still admits normal simultaneous cloud/BLE operation. */
 app_request_local_ble(false);s_local_ble_mode=false;paused=false;cloud=true;
 suspended=false;failed=false;live=false;s_ble_services_started=false;
 clock_ms+=BLE_START_RETRY_MS;startup_deferred_tick();
 assert(starts==2&&live&&cloud&&!paused&&permit);
 clock_ms+=60000;startup_deferred_tick();assert(starts==2&&live&&cloud);
 /* A pending weather TLS admission must release the parked startup stack,
  * while preventing both MQTT handshakes and BLE lifecycle transitions. */
 weather_busy=true;s_startup_owner=(TaskHandle_t)1;
 atomic_store(&s_startup_health_done,true);s_startup_release_ms=clock_ms;
 startup_deferred_tick();assert(deleted_tasks==1&&!s_startup_owner&&!permit&&starts==2);
 weather_busy=false;
 /* BLE scheduling backoff must not close an already granted cloud window. */
 permit=true;s_next_ble_start_ms=clock_ms+BLE_START_RETRY_MS;
 startup_deferred_tick();assert(permit);
 clock_ms+=BLE_START_RETRY_MS;
 /* Reproduce the field's 44148/18432 AFTER MQTT has already been released.
  * BLE cannot start; cloud must resume without the 30s transport oscillation. */
 app_request_local_ble(false);s_local_ble_mode=false;paused=false;cloud=false;
 live=false;suspended=false;failed=false;s_ble_services_started=false;s_ble_suspended_for_mqtt=false;
 free_bytes=44148;largest=18432;s_next_ble_start_ms=clock_ms;s_cloud_retry_deadline_ms=0;
 unsigned old_starts=starts,old_pauses=pauses;
 startup_deferred_tick();assert(paused&&s_local_ble_mode);
 clock_ms+=STARTUP_STACK_SETTLE_MS;startup_deferred_tick();
 assert(!paused&&!s_local_ble_mode&&permit&&starts==old_starts);
 cloud=true;
 for(unsigned i=0;i<120;++i){clock_ms+=1000;startup_deferred_tick();}
 assert(permit&&!paused&&pauses==old_pauses+1&&starts==old_starts);
 /* A fresh explicit user request may retry, then safely restore cloud again. */
 app_request_local_ble(true);startup_deferred_tick();assert(paused&&s_local_ble_mode);
 clock_ms+=STARTUP_STACK_SETTLE_MS;startup_deferred_tick();
 assert(!paused&&!s_local_ble_mode&&permit&&starts==old_starts);
 for(unsigned i=0;i<120;++i){clock_ms+=1000;startup_deferred_tick();}
 assert(permit&&!paused&&pauses==old_pauses+2&&starts==old_starts);
 app_request_local_ble(false);
 puts("PASS BLE memory admission: quiet cloud, one failed-host cleanup, explicit phone request releases MQTT, suspended-host fallback, peer protection, OTA ownership");
}
'''.replace('DECLARATIONS', declarations).replace('FUNCTIONS', '\n'.join(
    function(name) for name in ('app_request_local_ble', 'local_ble_mode_tick', 'startup_deferred_tick')))
folder = ROOT / 'tmp'
folder.mkdir(exist_ok=True)
cfile, exe = folder / 'ble_admission_test.c', folder / 'ble_admission_test.exe'
cfile.write_text(harness, encoding='utf-8')
env = os.environ.copy()
env['PATH'] = str(Path(sys.argv[1]).parent) + os.pathsep + env.get('PATH', '')
subprocess.run([sys.argv[1], '-std=c11', '-Wall', '-Wextra', '-Werror',
                '-Wno-unused-function', '-Wno-unused-variable', str(cfile), '-o', str(exe)],
               check=True, env=env, timeout=30)
result = subprocess.run([str(exe)], capture_output=True, text=True, env=env, timeout=10)
assert result.returncode == 0, result.stdout + result.stderr
print(result.stdout, end='')
