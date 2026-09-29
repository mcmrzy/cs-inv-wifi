/* Host tests for main/ota/ble_ota.c.
 * Real OTA GATT adapter logic + real cJSON; FreeRTOS/GATT/OTA service are fakes. */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ble_ota.h"
#include "ble_ct.h"
#include "ble_prov.h"
#include "cJSON.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "mbedtls/base64.h"
#include "ota_service.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define CTRL_HANDLE 0x31
#define DATA_HANDLE 0x32
#define STATUS_HANDLE 0x33
#define QUEUE_DEPTH 2

#define CHECK(x) \
    do { \
        if (!(x)) { \
            fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); \
            ++failures; \
        } \
    } while (0)

static int failures;
static char logs[4096];
static char last_notify[1024];
static int notify_count;

/* ---- FreeRTOS queue / task stubs ---- */
struct HostQueue {
    UBaseType_t length;
    UBaseType_t item_size;
    UBaseType_t count;
    unsigned head;
    unsigned tail;
    unsigned char *storage;
};

static struct HostQueue g_queue;
static unsigned char g_queue_storage[QUEUE_DEPTH * 512];
static TaskFunction_t g_pending_task;
static void *g_pending_task_arg;
static TaskHandle_t g_pending_task_handle;
static int g_queue_create_attempts;
static int g_queue_create_failures_remaining;
static int g_queue_delete_calls;
static int g_task_create_attempts;
static int g_task_create_failures_remaining;
static int g_reboot_schedule_calls;
static uint32_t g_reboot_schedule_delay_ms;
void esp_restart(void) {}

QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t item_size) {
    ++g_queue_create_attempts;
    if (g_queue_create_failures_remaining-- > 0) return NULL;
    g_queue.length = length;
    g_queue.item_size = item_size;
    g_queue.count = 0;
    g_queue.head = 0;
    g_queue.tail = 0;
    g_queue.storage = g_queue_storage;
    return &g_queue;
}

void xQueueReset(QueueHandle_t queue) {
    queue->count = 0;
    queue->head = 0;
    queue->tail = 0;
}

void vQueueDelete(QueueHandle_t queue) {
    if (!queue) return;
    ++g_queue_delete_calls;
    queue->count = 0;
}

BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t ticks) {
    (void)ticks;
    if (queue->count >= queue->length) return pdFALSE;
    memcpy(queue->storage + queue->tail * queue->item_size, item, queue->item_size);
    queue->tail = (queue->tail + 1) % queue->length;
    queue->count++;
    return pdTRUE;
}

BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t ticks) {
    (void)ticks;
    if (queue->count == 0) return pdFALSE;
    memcpy(item, queue->storage + queue->head * queue->item_size, queue->item_size);
    queue->head = (queue->head + 1) % queue->length;
    queue->count--;
    return pdTRUE;
}

UBaseType_t uxQueueMessagesWaiting(QueueHandle_t queue) { return queue->count; }

BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack,
                       void *arg, UBaseType_t prio, TaskHandle_t *out) {
    (void)name;
    ++g_task_create_attempts;
    if (g_task_create_failures_remaining-- > 0) return pdFALSE;
    (void)stack;
    (void)prio;
    g_pending_task = fn;
    g_pending_task_arg = arg;
    g_pending_task_handle = (TaskHandle_t)(uintptr_t)0xA11E;
    if (out) *out = g_pending_task_handle;
    return pdPASS;
}

void vTaskDelete(TaskHandle_t handle) {
    (void)handle;
    g_pending_task_handle = NULL;
}

UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t handle) {
    (void)handle;
    return 1024;
}

void vTaskDelay(TickType_t ticks) { (void)ticks; }

void host_freertos_run_pending_task(void) {
    if (!g_pending_task) return;
    TaskFunction_t fn = g_pending_task;
    void *arg = g_pending_task_arg;
    g_pending_task = NULL;
    fn(arg);
}

void host_freertos_reset(void) {
    g_pending_task = NULL;
    g_pending_task_arg = NULL;
    g_pending_task_handle = NULL;
    if (g_queue.storage) {
        g_queue.count = 0;
        g_queue.head = 0;
        g_queue.tail = 0;
        memset(g_queue_storage, 0, sizeof(g_queue_storage));
    }
}

