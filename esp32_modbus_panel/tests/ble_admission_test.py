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
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
#define ESP_LOGW(tag,fmt,...) do { (void)(tag); if (false) printf(fmt,__VA_ARGS__); } while(0)
static const char *TAG="app";
DECLARATIONS
static uint32_t clock_ms=20000;
static bool cloud,handshake,ota,admin,weather,failed,live,permit,wifi_available;
static bool ble_available=true;
static unsigned starts,wifi_attempts,wifi_connects,cleanups,deleted_tasks;
static uint32_t now_ms(void){return clock_ms;}
static bool mqtt_is_connected(void){return cloud;}
static bool mqtt_is_connecting(void){return handshake;}
static void mqtt_poll_connection(void){}
static void mqtt_set_connect_allowed(bool p){permit=p;}
static bool panel_weather_network_busy(void){return weather;}
static bool ota_service_is_busy(void){return ota;}
static bool ota_service_admin_begin(void){if(ota||admin)return false;admin=true;return true;}
static void ota_service_admin_end(void){assert(admin);admin=false;}
static bool ble_prov_is_ready(void){return live;}
static bool ble_prov_start_failed(void){return failed;}
static bool ble_prov_recover_start_failure(void){assert(admin);cleanups++;failed=false;live=false;return true;}
static bool start_ble_services(void){assert(admin);starts++;live=ble_available;return live;}
static bool wifi_sta_try_init(void){assert(starts>0);wifi_attempts++;return wifi_available;}
static bool start_saved_wifi(bool advance){assert(!advance&&!admin);wifi_connects++;return true;}
static void sntp_sync_init(void){assert(wifi_connects==1);}
static void vTaskDelete(TaskHandle_t task){assert(task);deleted_tasks++;}
static unsigned heap_caps_get_free_size(unsigned caps){(void)caps;return 0;}
static unsigned heap_caps_get_largest_free_block(unsigned caps){(void)caps;return 0;}
static unsigned heap_caps_get_minimum_free_size(unsigned caps){(void)caps;return 0;}
FUNCTION
int main(void){
 s_startup_owner=(TaskHandle_t)1;
 startup_deferred_tick();assert(!starts&&!wifi_attempts&&!deleted_tasks);
 atomic_store(&s_startup_health_done,true);
 startup_deferred_tick();assert(deleted_tasks==1&&!starts&&!wifi_attempts);
 clock_ms+=STARTUP_STACK_SETTLE_MS;
 startup_deferred_tick();assert(live&&starts==1&&wifi_attempts==1&&!wifi_connects);
 wifi_available=true;clock_ms+=BLE_START_RETRY_MS;
 startup_deferred_tick();assert(live&&starts==1&&wifi_attempts==2&&wifi_connects==1&&permit);
 cloud=true;clock_ms+=60000;startup_deferred_tick();
 assert(live&&permit&&starts==1&&wifi_connects==1);
 cloud=false;handshake=true;startup_deferred_tick();
 assert(live&&permit&&starts==1&&wifi_attempts==2);
 ota=true;handshake=false;clock_ms+=60000;startup_deferred_tick();
 assert(live&&permit&&!cleanups);
 ota=false;weather=true;startup_deferred_tick();assert(live&&!permit);
 weather=false;failed=true;live=false;
 startup_deferred_tick();assert(cleanups==1&&permit&&starts==1&&wifi_attempts==2);
 clock_ms+=BLE_START_RETRY_MS;startup_deferred_tick();
 assert(live&&starts==2&&wifi_attempts==2&&wifi_connects==1);
 /* Failed first BLE admission still starts WiFi, and retries without dropping it. */
 s_radios_started=s_ble_services_started=false;
 wifi_connects=0;ble_available=false;clock_ms+=BLE_START_RETRY_MS;
 startup_deferred_tick();assert(!live&&wifi_connects==1&&permit);
 ble_available=true;clock_ms+=BLE_START_RETRY_MS;
 startup_deferred_tick();assert(live&&wifi_connects==1&&permit);
 puts("PASS BLE memory admission: BLE-first boot, independent WiFi retry, resident BLE across reconnects and OTA, failed host recovery");
}
'''.replace('DECLARATIONS', declarations).replace('FUNCTION', function('startup_deferred_tick'))
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
