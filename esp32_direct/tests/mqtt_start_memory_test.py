"""Execute real startup ordering and MQTT start against a fragmented heap model."""
from pathlib import Path
import re
import subprocess

root = Path(__file__).resolve().parents[1]
gcc = Path(r'C:\Users\29538\AppData\Local\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin\gcc.exe')
source = (root / 'main/mqtt/mqtt_handler.c').read_text(encoding='utf-8')
app = (root / 'main/app/app_main.c').read_text(encoding='utf-8')

def function(text, signature):
    cursor = 0
    while True:
        start = text.index(signature, cursor)
        brace = text.index('{', start)
        semicolon = text.find(';', start, brace)
        if semicolon < 0:
            break
        cursor = semicolon + 1
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]

startup = app[app.index('    telemetry_init();', app.index('void app_main(')):]
startup = startup[:startup.index('\n}')]
deferred = ('\n'.join(function(app, name) for name in
                      ('static bool start_ble_services(', 'void app_request_local_ble(', 'static void local_ble_mode_tick(', 'static void startup_deferred_tick('))
            if 'static void startup_deferred_tick(' in app else '')
defines = '\n'.join(re.findall(r'^#define (?:MQTT_START_RETRY_MS|MQTT_TRANSPORT_START_TIMEOUT_MS|MQTT_TASK_STACK_SIZE|MQTT_START_RESERVE_SIZE)\s+.*$', source, re.M))
defines += '\n' + '\n'.join(re.findall(r'^#define (?:MAIN_LOOP_STACK_SIZE|STARTUP_STACK_SETTLE_MS|BLE_STARTUP_WAIT_MS|BLE_START_RETRY_MS|LOCAL_BLE_IDLE_MS|CLOUD_RETRY_WINDOW_MS|LCD_FIRST_UI_WAIT_MS)\s+.*$', app, re.M))
declarations = '\n'.join(re.findall(r'^static (?:atomic_llong g_(?:next_start_ms|connect_attempt_ms)|atomic_bool g_(?:mqtt_connecting|connect_allowed|local_paused|transport_entered)|void \*g_start_reserve)[^;]*;', source, re.M))
declarations += '\n' + '\n'.join(re.findall(r'^static (?:bool s_(?:ble_suspended_for_mqtt|local_ble_mode)|atomic_bool s_local_ble_requested|uint32_t s_local_ble_idle_deadline_ms)[^;]*;', app, re.M))
dns_condition = re.search(r'const bool cannot_resolve\s*=.*?;', source, re.S).group()
harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
DEFINES
DECLARATIONS
static void *g_mqtt_client=(void*)1;
static atomic_bool g_mqtt_connected,g_mqtt_started;
static int64_t g_disconnect_start_ms, clock_ms=1000;
static const char *TAG="MQTT",*g_mqtt_uri="mqtts://test";
static unsigned largest=16000, starts, memory_logs;
static bool wifi_initialized, ble_started, main_created, startup_suspended, ota_busy;
static unsigned startup_deletes, ble_attempts, ready_notifications;
static bool fail_main, fail_ble;
static bool have_ip;
static bool stored_wifi=true;
bool wifi_sta_is_connected(void){return have_ip;}
#define MQTT_ERROR_TYPE_TCP_TRANSPORT 1
#define ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME 2
static bool dns_failure(int kind,int code){
 struct{int error_type,esp_tls_last_esp_err;} value={kind,code},*err=&value;
 DNS_CONDITION
 return cannot_resolve;
}
static int64_t esp_timer_get_time(void){return clock_ms*1000;}
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
static const char*esp_err_to_name(int e){(void)e;return "ESP_FAIL";}
static void log_message(const char*t,const char*f,...){(void)t;(void)f;}
#define ESP_LOGI log_message
#define ESP_LOGE log_message
#define ESP_LOGW log_message
void mqtt_log_tls_memory(const char*p){assert(p);memory_logs++;}
static int esp_mqtt_client_start(void*c){assert(c);starts++;if(largest<8192)return ESP_FAIL;largest-=8192;return ESP_OK;}
static int esp_mqtt_client_reconnect(void*c){assert(c);return ESP_OK;}
static void heap_caps_free(void*p){free(p);}
static void mqtt_force_restart(void){atomic_store(&g_mqtt_connecting,false);}
CONNECT
#define NOOP(name) static void name(void){}
NOOP(telemetry_init) NOOP(cmd_handler_init) NOOP(mqtt_ota_init)
NOOP(offline_cache_init) NOOP(sntp_sync_init)
NOOP(direct_panel_diagnostic_tick)
static bool direct_panel_software_healthy(void){return true;}
static bool mqtt_init(void*p){(void)p;return true;}
static bool ota_service_init(void){return true;}
static void wifi_sta_init(void){wifi_initialized=true;}
static void setup_wifi(void){assert(wifi_initialized);}
static bool ble_ct_start(const char*a,const char*b){assert(a&&b);return true;}
static bool ble_ota_init(void){return true;}
static bool ble_prov_start(const char*a,const char*b){assert(a&&b&&startup_deletes==1&&main_created);ble_attempts++;if(fail_ble)return false;ble_started=true;largest=6000;return true;}
static const char*ble_prov_get_service_name(void){return "test";}
#define ble_ct_cmd_set_executors(a,b) ((void)0)
#define ble_ct_cmd_set_battery_executors(a,b) ((void)0)
#define ble_prov_set_callback(a) ((void)0)
#define ble_prov_set_gap_event_forward(a) ((void)0)
#define ble_prov_set_pre_host_start_cb(a) ((void)0)
static void *cmd_handler_on_mqtt_message;
static char g_device_sn[]="test";
#define FIRMWARE_VERSION "test"
#define STATE_INIT 0
#define BOOT_FAILED 1
#define BOOT_READY 2
#define pdPASS 1
#define pdTRUE 1
#define portMAX_DELAY 0xffffffffU
#define pdMS_TO_TICKS(x) (x)
#define MALLOC_CAP_8BIT 1
#define MALLOC_CAP_INTERNAL 2
typedef void *TaskHandle_t;
#define MAYBE_UNUSED __attribute__((unused))
static MAYBE_UNUSED TaskHandle_t s_main_loop_handle,s_startup_owner;
static MAYBE_UNUSED atomic_bool s_startup_health_done;
static MAYBE_UNUSED bool s_ble_started;
static MAYBE_UNUSED bool s_ble_services_started;
static MAYBE_UNUSED uint32_t s_startup_release_ms,s_ble_start_deadline_ms,s_next_ble_start_ms,s_cloud_retry_deadline_ms;
static uint32_t s_next_coexist_log_ms;
static atomic_int s_boot_stage;
static bool s_prov_busy;
static bool ble_prov_is_connected(void){return false;}
static bool ble_prov_is_ready(void){return ble_started;}
static bool ble_prov_is_suspended(void){return s_ble_suspended_for_mqtt;}
static bool ble_prov_restore_after_ota(void){return true;}
static bool ble_prov_suspend_for_mqtt(void){return true;}
static bool ble_prov_restore_after_mqtt(void){return true;}
static bool ble_prov_start_failed(void){return false;}
static bool ble_prov_recover_start_failure(void){return true;}
static bool ota_service_admin_begin(void){return !ota_busy;}
static void ota_service_admin_end(void){}
static bool mqtt_is_paused_for_local(void){return atomic_load(&g_local_paused);}
static bool mqtt_pause_for_local(void){atomic_store(&g_local_paused,true);atomic_store(&g_mqtt_connected,false);return true;}
static bool mqtt_resume_after_local(void){atomic_store(&g_local_paused,false);return true;}
static void *main_loop_task;
static void enter_state(int s){(void)s;}
static int xTaskCreate(void*f,const char*n,int s,void*p,int pr,TaskHandle_t*h){(void)f;(void)n;(void)p;(void)pr;if(fail_main || (unsigned)s>largest)return 0;assert(!ble_started&&s==6144);largest-=(unsigned)s;main_created=true;if(h)*h=(void*)2;return pdPASS;}
static bool ota_service_start_boot_health_check(void){assert(main_created&&!ble_started&&ready_notifications==1);return true;}
static MAYBE_UNUSED TaskHandle_t xTaskGetCurrentTaskHandle(void){return (void*)1;}
static MAYBE_UNUSED void xTaskNotifyGive(TaskHandle_t h){assert(h==(void*)2);ready_notifications++;}
static MAYBE_UNUSED void vTaskSuspend(void*p){assert(p==NULL);startup_suspended=true;}
static MAYBE_UNUSED void vTaskDelay(unsigned ticks){assert(ticks>=STARTUP_STACK_SETTLE_MS);clock_ms+=ticks;}
static MAYBE_UNUSED void vTaskDelete(TaskHandle_t h){assert(h==(void*)1&&startup_suspended&&!ble_started);startup_deletes++;largest+=12288;}
static MAYBE_UNUSED bool storage_has_wifi_config(void){return stored_wifi;}
static MAYBE_UNUSED bool ota_service_is_busy(void){return ota_busy;}
static MAYBE_UNUSED unsigned esp_get_free_heap_size(void){return largest;}
static MAYBE_UNUSED unsigned heap_caps_get_largest_free_block(int caps){(void)caps;return largest;}
static MAYBE_UNUSED unsigned heap_caps_get_free_size(int caps){(void)caps;return 90000;}
static MAYBE_UNUSED bool mqtt_is_connected(void){return atomic_load(&g_mqtt_connected);}
static MAYBE_UNUSED int64_t now_ms(void){return clock_ms;}
#define WIFI_DEFAULT_SSID ""
__DISPLAY_GATE__
__DEFERRED_CODE__
static void startup(void){__STARTUP_BODY__}
int main(void){
 startup();
 assert(main_created && startup_suspended && !ble_started && starts==0);
#ifdef HAS_DEFERRED
 clock_ms+=STARTUP_STACK_SETTLE_MS;
 startup_deferred_tick();assert(startup_deletes==1&&!ble_started);
 clock_ms+=STARTUP_STACK_SETTLE_MS;
 have_ip=true;startup_deferred_tick();assert(mqtt_connect()&&starts==1);
 atomic_store(&g_mqtt_connecting,false);atomic_store(&g_mqtt_connected,true);
 ota_busy=true;startup_deferred_tick();assert(!ble_started);
 ota_busy=false;startup_deferred_tick();assert(ble_attempts==1&&ble_started);
 clock_ms+=600000;startup_deferred_tick();assert(ble_attempts==1&&ble_started);
 atomic_store(&g_mqtt_connected,false);
#endif
 mqtt_connect();
 assert(ble_started && starts==1 && atomic_load(&g_mqtt_started));
 /* Later reconnect polling must not create a second task. */
 assert(!mqtt_connect() && starts==1); /* BLE is no longer a transport admission. */
 atomic_store(&g_mqtt_started,false);atomic_store(&g_mqtt_connecting,false);g_disconnect_start_ms=0;g_next_start_ms=0;
 mqtt_set_connect_allowed(true);
 starts=0;clock_ms=10000;largest=6000;
 assert(!mqtt_connect() && starts==1 && g_disconnect_start_ms==clock_ms);
 for(int i=0;i<100;i++){clock_ms=10000+i*40;assert(!mqtt_connect());}
 assert(starts==1); /* Two polls/100 ms must not become two allocations. */
 clock_ms=15000;largest=14000;
 assert(mqtt_connect() && starts==2 && atomic_load(&g_mqtt_started));
 assert(memory_logs>=1);
 atomic_store(&g_mqtt_connected,true);assert(mqtt_connect() && starts==2);
 have_ip=false;assert(!dns_failure(1,2));
 have_ip=true;assert(dns_failure(1,2));assert(!dns_failure(0,2));assert(!dns_failure(1,0));
 atomic_store(&g_mqtt_connected,false);largest=16000;starts=0;atomic_store(&g_mqtt_started,false);
 atomic_store(&g_mqtt_connecting,false);mqtt_set_connect_allowed(false);have_ip=false;stored_wifi=false;
 atomic_store(&g_local_paused,false);atomic_store(&s_local_ble_requested,false);s_local_ble_mode=false;
 main_created=ble_started=startup_suspended=s_ble_started=false;startup_deletes=ble_attempts=ready_notifications=0;
 s_main_loop_handle=s_startup_owner=NULL;atomic_store(&s_startup_health_done,false);s_ble_services_started=false;s_next_ble_start_ms=0;
 startup();assert(!ble_started&&main_created);
#ifdef HAS_DEFERRED
 clock_ms=s_ble_start_deadline_ms;startup_deferred_tick();assert(!ble_started&&startup_deletes==1);
 clock_ms+=STARTUP_STACK_SETTLE_MS;startup_deferred_tick();assert(!ble_started&&s_local_ble_mode);
 clock_ms+=STARTUP_STACK_SETTLE_MS;startup_deferred_tick();assert(ble_started&&startup_deletes==1); /* offline provisioning remains available */
#endif
 largest=16000;main_created=ble_started=false;fail_main=true;atomic_store(&g_mqtt_started,false);starts=0;
 startup();assert(!main_created&&!ble_started&&starts==0&&atomic_load(&s_boot_stage)==BOOT_FAILED);
 puts("PASS real startup waits for DHCP and reclaimed startup stack; 8KB MQTT task precedes BLE; start failure backoff/recovery and offline provisioning preserved");
}
'''
for marker, value in [('DEFINES',defines),('DECLARATIONS',declarations),
                      ('DNS_CONDITION',dns_condition),
                      ('CONNECT','\n'.join(function(source, name) for name in ('void mqtt_set_connect_allowed(', 'bool mqtt_is_connecting(', 'void mqtt_poll_connection(', 'bool mqtt_connect('))),
                      ('__STARTUP_BODY__',startup),
                      ('__DISPLAY_GATE__',function(app,'static bool wait_for_first_ui_frame(')),
                      ('__DEFERRED_CODE__',('#define HAS_DEFERRED 1\n'+deferred) if deferred else '')]:
    harness = harness.replace(marker,value)
target=root/'tmp/mqtt_start_memory_test.c'
target.write_text(harness,encoding='utf-8')
subprocess.run([str(gcc),'-std=c11','-Wall','-Wextra','-Werror',str(target),'-o',str(target.with_suffix('.exe'))],check=True,timeout=30)
subprocess.run([str(target.with_suffix('.exe'))],check=True,timeout=10)
