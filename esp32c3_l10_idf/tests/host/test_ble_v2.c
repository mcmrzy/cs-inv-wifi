/* Real BLE lifecycle, AUTH/COMMAND/TELEMETRY and Mbed TLS/cJSON sources.
 * Only radio, storage, time, PIN binding and executors are host fixtures. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ble_ct.h"
#include "ble_ct_auth.h"
#include "ble_ct_cmd.h"
#include "ble_ct_telemetry.h"
#include "host/ble_gap.h"
#include "nvs.h"
#include "mbedtls/md.h"
#include "mbedtls/base64.h"
#include "cJSON.h"

static int failures, writes, notifications;
static uint16_t notified_conn;
static int64_t uptime_us = 1000000;
static uint32_t wall_time = 1789459200;
static unsigned random_counter;
static char response[2048];
static const struct ble_gatt_svc_def *services;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); ++failures; } } while (0)
void test_log(const char *tag, const char *format, ...) { (void)tag; (void)format; }
int64_t esp_timer_get_time(void) { return uptime_us; }
uint32_t ble_ct_now(void) { return wall_time; }
void ble_ct_calibrate(uint32_t ts) { (void)ts; CHECK(0); }
void ble_ct_clock_init(void) {}
void esp_fill_random(void *buf, size_t len) {
    uint8_t *out = buf;
    for (size_t i = 0; i < len; ++i) out[i] = (uint8_t)random_counter++;
}
int esp_read_mac(uint8_t *mac, int type) { (void)type; memset(mac, 1, 6); return 0; }
bool ble_ct_pin_verify(const char *sn, const char *pin, const uint8_t *addr) {
    (void)sn; (void)pin; (void)addr; return false;
}
int ble_gap_conn_find(uint16_t conn, struct ble_gap_conn_desc *desc) { (void)conn; memset(desc, 0, sizeof(*desc)); return 0; }
int ble_gap_terminate(uint16_t conn, uint8_t reason) { (void)conn; (void)reason; return 0; }
int ble_store_util_delete_peer(const void *addr) { (void)addr; return 0; }
int ble_gatts_count_cfg(const struct ble_gatt_svc_def *svcs) { (void)svcs; return 0; }
int ble_gatts_add_svcs(const struct ble_gatt_svc_def *svcs) {
    services = svcs;
    for (int i = 0; svcs[0].characteristics[i].uuid; ++i)
        *svcs[0].characteristics[i].val_handle = (uint16_t)(10 + i);
    return 0;
}
int ble_hs_mbuf_to_flat(const struct os_mbuf *om, void *dst, uint16_t max_len, uint16_t *out_len) {
    if (om->len > max_len || max_len >= 512) return 1;
    memcpy(dst, om->data, om->len);
    if (out_len) *out_len = om->len;
    return 0;
}
struct os_mbuf *ble_hs_mbuf_from_flat(const void *data, uint16_t len) {
    static struct os_mbuf om; om.data = data; om.len = len; return &om;
}
int os_mbuf_append(struct os_mbuf *om, const void *data, uint16_t len) {
    (void)om;
    if (len >= sizeof(response)) return 1;
    memcpy(response, data, len); response[len] = 0; return 0;
}
void os_mbuf_free_chain(struct os_mbuf *om) { (void)om; }
void os_mbuf_concat(struct os_mbuf *a, struct os_mbuf *b) { (void)a; (void)b; }
int ble_gattc_notify_custom(uint16_t conn, uint16_t handle, struct os_mbuf *om) {
    (void)handle; notified_conn = conn; ++notifications;
    return os_mbuf_append(NULL, om->data, om->len);
}
int nvs_open(const char *s, int mode, nvs_handle_t *h) { (void)s; (void)mode; *h=1; return ESP_OK; }
int nvs_get_blob(nvs_handle_t h, const char *s, void *p, size_t *n) {
    (void)h; (void)s;
    for(size_t i=0;i<*n;++i) ((uint8_t *)p)[i]=(uint8_t)i;
    return ESP_OK;
}
int nvs_set_blob(nvs_handle_t h, const char *s, const void *p, size_t n) { (void)h; (void)s; (void)p; (void)n; return ESP_OK; }
int nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }
void nvs_close(nvs_handle_t h) { (void)h; }
static int write_param(const char *name, double value) { (void)name; (void)value; ++writes; return BLE_CMD_OK; }
static int read_param(const char *name, double *out) { (void)name; *out=12; return BLE_CMD_OK; }
static int access(uint16_t conn, int index, const char *json) {
    struct os_mbuf om = {json, json ? (uint16_t)strlen(json) : 0};
    struct ble_gatt_access_ctxt ctxt = {json ? BLE_GATT_ACCESS_OP_WRITE_CHR : BLE_GATT_ACCESS_OP_READ_CHR, &om};
    response[0]=0;
    struct ble_gatt_chr_def *chr = &services[0].characteristics[index];
    return chr->access_cb(conn, *chr->val_handle, &ctxt, NULL);
}
static void connect(uint16_t conn) {
    struct ble_gap_event ev = {0}; ev.type=BLE_GAP_EVENT_CONNECT; ev.connect.conn_handle=conn;
    ble_ct_on_gap_event(&ev, NULL);
}
static void disconnect(uint16_t conn) {
    struct ble_gap_event ev = {0}; ev.type=BLE_GAP_EVENT_DISCONNECT; ev.disconnect.conn.conn_handle=conn;
    ble_ct_on_gap_event(&ev, NULL);
}
static int session(char *out, size_t size) {
    CHECK(access(ble_ct_get_conn_handle(), 4, NULL)==0);
    cJSON *root=cJSON_Parse(response), *item=cJSON_GetObjectItem(root,"session_id");
    int ok=cJSON_IsString(item);
    CHECK(ok);
    if(ok) snprintf(out,size,"%s",item->valuestring);
    cJSON_Delete(root); return ok;
}
static void make_proof(const char *prefix, const char *sid, const uint8_t *phone,
                       const uint8_t *device, uint32_t ts, char *out) {
    uint8_t key[32], digest[32], msg[256]; size_t used=0, n=0;
    for(size_t i=0;i<32;++i) key[i]=(uint8_t)i;
    used=(size_t)snprintf((char *)msg,sizeof(msg),"%s%s|",prefix,sid);
    memcpy(msg+used,phone,16); used+=16; msg[used++]='|';
    memcpy(msg+used,device,16); used+=16; msg[used++]='|';
    used+=(size_t)snprintf((char *)msg+used,sizeof(msg)-used,"%lu",(unsigned long)ts);
    CHECK(mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),key,32,msg,used,digest)==0);
    CHECK(mbedtls_base64_encode((unsigned char *)out,45,&n,digest,32)==0);
}
static int init(char *sid, char *proof, char *device_proof, unsigned id) {
    char req[512]; uint8_t phone[16], device[16]; size_t n=0;
    for(size_t i=0;i<16;++i) phone[i]=(uint8_t)i;
    if(!session(sid,65)) return 0;
    snprintf(req,sizeof(req),"{\"v\":2,\"type\":\"auth.init\",\"message_id\":\"init-%u\",\"session_id\":\"%s\",\"body\":{\"phone_nonce\":\"AAECAwQFBgcICQoLDA0ODw==\"}}",id,sid);
    CHECK(access(ble_ct_get_conn_handle(),0,req)==0);
    CHECK(!ble_ct_auth_is_authenticated());
    cJSON *root=cJSON_Parse(response), *body=cJSON_GetObjectItem(root,"body");
    cJSON *nonce=cJSON_GetObjectItem(body,"device_nonce"), *ts=cJSON_GetObjectItem(body,"device_ts");
    int ok=cJSON_IsString(nonce)&&cJSON_IsNumber(ts); CHECK(ok);
    if(ok) {
        CHECK(mbedtls_base64_decode(device,16,&n,(unsigned char *)nonce->valuestring,strlen(nonce->valuestring))==0 && n==16);
        make_proof("CSIV-CT/v2/app|",sid,phone,device,(uint32_t)ts->valuedouble,proof);
        make_proof("CSIV-CT/v2/device|",sid,phone,device,(uint32_t)ts->valuedouble,device_proof);
    }
    cJSON_Delete(root); return ok;
}
static int prove(uint16_t conn, const char *sid, const char *proof, unsigned id) {
    char req[512];
    snprintf(req,sizeof(req),"{\"v\":2,\"type\":\"auth.proof\",\"message_id\":\"proof-%u\",\"session_id\":\"%s\",\"body\":{\"phone_proof\":\"%s\"}}",id,sid,proof);
    return access(conn,0,req);
}
int main(void) {
    ble_ct_start("TEST-L10-0001","FW-TEST"); ble_ct_register_services();
    ble_ct_cmd_set_executors(write_param,read_param); connect(1);
    CHECK(access(1,0,"{\"mode\":\"auth\",\"nonce\":\"AAECAwQFBgcICQoLDA0ODw==\",\"ts\":1789459200}")!=0);
    CHECK(!ble_ct_auth_is_authenticated());
    CHECK(access(1,1,NULL)==BLE_ATT_ERR_INSUFFICIENT_AUTHEN);
    CHECK(access(1,2,"{\"command_id\":\"denied\",\"action\":\"power_on\"}")==BLE_ATT_ERR_INSUFFICIENT_AUTHEN);
    CHECK(writes==0);
    char sid[65], proof[45], device_proof[45];
    if(init(sid,proof,device_proof,1)) {
        char wrong[45]; strcpy(wrong,proof); wrong[0]=wrong[0]=='A'?'B':'A';
        CHECK(prove(1,sid,wrong,1)==0); CHECK(!ble_ct_auth_is_authenticated());
        CHECK(prove(1,sid,proof,2)==0); CHECK(!ble_ct_auth_is_authenticated());
        CHECK(init(sid,proof,device_proof,2));
        CHECK(prove(1,sid,proof,3)==0); CHECK(ble_ct_auth_is_authenticated());
        CHECK(strstr(response,device_proof)!=NULL); CHECK(notified_conn==1);
        char command[512];
        snprintf(command,sizeof(command),
            "{\"v\":2,\"type\":\"control.request\",\"message_id\":\"cmd-msg-1\","
            "\"session_id\":\"%s\",\"body\":{\"operation_id\":\"op-1\","
            "\"command_id\":\"cmd-1\",\"session_id\":\"%s\",\"sequence\":1,"
            "\"action\":\"set_param\",\"params\":{\"param_id\":\"output_priority\",\"value\":1}}}",
            sid,sid);
        CHECK(access(1,2,command)==0); CHECK(writes==1);
        CHECK(strstr(response,"\"type\":\"control.result\"")!=NULL);
        CHECK(strstr(response,"\"status\":\"accepted\"")!=NULL);
        ble_ct_cmd_on_write_ack("output_priority",0);
        ble_ct_cmd_on_write_readback("output_priority",1.0,7);
        CHECK(strstr(response,"\"status\":\"applied\"")!=NULL);
        CHECK(strstr(response,"\"parameter_revision\":7")!=NULL);
        CHECK(access(1,2,command)==0); CHECK(writes==1);
        CHECK(strstr(response,"\"status\":\"applied\"")!=NULL);
        char conflict[512];
        snprintf(conflict,sizeof(conflict),
            "{\"v\":2,\"type\":\"control.request\",\"message_id\":\"cmd-msg-2\","
            "\"session_id\":\"%s\",\"body\":{\"operation_id\":\"op-2\","
            "\"command_id\":\"cmd-1\",\"session_id\":\"%s\",\"sequence\":2,"
            "\"action\":\"set_param\",\"params\":{\"param_id\":\"output_priority\",\"value\":0}}}",
            sid,sid);
        CHECK(access(1,2,conflict)==0); CHECK(writes==1);
        CHECK(strstr(response,"IDEMPOTENCY_CONFLICT")!=NULL);
        CHECK(access(2,1,NULL)==BLE_ATT_ERR_INSUFFICIENT_AUTHEN);
        CHECK(access(2,2,"{\"command_id\":\"other\",\"action\":\"power_on\"}")==BLE_ATT_ERR_INSUFFICIENT_AUTHEN);
        CHECK(writes==1);
        CHECK(prove(1,sid,proof,3)==0); CHECK(!ble_ct_auth_is_authenticated());
        CHECK(init(sid,proof,device_proof,3)); uptime_us+=31000000;
        CHECK(prove(1,sid,proof,4)==0); CHECK(!ble_ct_auth_is_authenticated());
        CHECK(init(sid,proof,device_proof,4));
        char old_sid[65]; strcpy(old_sid,sid); connect(2);
        CHECK(!ble_ct_auth_is_authenticated());
        CHECK(prove(2,old_sid,proof,5)!=0); CHECK(!ble_ct_auth_is_authenticated());
        CHECK(init(sid,proof,device_proof,5)); CHECK(strcmp(old_sid,sid)!=0);
        CHECK(prove(2,sid,proof,6)==0); CHECK(ble_ct_auth_is_authenticated());
        disconnect(1); CHECK(ble_ct_auth_is_authenticated());
        disconnect(2); CHECK(!ble_ct_auth_is_authenticated());
        connect(2); CHECK(!ble_ct_auth_is_authenticated());
        CHECK(prove(2,sid,proof,7)!=0); CHECK(!ble_ct_auth_is_authenticated());
    }
    printf("ble_v2: %s (%d failed assertions)\n",failures?"FAIL":"PASS",failures);
    return failures?EXIT_FAILURE:EXIT_SUCCESS;
}
