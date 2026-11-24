"""Exercise the actual cloud config formatter and Modbus request scheduler."""
from pathlib import Path
import subprocess

panel = Path(__file__).resolve().parents[1]
telemetry = (panel.parent / "esp32_direct/main/telemetry/telemetry.c").read_text(encoding="utf-8")
start = telemetry.index("static bool publish_config(")
stop = telemetry.index("/* ---- ARM 通信故障告警", start)
source = telemetry[start:stop]
test = r'''#include <assert.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#define PANEL_FULL_DIRECT 1
#define TELEMETRY_CONFIG_COUNT 4
#define TOPIC_CONFIG "config"
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
static struct {const char *name; int scale;} s_cfg_keys[]={
    {"set_modbus_power_saving",1},{"set_modbus_auto_frequency",1},
    {"set_modbus_battery_open_alarm",1},{"set_modbus_inverter_mute",1},
};
static atomic_bool s_config_requested=true;
static uint32_t s_modbus_config_attempt_ms;
static unsigned snapshots, sends;
static bool online, available, fail, request_during_publish;
static char sent[2048];
static bool mqtt_is_connected(void){return online;}
static bool full_modbus_cloud_config_snapshot(uint16_t *values,bool *valid,uint32_t *revision)
{
    snapshots++;
    if(!available)return false;
    values[0]=1;values[1]=0;values[2]=1;values[3]=1;
    valid[0]=true;valid[1]=true;valid[2]=false;valid[3]=true;
    *revision=9;return true;
}
static int mqtt_publish_volatile(const char *topic,const char *data,int len,int qos)
{
    assert(!strcmp(topic,"config") && len==(int)strlen(data) && qos==1);
    sends++;
    if(request_during_publish)atomic_store(&s_config_requested,true);
    if(fail)return -1;
    snprintf(sent,sizeof(sent),"%s",data);return 0;
}
SOURCE
int main(void)
{
    publish_requested_modbus_config(1000);
    assert(snapshots==0 && sends==0 && s_config_requested);
    online=true;publish_requested_modbus_config(1000);
    assert(snapshots==1 && sends==0 && s_config_requested);
    available=true;publish_requested_modbus_config(1999);
    assert(snapshots==1);
    fail=true;publish_requested_modbus_config(2000);
    assert(snapshots==2 && sends==1 && s_config_requested);
    fail=false;request_during_publish=true;publish_requested_modbus_config(3000);
    assert(sends==2 && s_config_requested);
    assert(!strcmp(sent,"{\"rev\":9,\"params\":{\"set_modbus_power_saving\":1,\"set_modbus_auto_frequency\":0,\"set_modbus_inverter_mute\":1}}"));
    request_during_publish=false;publish_requested_modbus_config(4000);
    assert(sends==3 && !s_config_requested);
    publish_requested_modbus_config(5000);assert(sends==3);
    puts("PASS Modbus /config: no ARM timestamp dependency, QoS1, unknown omission and retry");
}
'''.replace("SOURCE", source)
target = panel / "tmp/modbus_cloud_config_test.c"
target.parent.mkdir(exist_ok=True)
target.write_text(test, encoding="utf-8")
gcc = Path(r"C:\Users\29538\AppData\Local\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin\gcc.exe")
exe = target.with_suffix(".exe")
subprocess.run([str(gcc), "-std=c11", "-Wall", "-Wextra", "-Werror", str(target), "-o", str(exe)], check=True)
subprocess.run([str(exe)], check=True)
