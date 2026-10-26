#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include "arm_poll.h"
#include "direct_protocol.h"
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ONLINE_PERIOD_MS 90000U
#define TELEMETRY_PERIOD_MS 180000U
#define ARM_FAIL_THRESHOLD 3
arm_link_stat_t g_arm_stat;
OnlyReadDataDef g_only_read;
ReadWiteDataDef g_read_write;
uint16_t g_ctrl_param[CTRL_PARAM_COUNT];
static uint32_t clock_ms,s_now_ms,s_last_hb_ms,s_last_online_ms,s_last_info_ms,s_last_config_ms;
static uint32_t s_debug_deadline_ms,s_debug_interval_ms,s_arm_fail_count;
static bool s_hb_requested,s_config_requested,s_fault_reported,s_debug_active,s_quiet,fresh,locked;
static unsigned sent,ble_sent,info_sent;
static uint32_t now_ms(void){return clock_ms;}
bool arm_poll_take_param(void){assert(!locked);locked=true;return true;}
void arm_poll_give_param(void){assert(locked);locked=false;}
void arm_poll_request_info(void){}
static void telemetry_request_config(void){s_config_requested=true;}
static void publish_online(void){}
static void publish_identity_changes(void){}
static void publish_info(const OnlyReadDataDef *p){(void)p;assert(!locked);info_sent++;}
static void publish_config(const uint16_t *p,const bool *valid,uint32_t rev){(void)p;(void)valid;(void)rev;assert(!locked);}
static bool direct_config_snapshot_wire(uint16_t *p,bool *valid,uint32_t *rev){(void)p;(void)valid;(void)rev;assert(!locked);return false;}
static void publish_arm_recovery_alarm(void){}
static void publish_arm_fault_alarm(void){}
static void publish_heartbeat(const RunParamDef *p,const direct_bms_summary_t *b){(void)b;assert(p->SysStatus==123);sent++;}
static bool direct_bms_get_summary(direct_bms_summary_t *b){(void)b;assert(!locked);return false;}
static void publish_ble_telemetry(const RunParamDef *p){assert(p->SysStatus==123);ble_sent++;}
static void check_alarm(const RunParamDef *p){assert(p->SysStatus==123);}
static bool direct_dsp_get_measurement(RunParamDef *p,uint32_t *received){
    assert(!locked&&p&&received);
    *received=clock_ms;
    if(fresh)p->SysStatus=123;
    return fresh;
}
#include "../tmp/telemetry_fresh_under_test.inc"
int main(void){
    clock_ms=1000;g_arm_stat.last_rx_ms=1000;g_arm_stat.poll_count=10;
    telemetry_on_mqtt_connected();assert(info_sent==1);
    telemetry_request_info();assert(info_sent==2);
    telemetry_tick();assert(sent==0&&s_hb_requested);
    clock_ms=1100;telemetry_tick();assert(sent==0&&s_arm_fail_count==0);
    fresh=true;telemetry_tick();assert(sent==1&&ble_sent==1&&!s_hb_requested);
    fresh=false;clock_ms+=180000;g_arm_stat.last_rx_ms=clock_ms;
    telemetry_tick();assert(sent==1&&ble_sent==1);
    fresh=true;s_hb_requested=true;telemetry_tick();assert(sent==2&&ble_sent==2);
    s_quiet=true;s_hb_requested=true;telemetry_tick();assert(sent==2&&ble_sent==3);
    puts("PASS telemetry: info on reconnect/query without ARM; status-only/stale frames never uploaded; fresh reconnect and query; no nested snapshot lock");
    return 0;
}
