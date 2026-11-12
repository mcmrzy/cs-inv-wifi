"""Execute actual MQTT adapter functions and both shared alarm compile paths."""
from pathlib import Path
import subprocess
import sys

root=Path(__file__).resolve().parents[1]
tmp=root/'tmp'
gcc=sys.argv[1]
def run(name,source,flags=()):
    path=tmp/(name+'.c');path.write_text(source,encoding='utf-8')
    exe=tmp/(name+'.exe')
    subprocess.run([gcc,'-save-temps=obj','-std=c11','-Wall','-Wextra','-Werror',*flags,str(path),'-o',str(exe)],check=True)
    result=subprocess.run([str(exe)],capture_output=True,text=True)
    print(result.stdout,end='');print(result.stderr,end='');result.check_returncode()

adapter=(root/'full/mqtt_handler.c').read_text(encoding='utf-8')
functions=adapter[adapter.index('static atomic_uint s_alarm_listener;'):]
run('alarm_transport_actual',r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
typedef int esp_err_t;
typedef void *esp_mqtt_client_handle_t;
typedef int esp_mqtt_event_id_t;
typedef const char *esp_event_base_t;
typedef void (*esp_event_handler_t)(void *,esp_event_base_t,int32_t,void *);
typedef struct {int msg_id;} event_t;
typedef event_t *esp_mqtt_event_handle_t;
enum {ESP_OK=0,ESP_EVENT_ANY_ID=-1,MQTT_EVENT_CONNECTED=1,MQTT_EVENT_DISCONNECTED=2,MQTT_EVENT_PUBLISHED=3,MQTT_EVENT_DELETED=4};
static atomic_bool g_local_paused,g_mqtt_connected;
static esp_mqtt_client_handle_t g_mqtt_client=(void *)1;
static char g_device_sn[33]="MB00001122334455";
static unsigned registrations,resets,acks,expires,queued;
static int error_on,ack_id,expired_id,enqueue_result=77;
static void *listener_arg;
static esp_event_handler_t listener;
#define ESP_LOGE(...) ((void)0)
static const char *esp_err_to_name(int e){(void)e;return "mock";}
static void full_alarm_transport_reset(void){resets++;}
static void full_alarm_puback(int id){acks++;ack_id=id;}
static void full_alarm_expired(int id){expires++;expired_id=id;}
static bool catalog_online;
static void panel_firmware_catalog_network(bool online){catalog_online=online;}
static esp_err_t esp_mqtt_client_register_event(esp_mqtt_client_handle_t c,int id,esp_event_handler_t cb,void *arg)
{assert(c==g_mqtt_client && id==ESP_EVENT_ANY_ID);registrations++;if((int)registrations==error_on)return 9;listener=cb;listener_arg=arg;return 0;}
static int esp_mqtt_client_enqueue(esp_mqtt_client_handle_t c,const char *topic,const char *data,int len,int qos,int retain,bool store)
{assert(c==g_mqtt_client && (!strcmp(topic,"cs_inv/MB00001122334455/alarm")||!strcmp(topic,"cs_inv/MB00001122334455/ota/status")));assert(!strcmp(data,"{\"v\":1}") && len==(int)strlen(data));assert(qos==1 && !retain && store);queued++;return enqueue_result;}
''' .replace('#define ESP_LOGE(...) ((void)0)','#define ESP_LOGE(t,f,e) ((void)(t),(void)(f),(void)(e))')+functions+r'''
static void original(void *a,esp_event_base_t b,int32_t id,void *d){(void)a;(void)b;(void)id;(void)d;}
int main(void){
 assert(full_register_events(g_mqtt_client,-1,original,NULL)==0 && registrations==2);
 event_t e={55};listener(listener_arg,"MQTT",MQTT_EVENT_CONNECTED,NULL);assert(resets==2);
 assert(catalog_online);
 listener(listener_arg,"MQTT",MQTT_EVENT_PUBLISHED,&e);assert(acks==1 && ack_id==55);
 listener(listener_arg,"MQTT",MQTT_EVENT_DELETED,&e);assert(expires==1 && expired_id==55);
 assert(full_alarm_mqtt_enqueue("{\"v\":1}")==-1 && !queued);
 atomic_store(&g_mqtt_connected,true);assert(full_alarm_mqtt_enqueue("{\"v\":1}")==77 && queued==1);
 atomic_store(&g_local_paused,true);assert(full_alarm_mqtt_enqueue("{\"v\":1}")==-1 && queued==1);atomic_store(&g_local_paused,false);
 void *old=listener_arg;assert(full_register_events(g_mqtt_client,-1,original,NULL)==0 && registrations==4);
 listener(old,"MQTT",MQTT_EVENT_PUBLISHED,&e);assert(acks==1);listener(listener_arg,"MQTT",MQTT_EVENT_PUBLISHED,&e);assert(acks==2);
 error_on=6;assert(full_register_events(g_mqtt_client,-1,original,NULL)==9);assert(full_alarm_mqtt_enqueue("{\"v\":1}")==-2 && queued==1);
 error_on=0;assert(full_register_events(g_mqtt_client,-1,original,NULL)==0);enqueue_result=-1;
 assert(full_alarm_mqtt_enqueue("{\"v\":1}")==-4 && queued==2);
 assert(!full_panel_rpc_publish("{\"v\":1}",7));enqueue_result=99;
 assert(full_panel_rpc_publish("{\"v\":1}",7));
 listener(listener_arg,"MQTT",MQTT_EVENT_DISCONNECTED,NULL);assert(!catalog_online);
 puts("PASS alarm transport: strict envelope unchanged, nonblocking QoS1, ACK/deletion listeners on every client, stale callbacks ignored, registration failure retained");return 0;
}
''')

telemetry=(root.parent/'esp32_direct/main/telemetry/telemetry.c').read_text(encoding='utf-8')
functions=telemetry[telemetry.index('static void publish_arm_fault_alarm('):telemetry.index('void telemetry_init(')]
harness=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
typedef struct {uint32_t FaultValue;uint64_t Warning;uint16_t BmsWarning;} RunParamDef;
static bool s_alarm_armed;
static uint64_t s_last_warning;
static uint32_t s_last_fault;
static uint16_t s_last_bms;
static unsigned messages;
#define TOPIC_ALARM "alarm"
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
static int mqtt_publish(const char *topic,const char *text,int len,int qos,int retain)
{assert(!strcmp(topic,"alarm") && text && !len && qos==1 && !retain);messages++;return 0;}
'''+functions+r'''
int main(void){
 publish_arm_fault_alarm();publish_arm_recovery_alarm();RunParamDef p={0};check_alarm(&p);p.FaultValue=1;p.Warning=2;p.BmsWarning=4;check_alarm(&p);
 #if PANEL_FULL_DIRECT
 assert(!messages);
 #else
 assert(messages==5);
 #endif
 puts("PASS legacy alarm ownership: full Modbus build suppresses translated events; original direct build retains existing behavior");return 0;
}
'''
for mode in [0,1]:run('alarm_ownership_'+str(mode),harness,['-DPANEL_FULL_DIRECT='+str(mode)])

bus=(root/'main/panel_bus.c').read_text(encoding='utf-8')
assert 'poll_block(0x41,4);poll_block(0x46,4)' in bus
sample=bus[bus.index('bool panel_bus_alarm_sample('):bus.index('bool panel_bus_submit(')]
assert 's_shared.valid[slot]' in sample and 'PANEL_FRESH_MS' in sample
assert 's_shared.scanning' in sample and 's_quiet' in sample
boot=(root/'full/app_main.c').read_text(encoding='utf-8')
assert 'full_alarm_init();' in boot and 'full_alarm_tick();' in boot
assert 'full_alarm_needs_attention()' in boot
assert 'CONFIG_MQTT_REPORT_DELETED_MESSAGES=y' in (root/'sdkconfig.full').read_text(encoding='utf-8')
print('PASS alarm integration: independent raw-word polling, freshness/quiet checks, startup/tick and local warning wired')
