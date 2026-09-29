/* COMMAND 回调 + 固件 cJSON 的真实实现; 只替换平台边界。
 * 2026-09-22: 鉴权协议已拆除 (ble_ct_auth.c/ble_ct_pin.c 删除), 本测试随之
 * 只覆盖控制路径 —— 并新增一条"无鉴权即可控制"的断言, 因为 App 侧 checkPin
 * 已无调用点, 原鉴权门只会把 App 的控制请求全部拒掉。 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "ble_ct_cmd.h"
#include "host/ble_gatt.h"
#include "nvs.h"
#include "mbedtls/md.h"
#include "cJSON.h"

static int failures, writes, flatten_calls, flatten_error, read_rc;
static int oversized_notify;
static double read_value = 12.5;
static char response[1024], logs[4096];
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); ++failures; } } while (0)
void test_log(const char *tag, const char *format, ...) {
    (void)tag;
    va_list args; va_start(args, format);
    size_t n = strlen(logs);
    vsnprintf(logs + n, sizeof(logs) - n, format, args);
    va_end(args);
}
int64_t esp_timer_get_time(void) { return 1000000; }
uint16_t ble_ct_get_conn_handle(void) { return 1; }
uint16_t ble_ct_get_result_handle(void) { return 2; }
const char *ble_ct_get_sn(void) { return "TEST-SN"; }
const char *ble_ct_get_session_id(void) { return "host-session"; }
void esp_fill_random(void *buf, size_t len) { memset(buf, 7, len); }
uint32_t ble_ct_now(void) { return 0; }
void ble_ct_calibrate(uint32_t ts) { (void)ts; }
void ble_ct_clock_init(void) {}
int ble_gap_conn_find(uint16_t conn, struct ble_gap_conn_desc *desc) { (void)conn; memset(desc, 0, sizeof(*desc)); return 0; }
int ble_hs_mbuf_to_flat(const struct os_mbuf *om, void *dst, uint16_t max_len, uint16_t *out_len) {
    ++flatten_calls;
    if (flatten_error) return flatten_error;
    /* Prevent the old vulnerability from corrupting the host runner itself. */
    if (max_len >= 512) return 1;
    if (om->len > max_len) return 1;
    memcpy(dst, om->data, om->len);
    if (out_len) *out_len = om->len;
    return 0;
}
struct os_mbuf *ble_hs_mbuf_from_flat(const void *data, uint16_t len) {
    if (len >= 512) { ++oversized_notify; return NULL; }
    static struct os_mbuf om; om.data = data; om.len = len; return &om;
}
int ble_gattc_notify_custom(uint16_t conn, uint16_t handle, struct os_mbuf *om) {
    (void)conn; (void)handle;
    size_t n = om->len < sizeof(response)-1 ? om->len : sizeof(response)-1;
    memcpy(response, om->data, n); response[n] = 0; return 0;
}
int nvs_open(const char *s, int mode, nvs_handle_t *h) { (void)s; (void)mode; *h=1; return ESP_OK; }
int nvs_get_blob(nvs_handle_t h, const char *s, void *p, size_t *n) { (void)h; (void)s; memset(p, 1, *n); return ESP_OK; }
int nvs_set_blob(nvs_handle_t h, const char *s, const void *p, size_t n) { (void)h; (void)s; (void)p; (void)n; return ESP_OK; }
int nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }
void nvs_close(nvs_handle_t h) { (void)h; }
int mbedtls_base64_decode(unsigned char *out, size_t cap, size_t *n, const unsigned char *in, size_t len) {
    (void)in; (void)len; memset(out, 1, cap); *n=cap; return 0;
}
int mbedtls_base64_encode(unsigned char *out, size_t cap, size_t *n, const unsigned char *in, size_t len) {
    (void)cap; (void)in; (void)len; memcpy(out, "fake", 4); *n=4; return 0;
}
const mbedtls_md_info_t *mbedtls_md_info_from_type(int type) { static int md; (void)type; return &md; }
int mbedtls_md_hmac(const mbedtls_md_info_t *md, const unsigned char *key, size_t kl, const unsigned char *msg, size_t ml, unsigned char *out) {
    (void)md; (void)key; (void)kl; (void)msg; (void)ml; memset(out, 1, 32); return 0;
}
static int write_param(const char *name, double value) { (void)name; (void)value; ++writes; return BLE_CMD_OK; }
static int read_param(const char *name, double *out) { (void)name; *out=read_value; return read_rc; }
static int invoke(const char *json, size_t len) {
    struct os_mbuf om = {json, (uint16_t)len};
    struct ble_gatt_access_ctxt ctxt = {BLE_GATT_ACCESS_OP_WRITE_CHR, &om};
    response[0]=0; logs[0]=0;
    return ble_ct_cmd_access_cb(1, 2, &ctxt, NULL);
}
static int send(const char *json) { return invoke(json, strlen(json)); }
static void expect_status(const char *status) {
    cJSON *root=cJSON_Parse(response);
    CHECK(root != NULL);
    cJSON *item=cJSON_GetObjectItem(root, "status");
    CHECK(cJSON_IsString(item) && strcmp(item->valuestring,status)==0);
    cJSON_Delete(root);
}
int main(void) {
    ble_ct_cmd_set_executors(write_param, read_param);

    /* 无鉴权即可控制 (2026-09-22): 原 INSUFFICIENT_AUTHEN 门已随鉴权协议移除。
     * 非写操作仍按原样拒绝 (READ 走 telemetry/info 特征, 控制特征只接受写)。 */
    {
        struct os_mbuf om = {"{}", 2};
        struct ble_gatt_access_ctxt read_ctxt = {BLE_GATT_ACCESS_OP_READ_CHR, &om};
        CHECK(ble_ct_cmd_access_cb(1, 2, &read_ctxt, NULL) == BLE_ATT_ERR_UNLIKELY);
    }

    const char *v2_command =
        "{\"v\":2,\"type\":\"control.request\",\"message_id\":\"msg-v2-1\","
        "\"session_id\":\"host-session\",\"body\":{\"operation_id\":\"op-v2-1\","
        "\"command_id\":\"cmd-v2-1\",\"session_id\":\"host-session\","
        "\"sequence\":1,\"action\":\"set_param\",\"params\":{"
        "\"param_id\":\"charge\",\"value\":12.5}}}";
    writes=0;
    CHECK(send(v2_command)==0); CHECK(writes==1);
    CHECK(strstr(response,"\"status\":\"accepted\"")!=NULL);
    ble_ct_cmd_on_write_ack("charge",0);
    ble_ct_cmd_on_write_readback("charge",12.5,9);
    CHECK(strstr(response,"\"status\":\"applied\"")!=NULL);
    CHECK(strstr(response,"\"parameter_revision\":9")!=NULL);
    CHECK(send(v2_command)==0); CHECK(writes==1);
    CHECK(strstr(response,"\"status\":\"applied\"")!=NULL);

    read_rc=BLE_CMD_OK;
    CHECK(send("{\"command_id\":\"read-ok\",\"action\":\"get_param\",\"params\":{\"param_id\":\"charge\"}}")==0);
    expect_status("ok"); CHECK(strstr(response,"12.5") != NULL);
    read_rc=BLE_CMD_NOT_READABLE;
    CHECK(send("{\"command_id\":\"read-bad\",\"action\":\"get_param\",\"params\":{\"param_id\":\"charge\"}}")==0);
    expect_status("error"); CHECK(strstr(response,"\"value\"")==NULL);
    read_rc=BLE_CMD_OK; read_value=NAN;
    send("{\"command_id\":\"read-nan\",\"action\":\"get_param\",\"params\":{\"param_id\":\"charge\"}}");
    expect_status("error");
    writes=0;
    send("{\"command_id\":\"power\",\"action\":\"set_power\",\"params\":{\"power_w\":100}}");
    CHECK(writes==0); CHECK(strstr(response,"UNKNOWN_ACTION")!=NULL);
    send("{\"command_id\":\"infinite\",\"action\":\"set_param\",\"params\":{\"param_id\":\"charge\",\"value\":1e999}}");
    CHECK(writes==0); expect_status("error");

    {
        char large[600]; memset(large,' ',sizeof(large));
        int calls=flatten_calls;
        CHECK(invoke(large,512)==BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN);
        CHECK(flatten_calls==calls);
        CHECK(invoke(large,sizeof(large))==BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN);
        CHECK(flatten_calls==calls);
        const char *valid="{\"command_id\":\"nul\",\"action\":\"unknown\"}";
        memcpy(large,valid,strlen(valid));
        CHECK(invoke(large,511)==0);
        flatten_error=1;
        CHECK(send(valid)==BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN);
        flatten_error=0;
        char nul[128]; size_t n=strlen(valid); memcpy(nul,valid,n+1); memcpy(nul+n+1,"junk",4);
        CHECK(invoke(nul,n+5)==BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN);
        snprintf(nul,sizeof(nul),"%s garbage",valid);
        CHECK(send(nul)==BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN);
        CHECK(invoke(valid,0)==BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN);
    }
    writes=0;
    CHECK(send("{\"command_id\":\"bad-ts\",\"action\":\"set_param\",\"ts\":1e999,\"params\":{\"param_id\":\"charge\",\"value\":1}}")==BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN);
    CHECK(writes==0);
    char long_id[401], request[512]; memset(long_id,'x',400); long_id[400]=0;
    snprintf(request,sizeof(request),"{\"command_id\":\"%s\",\"action\":\"power_off\"}",long_id);
    CHECK(send(request)==BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN);
    CHECK(writes==0); CHECK(oversized_notify==0);
    const char *bad_tokens[] = {"power_off\\u0000suffix", "bad\\\"id", "bad\\nline", "bad\\\\slash", "", "bad space"};
    for (size_t i=0;i<sizeof(bad_tokens)/sizeof(bad_tokens[0]);++i) {
        snprintf(request,sizeof(request),"{\"command_id\":\"%s\",\"action\":\"power_off\"}",bad_tokens[i]);
        CHECK(send(request)==BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN);
        snprintf(request,sizeof(request),"{\"command_id\":\"tok-action-%u\",\"action\":\"%s\"}",(unsigned)i,bad_tokens[i]);
        CHECK(send(request)==BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN);
        snprintf(request,sizeof(request),"{\"command_id\":\"tok-param-%u\",\"action\":\"set_param\",\"params\":{\"param_id\":\"%s\",\"value\":1}}",(unsigned)i,bad_tokens[i]);
        CHECK(send(request)==BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN);
    }
    CHECK(writes==0);
    const char *bad_ts[]={"1.5","4294967296","-1","\"1\"","null","true"};
    for (size_t i=0;i<sizeof(bad_ts)/sizeof(bad_ts[0]);++i) {
        snprintf(request,sizeof(request),"{\"command_id\":\"ts-%u\",\"action\":\"power_off\",\"ts\":%s}",(unsigned)i,bad_ts[i]);
        CHECK(send(request)==BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN);
    }
    CHECK(writes==0);
    read_value=INFINITY;
    send("{\"command_id\":\"read-inf\",\"action\":\"get_param\",\"params\":{\"param_id\":\"charge\"}}");
    expect_status("error");
    long_id[64]=0;
    snprintf(request,sizeof(request),"{\"command_id\":\"%s\",\"action\":\"power_off\"}",long_id);
    CHECK(send(request)==0); CHECK(writes==1);
    CHECK(send(request)==0); CHECK(writes==1);
    /* 控制路径的任何日志都不得出现凭据类内容 (原 PIN 留痕红线, 现只覆盖控制) */
    CHECK(strstr(logs,"123456")==NULL);
    printf("ble_control: %s (%d failed assertions)\n",failures ? "FAIL":"PASS",failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