/* ---- platform / auth / prov ---- */
void test_log(const char *tag, const char *format, ...) {
    (void)tag;
    va_list args;
    va_start(args, format);
    size_t n = strlen(logs);
    vsnprintf(logs + n, sizeof(logs) - n, format, args);
    va_end(args);
}

const char *ble_ct_get_session_id(void) { return "host-session"; }
const char *ble_ct_get_sn(void) { return "HOST-SN"; }

static void *g_ota_access_cb;
void (*g_ota_conn_cb)(uint16_t conn_handle);
uint16_t ble_prov_get_ota_ctrl_handle(void) { return CTRL_HANDLE; }
uint16_t ble_prov_get_ota_data_handle(void) { return DATA_HANDLE; }
uint16_t ble_prov_get_ota_status_handle(void) { return STATUS_HANDLE; }
void ble_prov_set_ota_access_cb(void *cb) { g_ota_access_cb = cb; }
void *ble_prov_get_ota_access_cb(void) { return g_ota_access_cb; }
void ble_prov_set_conn_callback(void (*cb)(uint16_t conn_handle)) {
    g_ota_conn_cb = cb;
}

static char status_read_buf[1024];
int os_mbuf_append(struct os_mbuf *om, const void *data, uint16_t len) {
    (void)om;
    size_t n = len < sizeof(status_read_buf) - 1 ? len : sizeof(status_read_buf) - 1;
    memcpy(status_read_buf, data, n);
    status_read_buf[n] = '\0';
    return 0;
}

int ble_hs_mbuf_to_flat(const struct os_mbuf *om, void *dst, uint16_t max_len,
                        uint16_t *out_len) {
    if (om->len > max_len) return 1;
    memcpy(dst, om->data, om->len);
    if (out_len) *out_len = om->len;
    return 0;
}

struct os_mbuf *ble_hs_mbuf_from_flat(const void *data, uint16_t len) {
    static struct os_mbuf om;
    om.data = data;
    om.len = len;
    return &om;
}

int ble_gattc_notify_custom(uint16_t conn, uint16_t handle, struct os_mbuf *om) {
    (void)conn;
    (void)handle;
    size_t n = om->len < sizeof(last_notify) - 1 ? om->len : sizeof(last_notify) - 1;
    memcpy(last_notify, om->data, n);
    last_notify[n] = '\0';
    ++notify_count;
    return 0;
}

/* Minimal real-ish base64 decode for host payloads. */
int mbedtls_base64_decode(unsigned char *out, size_t cap, size_t *n,
                          const unsigned char *in, size_t len) {
    static const char *alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    unsigned char map[256];
    memset(map, 0xFF, sizeof(map));
    for (int i = 0; i < 64; ++i) map[(unsigned char)alphabet[i]] = (unsigned char)i;
    map[(unsigned char)'='] = 0xFE;

    size_t out_len = 0;
    unsigned acc = 0;
    int bits = 0;
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = in[i];
        if (c == '=' ) break;
        if (map[c] == 0xFF) return -1;
        acc = (acc << 6) | map[c];
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (out_len >= cap) return -1;
            out[out_len++] = (unsigned char)((acc >> bits) & 0xFF);
        }
    }
    *n = out_len;
    return 0;
}

int mbedtls_base64_encode(unsigned char *out, size_t cap, size_t *n,
                          const unsigned char *in, size_t len) {
    (void)out;
    (void)cap;
    (void)in;
    (void)len;
    *n = 0;
    return -1;
}

/* ---- ota_service stubs ---- */
static OtaStreamReader g_reader;
static void *g_reader_ctx;
static OtaManifest g_started_manifest;
static int g_start_calls;
static int g_cancel_calls;
static char g_started_task[64];
static int g_snapshot_valid;
static OtaSnapshot g_snapshot;
static OtaState g_start_outcome_state = OTA_STATE_REBOOTING;
static OtaStatusObserver g_observer;
static unsigned char g_received[2048];
static size_t g_received_len;

