"""Run the firmware's actual BLE lifecycle functions with fake IDF boundaries.

These tests verify ordering, recovery and producer exclusion, not radio behavior
or reclaimed heap on a physical ESP32-C3.
"""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
GCC = os.environ.get("HOST_GCC") or shutil.which("gcc") or str(Path(
    r"C:\Users\29538\AppData\Local\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin\gcc.exe"))


def function(source, name):
    import re
    match = re.search(r"^(?:static\s+)?(?:bool|void|int)\s+" + name + r"\([^;]*?\)\s*\{", source, re.M)
    if not match:
        raise AssertionError("Missing BLE lifecycle function: " + name)
    start = match.start()
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


class BleMemoryLifecycleTests(unittest.TestCase):
    def compile_run(self, source):
        with tempfile.TemporaryDirectory(prefix="ble_lifecycle_", dir=ROOT / "tmp") as folder:
            path = Path(folder)
            cfile, exe = path / "test.c", path / "test.exe"
            cfile.write_text(source, encoding="utf-8")
            env = os.environ.copy()
            env["PATH"] = str(Path(GCC).parent) + os.pathsep + env.get("PATH", "")
            env["TMP"] = env["TEMP"] = folder
            subprocess.run([GCC, "-std=c11", "-Wall", "-Wextra", "-Werror", str(cfile), "-o", str(exe)],
                           check=True, timeout=30, env=env)
            result = subprocess.run([str(exe)], text=True, capture_output=True, timeout=10, env=env)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            print(result.stdout.strip())

    def test_provisioning_stop_free_restore_retry_and_no_host_access_while_suspended(self):
        source = (ROOT / "components/ble_prov/ble_prov.c").read_text(encoding="utf-8")
        names = ("ble_prov_host_acquire", "ble_prov_host_release", "reset_host_handles",
                 "ble_prov_start", "ble_prov_stop", "ble_prov_resume",
                 "ble_prov_suspend_for_ota", "ble_prov_restore_after_ota", "ble_prov_notify_status")
        functions = "\n".join(function(source, name) for name in names)
        declarations = source[source.index("static BleProvState"):source.index("/* 服务 UUID")]
        self.assertNotIn("esp_bt_controller_mem_release", functions)
        self.assertNotIn("esp_bt_mem_release", functions)
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
typedef enum { BLE_PROV_IDLE, BLE_PROV_RUNNING, BLE_PROV_DONE, BLE_PROV_ERROR } BleProvState;
typedef void (*BleProvCallback)(const char *, const char *);
typedef void (*BleProvConnCallback)(uint16_t);
typedef void (*BlePreHostStartCb)(void);
typedef void (*BleHostResetCb)(void);
typedef int (*BleGapEventForwardCb)(void *, void *);
typedef int esp_err_t;
typedef int *SemaphoreHandle_t;
static int mutex_storage, locked, init_calls, stop_calls, deinit_calls, host_delete_calls;
static int stack_live, host_live, stop_error, init_error, gatt_error, delete_order, reset_calls;
static int synced_calls, adv_calls, notify_calls, conn_callbacks;
#define ESP_OK 0
#define BLE_HS_CONN_HANDLE_NONE 65535
#define BLE_ERR_REM_USER_CONN_TERM 22
#define BLE_SM_IO_CAP_NO_IO 0
#define BLE_SM_PAIR_KEY_DIST_ENC 1
#define BLE_SM_PAIR_KEY_DIST_ID 2
#define pdTRUE 1
#define portMAX_DELAY 999
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
static SemaphoreHandle_t xSemaphoreCreateMutex(void) { return &mutex_storage; }
static int xSemaphoreTake(SemaphoreHandle_t m, int timeout) { assert(m); (void)timeout; assert(!locked); locked=1; return pdTRUE; }
static void xSemaphoreGive(SemaphoreHandle_t m) { assert(m && locked); locked=0; }
static size_t strlcpy(char *dst, const char *src, size_t n) {
    assert(dst != src); size_t len=strlen(src); if(n) { size_t copy=len<n-1?len:n-1; memcpy(dst,src,copy); dst[copy]=0; } return len;
}
static const char *esp_err_to_name(int error) { (void)error; return "failure"; }
static int nimble_port_init(void) { init_calls++; if(init_error) return init_error; assert(!stack_live); stack_live=1; return 0; }
static int nimble_port_stop(void) {
    stop_calls++; assert(stack_live && host_live && !locked);
    assert(!ble_prov_host_acquire());
    return stop_error;
}
static void nimble_port_freertos_deinit(void) { assert(host_live); host_delete_calls++; host_live=0; delete_order=1; }
static int nimble_port_deinit(void) { assert(stack_live && !host_live); assert(delete_order || !stop_calls); deinit_calls++; stack_live=0; return 0; }
static void nimble_host_reset(int reason) { (void)reason; }
static void nimble_host_sync(void) {}
static void nimble_host_task(void *p) { (void)p; }
static struct { void (*reset_cb)(int); void (*sync_cb)(void); int sm_io_cap,sm_bonding,sm_mitm,sm_sc,sm_our_key_dist,sm_their_key_dist; } ble_hs_cfg;
static void nimble_port_freertos_init(void (*task)(void *)) { (void)task; assert(stack_live); host_live=1; }
static void ble_svc_gap_init(void) { assert(stack_live); }
static void ble_svc_gatt_init(void) { assert(stack_live); }
static int ble_svc_gap_device_name_set(const char *n) { assert(n && stack_live); return 0; }
static int s_gatt_svcs;
static int ble_gatts_count_cfg(void *p) { (void)p; assert(stack_live); return 0; }
static int ble_gatts_add_svcs(void *p) { (void)p; assert(stack_live); return gatt_error; }
static int ble_hs_synced(void) { assert(stack_live && host_live); synced_calls++; return 1; }
static int ble_gap_terminate(int conn,int reason) { (void)conn;(void)reason; assert(stack_live); return 0; }
static int ble_gap_adv_stop(void) { assert(stack_live); return 0; }
static void start_advertising(void) { assert(stack_live && host_live); adv_calls++; }
struct os_mbuf { int unused; };
static struct os_mbuf mbuf;
static struct os_mbuf *ble_hs_mbuf_from_flat(const void *p,size_t len) { (void)p;(void)len; assert(stack_live && host_live); return &mbuf; }
static int ble_gattc_notify_custom(int c,int h,struct os_mbuf *m) { (void)c;(void)h;(void)m; assert(stack_live && host_live); notify_calls++; return 0; }
static void reset_callback(void) { reset_calls++; }
static void connection_callback(uint16_t c) { assert(c==BLE_HS_CONN_HANDLE_NONE); conn_callbacks++; }
bool ble_prov_host_acquire(void);
void ble_prov_resume(void);
DECLARATIONS
static uint16_t s_sn_val_handle,s_fw_val_handle,s_mac_val_handle,s_ssid_val_handle,s_pass_val_handle;
static uint16_t s_ota_ctrl_handle,s_ota_data_handle,s_ota_status_handle;
FUNCTIONS
int main(void) {
    (void)s_callback; (void)s_own_addr_type; (void)s_gap_forward_cb; (void)esp_err_to_name;
    s_host_reset_cb=reset_callback; s_conn_cb=connection_callback;
    assert(ble_prov_suspend_for_ota()); assert(ble_prov_restore_after_ota()); assert(init_calls==0);
    assert(ble_prov_start("SN123","0.6.19")); assert(init_calls==1);
    s_conn_handle=7; s_status_val_handle=23; s_ota_data_handle=42;
    assert(ble_prov_host_acquire()); ble_prov_host_release();
    assert(ble_prov_suspend_for_ota()); assert(!stack_live && !host_live);
    assert(host_delete_calls==1 && deinit_calls==1 && reset_calls>=1);
    assert(s_ota_data_handle==0 && s_conn_handle==BLE_HS_CONN_HANDLE_NONE);
    int sync_before=synced_calls; ble_prov_notify_status("failed"); ble_prov_resume(); ble_prov_stop();
    assert(!ble_prov_host_acquire()); assert(synced_calls==sync_before && notify_calls==0 && adv_calls==0);
    assert(ble_prov_suspend_for_ota()); assert(stop_calls==1);
    init_error=1; assert(!ble_prov_restore_after_ota()); assert(!stack_live);
    init_error=0; assert(ble_prov_restore_after_ota());
    assert(!strcmp(s_device_sn,"SN123") && !strcmp(s_fw_version,"0.6.19"));
    assert(ble_prov_restore_after_ota()); assert(init_calls==3);
    stop_error=1; assert(!ble_prov_suspend_for_ota()); assert(stack_live && host_live && deinit_calls==1);
    stop_error=0; assert(ble_prov_restore_after_ota()); assert(ble_prov_host_acquire()); ble_prov_host_release();
    assert(ble_prov_suspend_for_ota()); gatt_error=1; assert(!ble_prov_restore_after_ota()); assert(!stack_live);
    gatt_error=0; assert(ble_prov_restore_after_ota());
    assert(ble_prov_suspend_for_ota()); assert(ble_prov_restore_after_ota());
    ble_prov_stop(); assert(!s_advertising_enabled && s_state==BLE_PROV_IDLE);
    assert(ble_prov_suspend_for_ota()); assert(ble_prov_restore_after_ota());
    int before_resume=adv_calls;
    ble_prov_resume(); assert(s_advertising_enabled && adv_calls==before_resume+1);
    assert(!locked && s_conn_cb==connection_callback && s_host_reset_cb==reset_callback);
    puts("PASS: BLE reversible stop/deinit/reinit, retries, identity and host exclusion");
}
'''.replace("DECLARATIONS", declarations).replace("FUNCTIONS", functions)
        # The mock verifies that stop callbacks cannot take the closed producer gate.
        harness = harness.replace("static int nimble_port_stop(void)", "bool ble_prov_host_acquire(void);\nstatic int nimble_port_stop(void)")
        harness = harness.replace("ble_gatts_count_cfg(s_gatt_svcs)", "ble_gatts_count_cfg(&s_gatt_svcs)")
        harness = harness.replace("ble_gatts_add_svcs(s_gatt_svcs)", "ble_gatts_add_svcs(&s_gatt_svcs)")
        self.compile_run(harness)

    def test_control_services_reregister_without_stale_handles_or_subscriptions(self):
        source = (ROOT / "components/ble_ct/ble_ct.c").read_text(encoding="utf-8")
        reset = function(source, "ble_ct_reset_host")
        register = function(source, "ble_ct_register_services")
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#define BLE_HS_CONN_HANDLE_NONE 65535
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
static bool s_services_registered,s_telemetry_subscribed,s_result_subscribed;
static uint16_t s_conn_handle,s_telemetry_handle,s_cmd_handle,s_result_handle,s_info_handle;
static char s_session_id[33];
static int s_ct_gatt_svcs,registration_count;
static int ble_gatts_count_cfg(int svc) { (void)svc; return 0; }
static int ble_gatts_add_svcs(int svc) { (void)svc; registration_count++; return 0; }
RESET
REGISTER
int main(void) {
    ble_ct_register_services(); ble_ct_register_services(); assert(registration_count==1);
    s_conn_handle=7; s_telemetry_handle=11; s_cmd_handle=12; s_result_handle=13; s_info_handle=14;
    s_telemetry_subscribed=s_result_subscribed=true; s_session_id[0]='x';
    ble_ct_reset_host(); assert(s_conn_handle==BLE_HS_CONN_HANDLE_NONE && !s_session_id[0]);
    assert(!s_telemetry_handle && !s_cmd_handle && !s_result_handle && !s_info_handle);
    assert(!s_telemetry_subscribed && !s_result_subscribed);
    ble_ct_register_services(); assert(registration_count==2);
    puts("PASS: fresh CT services, handles and subscriptions after BLE host restoration");
}
'''.replace("RESET", reset).replace("REGISTER", register)
        self.compile_run(harness)

    def test_background_notifications_hold_gate_and_skip_closed_host(self):
        telemetry = (ROOT / "components/ble_ct/ble_ct_telemetry.c").read_text(encoding="utf-8")
        command = (ROOT / "components/ble_ct/ble_ct_cmd.c").read_text(encoding="utf-8")
        ota = (ROOT / "main/ota/ble_ota.c").read_text(encoding="utf-8")
        functions = "\n".join(function(telemetry, name) for name in
                              ("ble_ct_telemetry_set_snapshot", "ble_ct_telemetry_should_push", "send_frame", "ble_ct_telemetry_notify"))
        functions += "\n" + function(command, "ble_ct_cmd_notify_result")
        functions += "\n" + function(ota, "notify_status")
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define ESP_LOGW(...) ((void)0)
#define SNAP_MAX 1024
#define RESULT_CAPACITY 2048
#define MAX_FRAME_PAYLOAD 509
#define MAX_FRAMES 8
#define FRAME_CTRL_FIRST 0x80
#define FRAME_CTRL_LAST 0x40
#define FRAME_CTRL_SINGLE 0xC0
#define BLE_HS_CONN_HANDLE_NONE 65535
static char s_snapshot[SNAP_MAX];
static uint16_t s_snapshot_len;
static bool gate_open,held,subscribed=true;
static unsigned allocated,notified,acquired,released;
static uint16_t connection=7;
static uint16_t s_conn=7;
static char s_status[]="{}";
static bool ble_prov_host_acquire(void) { if(!gate_open)return false; assert(!held); held=true; acquired++; return true; }
static void ble_prov_host_release(void) { assert(held); held=false; released++; }
static bool ble_ct_is_telemetry_subscribed(void) { return subscribed; }
static uint16_t ble_ct_get_conn_handle(void) { return connection; }
static uint16_t ble_ct_get_telemetry_handle(void) { return 11; }
static uint16_t ble_ct_get_result_handle(void) { return 12; }
static uint16_t ble_prov_get_ota_status_handle(void) { return 13; }
struct os_mbuf { int unused; };
static struct os_mbuf mbuf;
static struct os_mbuf *ble_hs_mbuf_from_flat(const void *p,unsigned len) { (void)p;(void)len; assert(held && gate_open); allocated++; return &mbuf; }
static void os_mbuf_free_chain(struct os_mbuf *p) { (void)p; assert(held); }
static void os_mbuf_concat(struct os_mbuf *p,struct os_mbuf *q) { (void)p;(void)q; assert(held); }
static int ble_gattc_notify_custom(int conn,int handle,struct os_mbuf *p) { (void)conn;(void)handle;(void)p; assert(held && gate_open); notified++; return 0; }
FUNCTIONS
int main(void) {
    (void)ble_prov_host_acquire; (void)ble_prov_host_release;
    char long_json[5000]; memset(long_json,'x',sizeof(long_json));
    ble_ct_telemetry_notify("{}",2); ble_ct_cmd_notify_result("{}",2); notify_status(); assert(!allocated && !notified);
    gate_open=true;
    ble_ct_telemetry_notify("{}",2); ble_ct_telemetry_notify(long_json,600); ble_ct_cmd_notify_result("{}",2);
    assert(notified==4 && !held && acquired==released);
    notify_status(); assert(notified==5 && !held && acquired==released);
    unsigned count=notified;
    subscribed=false; ble_ct_telemetry_notify("{}",2); subscribed=true;
    ble_ct_telemetry_notify(long_json,5000);
    connection=BLE_HS_CONN_HANDLE_NONE; ble_ct_telemetry_notify("{}",2); ble_ct_cmd_notify_result("{}",2);
    s_conn=BLE_HS_CONN_HANDLE_NONE; notify_status();
    assert(notified==count && !held && acquired==released);
    puts("PASS: telemetry and command notifications drain before host deinit and skip the stopped host");
}
'''.replace("FUNCTIONS", functions)
        self.compile_run(harness)


if __name__ == "__main__":
    unittest.main()
