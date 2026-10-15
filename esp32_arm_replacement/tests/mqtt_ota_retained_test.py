"""Replay real MQTT fragment handling: retained OTA must never reach start."""
from pathlib import Path
import subprocess

root=Path(__file__).resolve().parents[1]
gcc=Path(r'C:\Users\29538\AppData\Local\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin\gcc.exe')
adapter=(root/'main/mqtt/ota_mqtt_adapter.c').read_text(encoding='utf-8')
handler=(root/'main/mqtt/mqtt_handler.c').read_text(encoding='utf-8')
def fn(text,signature):
 start=text.index(signature);end=text.index('{',start)+1;depth=1
 while depth:
  depth+=(text[end]=='{')-(text[end]=='}');end+=1
 return text[start:end]
wrapper=(fn(adapter,'bool mqtt_ota_handle_delivery(') if 'bool mqtt_ota_handle_delivery(' in adapter else '')
start=handler.index('    case MQTT_EVENT_DATA:')+len('    case MQTT_EVENT_DATA:')
end=handler.index('    case MQTT_EVENT_SUBSCRIBED:',start)
code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define MQTT_RX_MAX 2048
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
static char g_rx_topic[128],g_rx_payload[2049];
static int g_rx_expected,g_rx_next_offset;
static bool g_rx_retained;
static unsigned starts,other,binding;
static bool g_msg_handler=true;
bool mqtt_ota_handle_command(const char*t,const uint8_t*p,unsigned n){assert(t&&p&&n);starts++;return true;}
static void cloud_binding_receive(const char*t,const void*p,size_t n){assert(t&&p&&n);binding++;}
WRAPPER
RESET
typedef struct {int data_len,total_data_len,current_data_offset,topic_len;const char*topic,*data;bool retain;} event_t;
static void receive(const event_t*event){switch(0){case 0:BODY}}
int main(void){
 const char *topic="cs_inv/test/ota/cmd";
 event_t e={.data_len=2,.total_data_len=4,.topic_len=(int)strlen(topic),.topic=topic,.data="ab",.retain=true};
 receive(&e);assert(!starts);e.current_data_offset=2;e.retain=false;e.data="cd";receive(&e);
 assert(!starts); /* Retain flag from first fragment survives later fragments. */
 e.current_data_offset=0;e.data_len=e.total_data_len=4;e.data="abcd";e.retain=false;
 receive(&e);assert(starts==1);
 e.retain=true;receive(&e);assert(starts==1);
 e.retain=false;receive(&e);assert(starts==2);
 assert(!g_rx_expected&&!g_rx_retained&&!other&&!binding);
 puts("PASS real MQTT fragment delivery: retained OTA rejected, first-fragment flag retained, ordinary new commands allowed, receive state reset");
}
'''
# Actual code calls the existing message-handler function pointer for other topics.
code=code.replace('static bool g_msg_handler=true;', 'static void generic(const char*t,const char*p,unsigned n){(void)t;(void)p;(void)n;other++;}\nstatic void (*g_msg_handler)(const char*,const char*,unsigned)=generic;')
code=code.replace('WRAPPER',wrapper).replace('RESET',fn(handler,'static void mqtt_rx_reset(')).replace('BODY',handler[start:end])
target=root/'tmp/mqtt_ota_retained_test.c';target.write_text(code,encoding='utf-8')
subprocess.run([str(gcc),'-std=c11','-Wall','-Wextra','-Werror',str(target),'-o',str(target.with_suffix('.exe'))],check=True)
subprocess.run([str(target.with_suffix('.exe'))],check=True)
