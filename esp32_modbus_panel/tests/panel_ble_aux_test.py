"""Exercise the production BLE snapshot and aux API with fake bus and clock."""
from pathlib import Path
import re
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
sdk = Path('D:/.espressif/v5.5.4/esp-idf/components/json/cJSON')
shared = root.parent / 'esp32_direct'
body = (root / 'full/panel_ble_aux.c').read_text(encoding='utf-8')
body = re.sub(r'^#include "[^\n]+"\s*$', '', body, flags=re.M)
fixture = r'''
#include "cJSON.h"
#include "panel_types.h"
#include "panel_catalog.h"
#include "panel_ui.h"
#include "panel_preferences.h"
#include "panel_bms_protocol.h"
#include "direct_protocol.h"
#include "ble_ct_cmd.h"
#include <assert.h>
#include <time.h>
#include <sys/time.h>
#include <stdio.h>
#include <string.h>

static panel_snapshot_t live;
static direct_bms_measurement_t pack;
static uint32_t pack_age;
static bool have_pack, connected, busy, write_fail;
static unsigned notifications, updates;
static char json[600], g_device_sn[] = "MB00001122334455";
static panel_preferences_t stored = {.alarm=1,.buzzer=1,.blank=1,
    .timezone=480,.style=2,.display={100,2},.feedback={24,3}};

static panel_snapshot_t *panel_ble_snapshot(void) { return &live; }
static bool panel_ble_read_uint(const cJSON *params,const char *key,uint32_t max,uint32_t *out) {
    const cJSON *item=cJSON_GetObjectItemCaseSensitive(params,key);
    if(!cJSON_IsNumber(item) || !(item->valuedouble>=0 && item->valuedouble<=max))return false;
    *out=(uint32_t)item->valuedouble;
    return item->valuedouble==*out;
}
static bool panel_ble_english(const cJSON *params) {
    return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(params,"en"));
}
static bool direct_bms_get_snapshot(direct_bms_measurement_t *b,uint32_t *age) {
    *b=pack;*age=pack_age;return have_pack;
}
static bool ota_service_is_busy(void) { return busy; }
static bool ble_prov_is_connected(void) { return connected; }
static int64_t esp_timer_get_time(void) { return (int64_t)live.now_ms*1000; }
static void ble_ct_telemetry_notify(const char *text,uint16_t len) {
    assert(len<sizeof(json));memcpy(json,text,len);json[len]=0;++notifications;
}
bool panel_preferences_get(panel_preferences_t *p) { *p=stored;return true; }
bool panel_preferences_reset(void) { return !write_fail; }
bool panel_preferences_update(const panel_preferences_t *p,unsigned mask) {
    ++updates;
    if(write_fail)return false;
    assert(mask==PANEL_PREF_BRIGHTNESS);
    stored.display.brightness=p->display.brightness;
    return true;
}
static void full_ui_reload_local_preferences(void) {}
bool panel_bms_protocol_set(panel_bms_protocol_t p) { return p==PANEL_BMS_CSERGY; }
bool panel_bms_detect_start(void) { return true; }
panel_bms_protocol_t panel_bms_protocol_get(void) { return PANEL_BMS_CSERGY; }
panel_bms_detection_t panel_bms_detection(void) { return PANEL_BMS_DETECT_MATCHED; }
bool panel_ui_view(const panel_ui_t *u,const panel_snapshot_t *s,panel_view_t *out) {
    (void)s;memset(out,0,sizeof(*out));
    strcpy(out->title,u->language_en?"Battery":"电池");
    strcpy(out->rows[0],u->language_en?"Battery voltage":"电池电压");
    return true;
}
static struct tm *test_gmtime_r(const time_t *time,struct tm *result) {
    const struct tm *value=gmtime(time);
    if(!value)return NULL;
    *result=*value;
    return result;
}
static int test_settimeofday(const struct timeval *t,void *z) {(void)t;(void)z;return 0;}
#define gmtime_r test_gmtime_r
#define settimeofday test_settimeofday
SOURCE

int main(void) {
    live.config.profile_override=PANEL_PROFILE_3KU;
    live.now_ms=1000;
    panel_store(&live,0x42,5,1000);
    panel_store(&live,0x81,1,1000);
    panel_store(&live,0x58,0x7fff,1000);
    panel_ble_aux_tick();assert(!notifications);
    connected=true;panel_ble_aux_tick();assert(notifications==1);
    cJSON *root=cJSON_Parse(json);assert(root);
    assert(cJSON_IsTrue(cJSON_GetObjectItem(root,"online")));
    assert(cJSON_GetNumberValue(cJSON_GetObjectItem(cJSON_GetObjectItem(root,"battery"),"current"))==-0.1);
    assert(cJSON_IsNull(cJSON_GetObjectItem(cJSON_GetObjectItem(root,"ac"),"voltage")));
    cJSON_Delete(root);
    live.now_ms+=999;panel_ble_aux_tick();assert(notifications==1);
    live.now_ms+=5001;panel_ble_aux_tick();assert(notifications==2);
    root=cJSON_Parse(json);assert(cJSON_IsFalse(cJSON_GetObjectItem(root,"online")));
    assert(cJSON_IsNull(cJSON_GetObjectItem(cJSON_GetObjectItem(root,"battery"),"current")));
    cJSON_Delete(root);
    have_pack=true;pack.voltage_10mv=5200;pack.current_10ma=-105;
    pack.percent_divisor=10;pack.soc_percent=780;pack.soh_percent=975;
    pack.cell_mv[0]=3200;pack.cell_mv[8]=3300;pack_age=6000;
    cJSON *query=cJSON_Parse("{\"page\":0}");cJSON *data=cJSON_CreateObject();
    assert(!panel_ble_aux_read("panel_battery",query,data));
    assert(cJSON_IsFalse(cJSON_GetObjectItem(data,"online")));
    cJSON *cells=cJSON_GetObjectItem(data,"cells");
    assert(cJSON_GetArraySize(cells)==8 && cJSON_GetNumberValue(cJSON_GetArrayItem(cells,0))==3200);
    assert(cJSON_IsNull(cJSON_GetArrayItem(cells,1)));
    assert(cJSON_GetNumberValue(cJSON_GetObjectItem(data,"count"))==9);
    cJSON_Delete(data);cJSON_Delete(query);
    query=cJSON_Parse("{\"page\":1,\"offset\":0,\"en\":true}");data=cJSON_CreateObject();
    assert(!panel_ble_aux_read("panel_metric",query,data));
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(data,"title")),"Battery"));
    cJSON_Delete(data);cJSON_Delete(query);
    live.config.profile_override=PANEL_PROFILE_6KU;
    panel_store(&live,0x42,2,live.now_ms);
    panel_store(&live,0x150,4,live.now_ms);
    pack.charge_max_deciamp=720;pack.charge_max_decivolt=560;
    pack.discharge_min_decivolt=440;
    assert(panel_ble_battery_suggestion(&live,0x4106)==0);
    pack_age=100;
    assert(panel_ble_battery_suggestion(&live,0x4106)==7);
    pack.protection=1;
    assert(panel_ble_battery_suggestion(&live,0x4106)==0);
    pack.protection=0;
    panel_store(&live,0x42,3,live.now_ms);
    assert(panel_ble_battery_suggestion(&live,0x4106)==0);
    assert(panel_ble_aux_write("panel_brightness",9)==BLE_CMD_OUT_OF_RANGE && updates==0);
    busy=true;assert(panel_ble_aux_write("panel_brightness",60)==BLE_CMD_UPGRADE_BUSY);
    busy=false;write_fail=true;
    assert(panel_ble_aux_write("panel_brightness",60)==BLE_WRITE_SAVE_FAILED);
    assert(!panel_ble_aux_take_preferences_changed());
    write_fail=false;assert(panel_ble_aux_write("panel_brightness",60)==BLE_CMD_OK);
    assert(stored.display.brightness==60 && panel_ble_aux_take_preferences_changed());
    puts("PASS BLE panel telemetry and preferences: freshness, sign, sentinels, BMS cells, guarded limits and committed save");
}
'''
source = root / 'tmp/panel_ble_aux_test.c'
source.write_text(fixture.replace('SOURCE', body), encoding='utf-8')
exe = source.with_suffix('.exe')
includes = [sdk, root/'main', root/'full', shared/'main/direct', shared/'components/ble_ct']
subprocess.run([sys.argv[1], '-std=c11', '-Wall', '-Wextra', '-Werror',
    *[f'-I{path}' for path in includes], str(source), str(sdk/'cJSON.c'),
    str(root/'main/panel_model.c'), str(root/'main/panel_catalog.c'), '-o', str(exe)], check=True)
subprocess.run([str(exe)], check=True)
