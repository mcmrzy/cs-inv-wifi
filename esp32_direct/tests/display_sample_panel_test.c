#include "direct_display_sample.h"
#include "direct_protocol.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static direct_display_sample_t s_display_sample;
static direct_dsp_status_t s_display_status;
static uint32_t s_display_status_ms,s_display_status_blocked_ms;
static bool s_display_status_valid,s_display_status_waiting;
static struct {
    RunParamDef run;
    direct_dsp_status_t status;
    bool dsp_online, status_valid, dsp_display_held, dsp_status_held;
    uint32_t polls, errors, measurement_age_ms;
} s_ui_data;
static struct { uint32_t poll_count, err_count; } g_arm_stat;
static uint32_t clock_ms, sample_ms;
static bool lock_available=true, getter_available, ota_busy, boot_active;
static RunParamDef input;
static uint32_t now_ms(void) { return clock_ms; }
static bool arm_poll_take_param(void) { return lock_available; }
static void arm_poll_give_param(void) { assert(lock_available); }
static bool ota_service_is_busy(void) { return ota_busy; }
static bool direct_dsp_boot_active(void) { return boot_active; }
static bool direct_dsp_get_status_sample(direct_dsp_status_t *status,uint32_t *received)
{ if(!getter_available)return false;memset(status,0,sizeof(*status));status->mode=9;*received=sample_ms;return true; }
static bool direct_dsp_get_measurement(RunParamDef *measurement, uint32_t *received)
{ if (!getter_available) return false; *measurement=input; *received=sample_ms; return true; }
#include "../tmp/display_sample_panel_under_test.inc"
static void snapshot(void)
{ memset(&s_ui_data, 0, sizeof(s_ui_data)); panel_snapshot_display_test(); }

int main(void)
{
    clock_ms=100; snapshot();
    assert(!s_ui_data.dsp_online && s_ui_data.measurement_age_ms==UINT32_MAX);
    input.BatVolt=497; input.OutputWatt=0; sample_ms=100; getter_available=true;
    snapshot(); assert(s_ui_data.dsp_online && s_ui_data.run.BatVolt==497);
    assert(!s_ui_data.dsp_display_held && s_ui_data.measurement_age_ms==0);
    assert(s_ui_data.status_valid && s_ui_data.status.mode==9);
    lock_available=false; getter_available=false; clock_ms=120;
    snapshot(); assert(s_ui_data.dsp_online && s_ui_data.run.BatVolt==497);
    assert(s_ui_data.status_valid && s_ui_data.status.mode==9);
    clock_ms=2101; snapshot();
    assert(s_ui_data.dsp_display_held && s_ui_data.measurement_age_ms==2001);
    clock_ms=5099; snapshot(); assert(!s_ui_data.dsp_online && s_ui_data.dsp_display_held);
    clock_ms=5100; snapshot(); assert(!s_ui_data.dsp_online && s_ui_data.run.BatVolt==497);
    assert(!s_ui_data.status_valid && s_ui_data.dsp_status_held && s_ui_data.status.mode==9);
    clock_ms=30100; snapshot();
    assert(s_ui_data.dsp_display_held && s_ui_data.run.BatVolt==497);
    getter_available=true; sample_ms=5101; clock_ms=5101;
    snapshot(); assert(s_ui_data.dsp_online);
    ota_busy=true; clock_ms=5200; snapshot();
    assert(!s_ui_data.dsp_online && !s_ui_data.status_valid);
    ota_busy=false; clock_ms=5300; snapshot(); assert(!s_ui_data.dsp_online);
    boot_active=true; clock_ms=5400; sample_ms=5400; snapshot();
    assert(!s_ui_data.dsp_online && !s_ui_data.status_valid);
    boot_active=false; clock_ms=5500; snapshot(); assert(!s_ui_data.dsp_online);
    sample_ms=5501; clock_ms=5501; input.BatVolt=0; snapshot();
    assert(s_ui_data.dsp_online && s_ui_data.run.BatVolt==0);
    puts("PASS actual panel DSP snapshot: retains real zero/data on lock failure, reports stale age, retains 03/04 for display without fresh validity, OTA and Boot discard old readings");
}