bool ota_service_init(void) { return true; }
void ota_service_set_observer(OtaStatusObserver observer) { g_observer = observer; }
bool ota_service_start_boot_health_check(void) { return true; }
void ota_service_note_main_loop_alive(void) {}
OtaResult ota_service_start_remote(const OtaManifest *manifest) {
    (void)manifest;
    return OTA_RESULT_INTERNAL;
}
OtaResult ota_service_start_local(const OtaManifest *manifest,
                                  OtaStreamReader reader, void *reader_context) {
    ++g_start_calls;
    if (manifest) {
        g_started_manifest = *manifest;
        snprintf(g_started_task, sizeof(g_started_task), "%s", manifest->task_id);
    }
    g_reader = reader;
    g_reader_ctx = reader_context;
    g_received_len = 0;
    while (reader && manifest && g_received_len < manifest->file_size) {
        uint8_t chunk[496];
        int n = reader(reader_context, chunk, sizeof(chunk));
        if (n <= 0) break;
        if (g_received_len + (size_t)n > sizeof(g_received)) {
            return OTA_RESULT_INTERNAL;
        }
        memcpy(g_received + g_received_len, chunk, (size_t)n);
        g_received_len += (size_t)n;
    }
    g_snapshot_valid = 1;
    g_snapshot.state = g_start_outcome_state;
    return OTA_RESULT_OK;
}
OtaResult ota_service_cancel(const char *task_id) {
    (void)task_id;
    ++g_cancel_calls;
    return OTA_RESULT_OK;
}
bool ota_service_get_snapshot(OtaSnapshot *snapshot) {
    if (!g_snapshot_valid) return false;
    *snapshot = g_snapshot;
    return true;
}
bool ota_service_is_busy(void) { return g_start_calls > 0; }
const char *ota_service_get_task_id(void) { return g_started_task; }
void ota_service_process(void) {}
bool ota_service_schedule_reboot(uint32_t delay_ms) {
    ++g_reboot_schedule_calls;
    g_reboot_schedule_delay_ms = delay_ms;
    return true;
}

const char *ota_result_code(OtaResult result) {
    switch (result) {
        case OTA_RESULT_OK: return "OK";
        case OTA_RESULT_SIGNATURE_REQUIRED: return "SIGNATURE_REQUIRED";
        case OTA_RESULT_SIGNATURE_INVALID: return "SIGNATURE_INVALID";
        case OTA_RESULT_BUSY: return "BUSY";
        case OTA_RESULT_SIZE_MISMATCH: return "SIZE_MISMATCH";
        default: return "INTERNAL";
    }
}

const char *ota_state_name(OtaState state) {
    switch (state) {
        case OTA_STATE_IDLE: return "idle";
        case OTA_STATE_ACCEPTED: return "accepted";
        case OTA_STATE_RECEIVING: return "receiving";
        case OTA_STATE_VERIFYING: return "verifying";
        case OTA_STATE_INSTALLING: return "installing";
        case OTA_STATE_SUCCEEDED: return "succeeded";
        case OTA_STATE_FAILED: return "failed";
        default: return "unknown";
    }
}

const char *ota_target_name(OtaTarget target) {
    (void)target;
    return "target";
}
const char *ota_source_name(OtaSource source) {
    (void)source;
    return "local";
}

/* ---- helpers ---- */
typedef int (*OtaAccessCb)(uint16_t, uint16_t, struct ble_gatt_access_ctxt *, void *);

static int write_attr_bytes(uint16_t handle, const void *data, size_t len) {
    OtaAccessCb cb = (OtaAccessCb)g_ota_access_cb;
    if (!cb) return -1;
    struct os_mbuf om = {data, (uint16_t)len};
    struct ble_gatt_access_ctxt ctxt = {BLE_GATT_ACCESS_OP_WRITE_CHR, &om};
    last_notify[0] = 0;
    return cb(1, handle, &ctxt, NULL);
}

static int write_attr(uint16_t handle, const char *json) {
    return write_attr_bytes(handle, json, strlen(json));
}

