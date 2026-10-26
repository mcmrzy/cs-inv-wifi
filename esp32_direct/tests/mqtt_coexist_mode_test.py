"""Exercise actual main-loop coexistence decisions; SDK calls are boundaries."""
from pathlib import Path
import re
import unittest
from mqtt_connection_window_test import function
import mqtt_local_mode_test as local_mode

ROOT = Path(__file__).resolve().parents[1]


class CoexistTests(unittest.TestCase):
    def compile_scenario(self, scenario=None):
        app = (ROOT/'main/app/app_main.c').read_text(encoding='utf-8')
        declarations = app[app.index('static TaskHandle_t s_main_loop_handle;'):app.index('static uint32_t now_ms(void)')]
        code = r'''
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
DECLARATIONS
static uint32_t clock_ms=20000;
static bool connected=true,connecting,have_ip=true,configured=true,peer,ota,admin,paused;
static bool fail_stop,fail_start,ble_failed,ble_live,permit,ble_suspended;
static atomic_bool s_prov_busy;
static unsigned starts,stops,restores,ota_restores,pauses,resumes,free_bytes=90000,largest=50000;
static const char *TAG="test";
static uint32_t now_ms(void){return clock_ms;}
static void mqtt_poll_connection(void){}
static bool mqtt_is_connected(void){return connected;}
static bool mqtt_is_connecting(void){return connecting;}
static bool mqtt_is_paused_for_local(void){return paused;}
static void mqtt_set_connect_allowed(bool p){permit=p;}
static bool wifi_sta_is_connected(void){return have_ip;}
static bool storage_has_wifi_config(void){return configured;}
#define WIFI_DEFAULT_SSID ""
static bool ota_service_is_busy(void){return ota;}
static bool ota_service_admin_begin(void){if(ota||admin)return false;admin=true;return true;}
static void ota_service_admin_end(void){assert(admin);admin=false;}
static bool mqtt_pause_for_local(void){assert(admin);pauses++;paused=true;connected=false;return true;}
static bool mqtt_resume_after_local(void){assert(admin&&!ble_live);resumes++;paused=false;return true;}
static bool ble_prov_is_connected(void){return peer;}
static bool ble_prov_is_ready(void){return ble_live;}
static bool ble_prov_is_suspended(void){return ble_suspended;}
static bool ble_prov_start_failed(void){return ble_failed;}
static bool ble_prov_recover_start_failure(void){assert(admin);if(ble_suspended)return false;ble_failed=false;ble_live=false;return true;}
static bool ble_prov_restore_after_ota(void){assert(admin);ota_restores++;ble_suspended=false;ble_failed=false;ble_live=true;return true;}
static bool ble_prov_suspend_for_mqtt(void){assert(admin&&!peer);stops++;if(fail_stop)return false;ble_live=false;ble_suspended=true;return true;}
static bool ble_prov_restore_after_mqtt(void){assert(admin);restores++;ble_live=true;ble_suspended=false;return true;}
static bool start_ble_services(void){assert(admin);starts++;if(fail_start)return false;ble_live=true;return true;}
static unsigned esp_get_free_heap_size(void){return free_bytes;}
static unsigned heap_caps_get_free_size(int c){(void)c;return free_bytes;}
static unsigned heap_caps_get_largest_free_block(int c){(void)c;return largest;}
static void vTaskDelete(TaskHandle_t h){assert(h);}
FUNCTIONS
int main(void){
 s_ble_start_deadline_ms=10000;
 startup_deferred_tick();assert(starts==1&&ble_live&&connected&&!paused&&permit&&!pauses);
 clock_ms+=600000;startup_deferred_tick();assert(starts==1&&permit); /* Resident, no periodic teardown. */
 app_request_local_ble(true);startup_deferred_tick();assert(!pauses&&connected&&ble_live);
 app_request_local_ble(false);
 connected=false;peer=true;free_bytes=20000;largest=10000;
 startup_deferred_tick();assert(ble_live&&!stops&&!permit); /* Never sacrifice a phone. */
 free_bytes=40000;largest=20000;startup_deferred_tick();assert(permit&&ble_live&&!stops);
 peer=false;fail_stop=true;startup_deferred_tick();assert(ble_live&&!permit);
 fail_stop=false;clock_ms+=BLE_START_RETRY_MS;startup_deferred_tick();
 assert(!ble_live&&s_ble_suspended_for_mqtt&&!permit);
 clock_ms+=STARTUP_STACK_SETTLE_MS;startup_deferred_tick();assert(permit&&!paused);
 connecting=true;startup_deferred_tick();assert(!ble_live); /* Finish admitted handshake first. */
 connecting=false;connected=true;free_bytes=90000;largest=50000;
 startup_deferred_tick();assert(ble_live&&!s_ble_suspended_for_mqtt&&connected&&!paused&&restores>=1);
 /* Remote OTA freezes mode changes; only a suspended host with heap can reconnect cloud. */
 ota=true;connected=false;ble_live=false;ble_suspended=true;free_bytes=40000;largest=20000;
 unsigned old_starts=starts;startup_deferred_tick();assert(permit&&starts==old_starts);
 free_bytes=20000;startup_deferred_tick();assert(!permit);
 ota=false;connected=true;ble_failed=true;free_bytes=90000;largest=50000;
 startup_deferred_tick();assert(ota_restores==1&&!ble_suspended&&!ble_failed&&ble_live);
 ota=false;connected=true;ble_suspended=false;ble_live=true;
 ble_failed=true;startup_deferred_tick();assert(!ble_live&&!ble_failed);
 clock_ms+=BLE_START_RETRY_MS;startup_deferred_tick();assert(ble_live&&starts==old_starts+1);
 /* An explicit phone request can trade cloud memory for local access when
  * coexistence is not admitted. Pending healthy BLE startup is not torn down. */
 ble_live=false;s_ble_services_started=false;free_bytes=50000;largest=20000;
 app_request_local_ble(true);startup_deferred_tick();assert(paused&&s_local_ble_mode&&!permit);
 free_bytes=90000;largest=50000;clock_ms+=STARTUP_STACK_SETTLE_MS;
 startup_deferred_tick();assert(ble_live&&paused);
 ble_suspended=true;ble_failed=true;ble_live=false;
 startup_deferred_tick();assert(ota_restores==2&&!ble_failed&&ble_live&&paused);
 app_request_local_ble(false);clock_ms+=LOCAL_BLE_IDLE_MS+1;startup_deferred_tick();
 clock_ms+=STARTUP_STACK_SETTLE_MS;startup_deferred_tick();assert(!paused&&!ble_live&&permit);
 resumes=0;
 /* No credentials: full local mode still releases MQTT and preserves phone/provisioning. */
 connected=false;s_ble_services_started=false;ble_live=false;configured=false;have_ip=false;
 startup_deferred_tick();assert(paused&&s_local_ble_mode&&!permit);
 clock_ms+=STARTUP_STACK_SETTLE_MS;startup_deferred_tick();assert(ble_live&&!permit);
 configured=true;have_ip=true;peer=true;clock_ms+=600000;
 startup_deferred_tick();assert(ble_live&&!permit);
 peer=false;clock_ms+=LOCAL_BLE_IDLE_MS+1;startup_deferred_tick();
 clock_ms+=STARTUP_STACK_SETTLE_MS;startup_deferred_tick();assert(!paused&&!ble_live&&permit&&resumes==1);
 puts("PASS normal resident BLE+MQTT, no menu disconnect, safe peer/reconnect/OTA admission, failed-host retry, no-Wi-Fi setup");
}
'''.replace('DECLARATIONS',declarations).replace('FUNCTIONS','\n'.join(function(app,n) for n in ('app_request_local_ble','local_ble_mode_tick','startup_deferred_tick')))
        if scenario is not None:
            deadlines = '\n'.join(re.findall(r'^    s_(?:ble_start|cloud_retry)_deadline_ms\s*=.*?;', app, re.M))
            code = code[:code.index('int main(void){')] + scenario.replace('DEADLINES', deadlines)
        local_mode.LocalModeTests.compile_run(self,code)

    def test_resident_ble_cloud_reconnect_and_safe_local_fallback(self):
        self.compile_scenario()

    def test_saved_wifi_offline_opens_ble_then_retries_cloud_without_dropping_phone(self):
        self.compile_scenario(r'''
int main(void){
 clock_ms=1000;DEADLINES
 connected=false;configured=true;have_ip=false;
 clock_ms+=9999;startup_deferred_tick();assert(!paused&&!ble_live&&permit);
 clock_ms=31001;startup_deferred_tick();assert(paused&&s_local_ble_mode&&!permit);
 clock_ms+=STARTUP_STACK_SETTLE_MS;startup_deferred_tick();assert(ble_live&&starts==1);
 peer=true;clock_ms+=600000;startup_deferred_tick();assert(ble_live&&paused&&stops==0);
 peer=false;clock_ms+=LOCAL_BLE_IDLE_MS+1;startup_deferred_tick();assert(!ble_live&&paused);
 clock_ms+=STARTUP_STACK_SETTLE_MS;startup_deferred_tick();assert(!paused&&!s_local_ble_mode&&permit);
 unsigned cloud_start=clock_ms;
 clock_ms+=1000;startup_deferred_tick();assert(!paused&&!ble_live&&permit);
 clock_ms=cloud_start+30001;startup_deferred_tick();assert(paused&&s_local_ble_mode);
 clock_ms+=STARTUP_STACK_SETTLE_MS;startup_deferred_tick();assert(ble_live);
 puts("PASS saved Wi-Fi offline recovery, active-phone protection, idle return and repeated cloud/BLE windows");
}
''')

    def test_expired_cloud_window_waits_for_handshake_and_ota_then_recovers(self):
        self.compile_scenario(r'''
int main(void){
 clock_ms=1000;DEADLINES
 connected=false;configured=true;have_ip=true;connecting=true;
 clock_ms=40000;startup_deferred_tick();assert(!paused&&!ble_live&&!pauses);
 connecting=false;ota=true;startup_deferred_tick();assert(!paused&&!ble_live&&!pauses);
 ota=false;startup_deferred_tick();assert(paused&&s_local_ble_mode&&!permit);
 clock_ms+=STARTUP_STACK_SETTLE_MS;startup_deferred_tick();assert(ble_live);
 puts("PASS recovery never destroys admitted MQTT handshake or preempts OTA");
}
''')

    def test_ram_policy_keeps_records_mtu_and_stacks(self):
        defaults=(ROOT/'sdkconfig.defaults').read_text(encoding='utf-8')
        self.assertIn('CONFIG_ESP_WIFI_IRAM_OPT=n',defaults)
        self.assertIn('CONFIG_ESP_WIFI_RX_IRAM_OPT=n',defaults)
        for line in ('CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN=16384','CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN=2048',
                     'CONFIG_MQTT_TASK_STACK_SIZE=8192','CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU=512'):
            self.assertIn(line,defaults)


if __name__=='__main__':
    unittest.main()