static int write_binary(uint32_t token, uint32_t offset,
                        const unsigned char *payload, size_t len) {
    unsigned char frame[10 + 497] = {0xB1, 0x01};
    for (unsigned i = 0; i < 4; ++i) {
        frame[2 + i] = (unsigned char)(token >> (i * 8));
        frame[6 + i] = (unsigned char)(offset >> (i * 8));
    }
    memcpy(frame + 10, payload, len);
    return write_attr_bytes(DATA_HANDLE, frame, 10 + len);
}

static void drain_one_chunk(void) {
    unsigned char item[512];
    CHECK(xQueueReceive(&g_queue, item, 0) == pdTRUE);
}

static int read_status(void) {
    OtaAccessCb cb = (OtaAccessCb)g_ota_access_cb;
    if (!cb) return -1;
    struct os_mbuf om = {0, 0};
    struct ble_gatt_access_ctxt ctxt = {BLE_GATT_ACCESS_OP_READ_CHR, &om};
    status_read_buf[0] = 0;
    return cb(1, STATUS_HANDLE, &ctxt, NULL);
}

static void b64(const unsigned char *in, size_t n, char *out, size_t cap) {
    static const char *alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = in[i] << 16;
        if (i + 1 < n) v |= in[i + 1] << 8;
        if (i + 2 < n) v |= in[i + 2];
        if (o + 4 >= cap) break;
        out[o++] = alphabet[(v >> 18) & 63];
        out[o++] = alphabet[(v >> 12) & 63];
        out[o++] = (i + 1 < n) ? alphabet[(v >> 6) & 63] : '=';
        out[o++] = (i + 2 < n) ? alphabet[v & 63] : '=';
    }
    out[o] = '\0';
}

static const char *valid_ctrl =
    "{\"v\":2,\"type\":\"ota.ctrl\",\"message_id\":\"m1\","
    "\"session_id\":\"host-session\",\"body\":{"
    "\"transfer_id\":\"tf-1\",\"task_id\":\"task-1\","
    "\"target\":\"communication_module\",\"version\":\"1.5.12\","
    "\"size\":8,"
    "\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\","
    "\"signature\":\"sig\",\"security_version\":1}}";

static char data_json[512];

static const char *make_data(const char *transfer, uint32_t offset,
                             const unsigned char *payload, size_t n) {
    char payload_b64[128];
    b64(payload, n, payload_b64, sizeof(payload_b64));
    snprintf(data_json, sizeof(data_json),
             "{\"v\":2,\"type\":\"ota.data\",\"message_id\":\"d%u\","
             "\"session_id\":\"host-session\",\"body\":{"
             "\"transfer_id\":\"%s\",\"offset\":%u,\"payload\":\"%s\"}}",
             offset, transfer, offset, payload_b64);
    return data_json;
}

static int status_has(const char *needle) {
    return strstr(last_notify, needle) != NULL ||
           strstr(status_read_buf, needle) != NULL;
}

int main(void) {
    host_freertos_reset();
    memset(&g_started_manifest, 0, sizeof(g_started_manifest));
    memset(&g_snapshot, 0, sizeof(g_snapshot));
    g_start_calls = 0;
    g_cancel_calls = 0;
    g_snapshot_valid = 0;
    notify_count = 0;
    g_queue_create_attempts = 0;
    g_queue_create_failures_remaining = 0;
    g_queue_delete_calls = 0;
    g_task_create_attempts = 0;
    g_task_create_failures_remaining = 0;
    g_reboot_schedule_calls = 0;
    g_reboot_schedule_delay_ms = 0;

    CHECK(ble_ota_init());
    CHECK(g_ota_access_cb != NULL);
    CHECK(g_queue_create_attempts == 0); /* 空闲时不常驻 OTA 队列 */

    /* 1. 鉴权门已随鉴权协议移除 (2026-09-22): 未鉴权连接可直接读 OTA 状态特征
     * (以前这里断言的是 INSUFFICIENT_AUTHEN 拒绝)。 */
    CHECK(read_status() == 0);
    CHECK(status_read_buf[0] != '\0');

    /* 2. invalid envelope versions / embedded NUL */
    CHECK(write_attr(CTRL_HANDLE, "{\"v\":1,\"type\":\"ota.ctrl\"}") ==
          BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN);
    CHECK(write_attr(CTRL_HANDLE, "") == BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN);

    /* 3. wrong session rejected */
    CHECK(write_attr(CTRL_HANDLE,
                     "{\"v\":2,\"type\":\"ota.ctrl\",\"message_id\":\"m\","
                     "\"session_id\":\"other\",\"body\":{}}") ==
          BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN);

    /* 4. malformed optional metadata / bad target still rejected
     * (2026-09-21: signature/sha256/security_version are optional, but
     * present values must still be well-formed) */
    CHECK(write_attr(CTRL_HANDLE,
                     "{\"v\":2,\"type\":\"ota.ctrl\",\"message_id\":\"m\","
                     "\"session_id\":\"host-session\",\"body\":{"
                     "\"transfer_id\":\"tf-1\",\"task_id\":\"task-1\","
                     "\"target\":\"communication_module\",\"version\":\"1.0.0\","
                     "\"size\":8,\"sha256\":\"zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz"
                     "zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz\",\"signature\":\"sig\","
                     "\"security_version\":1}}") ==
          BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN);
    CHECK(write_attr(CTRL_HANDLE,
                     "{\"v\":2,\"type\":\"ota.ctrl\",\"message_id\":\"m\","
                     "\"session_id\":\"host-session\",\"body\":{"
                     "\"transfer_id\":\"tf-1\",\"task_id\":\"task-1\","
                     "\"target\":\"communication_module\",\"version\":\"1.0.0\","
                     "\"size\":8,\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                     "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"signature\":\"sig\","
                     "\"security_version\":-1}}") ==
          BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN);
    CHECK(write_attr(CTRL_HANDLE,
                     "{\"v\":2,\"type\":\"ota.ctrl\",\"message_id\":\"m\","
                     "\"session_id\":\"host-session\",\"body\":{"
                     "\"transfer_id\":\"tf-1\",\"task_id\":\"task-1\","
                     "\"target\":\"communication_module\",\"version\":\"1.0.0\","
                     "\"size\":8,\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                     "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"signature\":\"sig\"}}") ==
          BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN);
    CHECK(write_attr(CTRL_HANDLE,
                     "{\"v\":2,\"type\":\"ota.ctrl\",\"message_id\":\"m\","
                     "\"session_id\":\"host-session\",\"body\":{"
                     "\"transfer_id\":\"tf-1\",\"task_id\":\"task-1\","
                     "\"target\":\"gpu\",\"version\":\"1.0.0\",\"size\":8,"
                     "\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                     "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"signature\":\"sig\","
                     "\"security_version\":1}}") ==
          BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN);

    /* 5. data before worker rejected */
    {
        unsigned char payload[2] = {1, 2};
        CHECK(write_attr(DATA_HANDLE, make_data("tf-1", 0, payload, 2)) ==
              BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN);
    }

    /* 6. valid ctrl: queue/task allocations retry at most three times */
    g_queue_create_failures_remaining = 2;
    g_task_create_failures_remaining = 2;
    CHECK(write_attr(CTRL_HANDLE, valid_ctrl) == 0);
    CHECK(g_queue_create_attempts == 3);
    CHECK(g_task_create_attempts == 3);
    CHECK(g_pending_task != NULL);
    CHECK(status_has("\"stage\":\"accepted\""));
    CHECK(status_has("\"transfer_id\":\"tf-1\""));
    CHECK(status_has("\"session_id\":\"host-session\""));

    /* 7. second ctrl while worker active rejected */
    CHECK(write_attr(CTRL_HANDLE, valid_ctrl) == BLE_ATT_ERR_UNLIKELY);

    /* 8. wrong transfer / non-continuous offset rejected */
    {
        unsigned char payload[4] = {9, 9, 9, 9};
        CHECK(write_attr(DATA_HANDLE, make_data("tf-other", 0, payload, 4)) ==
              BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN);
        CHECK(write_attr(DATA_HANDLE, make_data("tf-1", 4, payload, 4)) ==
              BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN);
    }

    /* 9. valid sequential chunks advance accepted_offset */
    {
        unsigned char a[3] = {1, 2, 3};
        unsigned char b[5] = {4, 5, 6, 7, 8};
        CHECK(write_attr(DATA_HANDLE, make_data("tf-1", 0, a, 3)) == 0);
        CHECK(status_has("\"accepted_offset\":3"));
        CHECK(status_has("\"stage\":\"receiving\""));
        CHECK(write_attr(DATA_HANDLE, make_data("tf-1", 3, b, 5)) == 0);
        CHECK(status_has("\"accepted_offset\":8"));
    }

    /* 10. overflow remaining size rejected */
    {
        unsigned char extra[4] = {1, 1, 1, 1};
        CHECK(write_attr(DATA_HANDLE, make_data("tf-1", 8, extra, 4)) ==
              BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN);
    }

    /* 11. run worker: ota_service receives queued bytes */
    host_freertos_run_pending_task();
    CHECK(g_start_calls == 1);

    CHECK(g_queue_delete_calls == 1); /* 终态立即释放队列 */
    CHECK(g_started_manifest.source == OTA_SOURCE_LOCAL);
    CHECK(g_started_manifest.target == OTA_TARGET_ESP);
    CHECK(g_started_manifest.file_size == 8);
    CHECK(g_started_manifest.security_version == 1);
    CHECK(strcmp(g_started_manifest.task_id, "task-1") == 0);
    CHECK(g_reader != NULL);
    CHECK(g_received_len == 8);
    CHECK(g_received[0] == 1 && g_received[2] == 3);
    CHECK(g_received[3] == 4 && g_received[7] == 8);
    CHECK(g_reboot_schedule_calls == 1);
    CHECK(g_reboot_schedule_delay_ms == 1000);

    /* 12. ota.query reports snapshot via observer/status */
    memset(&g_snapshot, 0, sizeof(g_snapshot));
    g_snapshot.source = OTA_SOURCE_LOCAL;
    g_snapshot.state = OTA_STATE_VERIFYING;
    g_snapshot.progress = 100;
    snprintf(g_snapshot.task_id, sizeof(g_snapshot.task_id), "task-1");
    snprintf(g_snapshot.error_code, sizeof(g_snapshot.error_code), "OK");
    snprintf(g_snapshot.message, sizeof(g_snapshot.message), "verifying");
    g_snapshot_valid = 1;
    CHECK(write_attr(CTRL_HANDLE,
                     "{\"v\":2,\"type\":\"ota.query\",\"message_id\":\"q1\","
                     "\"session_id\":\"host-session\",\"body\":{\"query\":\"status\"}}") ==
          0);
    CHECK(status_has("\"stage\":\"verifying\""));
    CHECK(status_has("\"progress\":100"));

    /* 13. ota.cancel */
    CHECK(write_attr(CTRL_HANDLE,
                     "{\"v\":2,\"type\":\"ota.cancel\",\"message_id\":\"c1\","
                     "\"session_id\":\"host-session\",\"body\":{\"query\":\"cancel\"}}") ==
          0);
    CHECK(g_cancel_calls == 1);
    CHECK(status_has("\"stage\":\"cancelling\""));

    /* 14. status READ returns last JSON */
    CHECK(read_status() == 0);
    CHECK(status_read_buf[0] != '\0');
    CHECK(strstr(status_read_buf, "\"type\":\"ota.status\"") != NULL);

    /* 15. queue backpressure: fill without running a new worker */
    host_freertos_reset();
    g_start_calls = 0;
    /* worker already completed so s_worker is NULL; new ctrl required */
    CHECK(write_attr(CTRL_HANDLE, valid_ctrl) == 0);
    CHECK(g_pending_task != NULL);
    {
        unsigned char payload[1] = {0xAA};
        /* The two-slot queue stays within approximately 1 KB. */
        for (int i = 0; i < QUEUE_DEPTH; ++i) {
            CHECK(write_attr(DATA_HANDLE, make_data("tf-1", (uint32_t)i, payload, 1)) == 0);
        }
        /* 队列满: 设备侧先等 200ms 再放弃, 回 9 (Prepare Queue Full, App 侧
         * 映射为"写缓冲满") —— 不再用含糊的 UNLIKELY(14) 把传输描述成
         * "无法启动"。2026-09-22, 与 001P 的 ATT 14 排查同一批改动。 */
        CHECK(write_attr(DATA_HANDLE, make_data("tf-1", QUEUE_DEPTH, payload, 1)) ==
              BLE_ATT_ERR_PREPARE_QUEUE_FULL);
    }

    /* 16. unsigned manifest accepted (2026-09-21: signature/sha256/
     * security_version are optional on the local channel) */
    host_freertos_run_pending_task(); /* drain section-15 worker: s_worker -> NULL */
    g_start_calls = 0;
    CHECK(write_attr(CTRL_HANDLE,
                     "{\"v\":2,\"type\":\"ota.ctrl\",\"message_id\":\"m2\","
                     "\"session_id\":\"host-session\",\"body\":{"
                     "\"transfer_id\":\"tf-2\",\"task_id\":\"task-2\","
                     "\"target\":\"communication_module\",\"version\":\"1.0.0\","
                     "\"size\":8,\"sha256\":\"\",\"signature\":\"\","
                     "\"security_version\":0}}") == 0);
    CHECK(g_pending_task != NULL);
    CHECK(status_has("\"stage\":\"accepted\""));
    host_freertos_run_pending_task();
    CHECK(g_start_calls == 1);

    CHECK(g_started_manifest.security_version == 0);
    CHECK(g_started_manifest.sha256[0] == '\0');
    CHECK(g_started_manifest.signature[0] == '\0');

    /* 17. 断连清理 (2026-09-22, 001P 的 ATT 14 排查): worker 在跑时 BLE 断连 →
     * 立即 cancel, worker 收尾清 s_worker; 紧接着重发清单不再被 s_worker!=NULL
     * 挡成 ATT 14 (修复前要挂到 600s 本地超时, 期间所有重试全部失败)。 */
    host_freertos_reset();
    g_start_calls = 0;
    g_cancel_calls = 0;
    CHECK(g_ota_conn_cb != NULL);
    CHECK(write_attr(CTRL_HANDLE, valid_ctrl) == 0);
    CHECK(g_pending_task != NULL);
    g_ota_conn_cb(BLE_HS_CONN_HANDLE_NONE);   /* 模拟 BLE 断连 */
    CHECK(g_cancel_calls == 1);               /* 会话被立即取消 */
    host_freertos_run_pending_task();         /* worker 退出: s_worker -> NULL */
    g_start_calls = 0;
    CHECK(write_attr(CTRL_HANDLE, valid_ctrl) == 0);  /* 立即重发清单: 不再 14 */
    CHECK(g_pending_task != NULL);
    host_freertos_run_pending_task();
    CHECK(g_start_calls == 1);

    /* A cancelled OTA also returns OK from the service, but must not reboot. */
    host_freertos_reset();
    g_start_outcome_state = OTA_STATE_CANCELLED;
    int scheduled_before_cancel = g_reboot_schedule_calls;
    CHECK(write_attr(CTRL_HANDLE, valid_ctrl) == 0);
    host_freertos_run_pending_task();
    CHECK(g_reboot_schedule_calls == scheduled_before_cancel);

    /* BMS shares the collector IAP path, but must be accepted at the BLE
     * manifest boundary before any OTA data is sent. */
    host_freertos_reset();
    g_start_outcome_state = OTA_STATE_SUCCEEDED;
    CHECK(write_attr(CTRL_HANDLE,
                     "{\"v\":2,\"type\":\"ota.ctrl\",\"message_id\":\"bms1\","
                     "\"session_id\":\"host-session\",\"body\":{"
                     "\"transfer_id\":\"tf-bms\",\"task_id\":\"task-bms\","
                     "\"target\":\"bms\",\"version\":\"1.0.0\","
                     "\"size\":8,\"sha256\":\"\",\"signature\":\"\","
                     "\"security_version\":0}}") == 0);
    CHECK(g_pending_task != NULL);
    host_freertos_run_pending_task();
    CHECK(g_started_manifest.target == OTA_TARGET_BMS);
    CHECK(g_reboot_schedule_calls == scheduled_before_cancel);

    /* Binary data has embedded NULs. ACK only at 1024 bytes and final size,
     * while a status read inside a batch must expose the new accepted offset. */
    host_freertos_reset();
    CHECK(write_attr(CTRL_HANDLE,
                     "{\"v\":2,\"type\":\"ota.ctrl\",\"session_id\":\"host-session\",\"body\":{"
                     "\"transfer_id\":\"12345678abcdef0123456789\",\"task_id\":\"task-binary\","
                     "\"target\":\"communication_module\",\"version\":\"1.0.12\","
                     "\"size\":1100,\"sha256\":\"\",\"signature\":\"\","
                     "\"security_version\":0,\"timeout_seconds\":1200}}") == 0);
    const int accepted_notify_count = notify_count;
    unsigned char bytes[496];
    for (size_t i = 0; i < sizeof(bytes); ++i) bytes[i] = (unsigned char)i;
    unsigned char short_frame[10] = {0xB1, 0x01, 0x78, 0x56, 0x34, 0x12};
    CHECK(write_attr_bytes(DATA_HANDLE, short_frame, sizeof(short_frame)) ==
          BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN);
    short_frame[1] = 0x02;
    CHECK(write_attr_bytes(DATA_HANDLE, short_frame, sizeof(short_frame)) ==
          BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN);
    CHECK(write_binary(0x87654321, 0, bytes, 32) ==
          BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN);
    CHECK(write_binary(0x12345678, 32, bytes, 32) ==
          BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN);
    unsigned char oversized[497] = {0};
    CHECK(write_binary(0x12345678, 0, oversized, sizeof(oversized)) ==
          BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN);
    CHECK(read_status() == 0);
    CHECK(strstr(status_read_buf, "\"accepted_offset\":0") != NULL);
    CHECK(write_binary(0x12345678, 0, bytes, 496) == 0);
    CHECK(write_binary(0x12345678, 0, bytes, 32) ==
          BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN);
    CHECK(notify_count == accepted_notify_count);
    CHECK(read_status() == 0);
    CHECK(strstr(status_read_buf, "\"accepted_offset\":496") != NULL);
    drain_one_chunk();
    CHECK(write_binary(0x12345678, 496, bytes, 496) == 0);
    CHECK(notify_count == accepted_notify_count);
    drain_one_chunk();
    CHECK(write_binary(0x12345678, 992, bytes, 32) == 0);
    CHECK(notify_count == accepted_notify_count + 1);
    CHECK(status_has("\"accepted_offset\":1024"));
    drain_one_chunk();
    CHECK(write_binary(0x12345678, 1024, bytes, 76) == 0);
    CHECK(notify_count == accepted_notify_count + 2);
    CHECK(status_has("\"accepted_offset\":1100"));
    CHECK(g_queue.item_size * g_queue.length <= 1024);
    host_freertos_run_pending_task();
    CHECK(g_started_manifest.timeout_seconds == 1200);

    /* A full queue is retryable. The rejected range is not accepted. */
    host_freertos_reset();
    CHECK(write_attr(CTRL_HANDLE,
                     "{\"v\":2,\"type\":\"ota.ctrl\",\"session_id\":\"host-session\",\"body\":{"
                     "\"transfer_id\":\"12345678abcdef0123456789\",\"task_id\":\"task-full\","
                     "\"target\":\"communication_module\",\"version\":\"1.0.12\","
                     "\"size\":1100,\"sha256\":\"\",\"signature\":\"\","
                     "\"security_version\":0,\"timeout_seconds\":30}}") == 0);
    CHECK(write_binary(0x12345678, 0, bytes, 496) == 0);
    CHECK(write_binary(0x12345678, 496, bytes, 496) == 0);
    CHECK(write_binary(0x12345678, 992, bytes, 32) ==
          BLE_ATT_ERR_PREPARE_QUEUE_FULL);
    CHECK(read_status() == 0);
    CHECK(strstr(status_read_buf, "\"accepted_offset\":992") != NULL);
    drain_one_chunk();
    CHECK(write_binary(0x12345678, 992, bytes, 32) == 0);
    host_freertos_run_pending_task();
    CHECK(g_started_manifest.timeout_seconds == 600);

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    puts("ble_ota host tests passed");
    return 0;
}
