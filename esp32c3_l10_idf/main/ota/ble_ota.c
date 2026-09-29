#include "ble_ota.h"

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdatomic.h>
#include <ctype.h>

#include "ble_ct.h"
#include "ble_prov.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "mbedtls/base64.h"
#include "ota_service.h"

#define BLE_OTA_JSON_MAX 512
#define BLE_OTA_CHUNK_MAX 496
#define BLE_OTA_QUEUE_DEPTH 2
#define BLE_OTA_BINARY_HEADER 10
#define BLE_OTA_ACK_BYTES 1024U
/* 4096: worker 只做队列搬运 + flash 写, 没有 TLS 深栈 (云端 worker 的 12288 是
 * mbedtls 读路径撑出来的)。本机堆最紧时最大连续块只有 ~4.6KB, 6144 经常分配
 * 失败 —— App 看到的就是 ATT 14。待串口看一次栈水位确认。 */
#define BLE_OTA_TASK_STACK 4096
/* 连续 600 次 50ms 无数据 (30s) 视为传输死亡, worker 自行退出清 s_worker */
#define BLE_OTA_IDLE_POLLS_MAX 600u
#define BLE_OTA_ALLOC_ATTEMPTS 3u
#define BLE_OTA_RETRY_DELAY_MS 50u

typedef struct {
    uint16_t len;
    uint8_t data[BLE_OTA_CHUNK_MAX];
} BleOtaChunk;

static const char *TAG = "ble_ota";
static QueueHandle_t s_queue;
static TaskHandle_t s_worker;
/* Exactly one OTA worker reads this queue; keep its 498-byte receive item off
 * the 4096-byte worker stack (the previous field log had only 640 B free). */
static BleOtaChunk s_reader_chunk;
/* 队列只在单次 OTA 会话内存在。这个无堆分配的原子锁只保护队列指针和
 * worker 句柄，避免 BLE 回调正在 xQueueSend 时 worker 把队列删掉。 */
static atomic_flag s_lifecycle_lock = ATOMIC_FLAG_INIT;
static unsigned s_idle_polls;
static OtaManifest s_manifest;
static char s_transfer_id[64];
static uint32_t s_binary_token;
static bool s_binary_token_valid;
static uint16_t s_owner_conn = BLE_HS_CONN_HANDLE_NONE;
static uint32_t s_accepted_offset;
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static char s_status[BLE_OTA_JSON_MAX] =
    "{\"v\":2,\"type\":\"ota.status\",\"body\":{\"stage\":\"idle\"}}";

static void ota_worker(void *arg);

static void lifecycle_lock(void) {
    while (atomic_flag_test_and_set(&s_lifecycle_lock))
        vTaskDelay(pdMS_TO_TICKS(1));
}

static void lifecycle_unlock(void) {
    atomic_flag_clear(&s_lifecycle_lock);
}

static QueueHandle_t create_queue_with_retry(void) {
    for (unsigned attempt = 1; attempt <= BLE_OTA_ALLOC_ATTEMPTS; ++attempt) {
        QueueHandle_t queue = xQueueCreate(BLE_OTA_QUEUE_DEPTH, sizeof(BleOtaChunk));
        if (queue) return queue;
        ESP_LOGW(TAG, "OTA queue allocation failed (%u/%u)", attempt,
                 BLE_OTA_ALLOC_ATTEMPTS);
        if (attempt < BLE_OTA_ALLOC_ATTEMPTS)
            vTaskDelay(pdMS_TO_TICKS(BLE_OTA_RETRY_DELAY_MS));
    }
    return NULL;
}

static bool create_worker_with_retry(void) {
    for (unsigned attempt = 1; attempt <= BLE_OTA_ALLOC_ATTEMPTS; ++attempt) {
        if (xTaskCreate(ota_worker, "ble_ota", BLE_OTA_TASK_STACK, NULL, 5,
                        &s_worker) == pdPASS) return true;
        s_worker = NULL;
        ESP_LOGW(TAG, "OTA worker allocation failed (%u/%u)", attempt,
                 BLE_OTA_ALLOC_ATTEMPTS);
        if (attempt < BLE_OTA_ALLOC_ATTEMPTS)
            vTaskDelay(pdMS_TO_TICKS(BLE_OTA_RETRY_DELAY_MS));
    }
    return false;
}

static void notify_status(void) {
    if (s_conn == BLE_HS_CONN_HANDLE_NONE) return;
    const size_t len = strlen(s_status);
    struct os_mbuf *om = ble_hs_mbuf_from_flat(s_status, len);
    if (!om) {
        ESP_LOGW(TAG, "OTA status notify allocation failed: len=%u", (unsigned)len);
        return;
    }
    const int rc = ble_gattc_notify_custom(s_conn, ble_prov_get_ota_status_handle(), om);
    if (rc != 0)
        ESP_LOGW(TAG, "OTA status notify failed: rc=%d len=%u", rc, (unsigned)len);
}

static void update_status(const char *stage, const char *code, uint8_t progress,
                          uint32_t accepted_offset, const char *message,
                          bool send_notification) {
    char safe_message[96];
    size_t safe_len = 0;
    for (const char *p = message ? message : "";
         *p && safe_len + 1 < sizeof(safe_message); ++p) {
        const unsigned char ch = (unsigned char)*p;
        safe_message[safe_len++] = (ch < 0x20 || ch == '"' || ch == '\\') ? ' ' : (char)ch;
    }
    safe_message[safe_len] = '\0';
    snprintf(s_status, sizeof(s_status),
        "{\"v\":2,\"type\":\"ota.status\",\"message_id\":\"ota-status\","
        "\"session_id\":\"%s\",\"body\":{\"transfer_id\":\"%s\","
        "\"task_id\":\"%s\",\"stage\":\"%s\",\"accepted_offset\":%lu,"
        "\"progress\":%u,\"result_code\":\"%s\",\"message\":\"%s\"}}",
        ble_ct_get_session_id(), s_transfer_id, s_manifest.task_id, stage,
        (unsigned long)accepted_offset, progress, code ? code : "OK",
        safe_message);
    if (send_notification) notify_status();
}

static void set_status(const char *stage, const char *code, uint8_t progress,
                       uint32_t accepted_offset, const char *message) {
    update_status(stage, code, progress, accepted_offset, message, true);
}

static void ota_observer(const OtaSnapshot *snapshot) {
    if (!snapshot || snapshot->source != OTA_SOURCE_LOCAL) return;
    set_status(ota_state_name(snapshot->state), snapshot->error_code,
               snapshot->progress, s_accepted_offset, snapshot->message);
}

static int queue_reader(void *context, uint8_t *buffer, size_t capacity) {
    (void)context;
    if (!s_queue) return -1;
    if (s_conn == BLE_HS_CONN_HANDLE_NONE) return -1;   /* BLE 已断连: 立即收尾 */
    if (xQueueReceive(s_queue, &s_reader_chunk, pdMS_TO_TICKS(50)) != pdTRUE) {
        /* 持续无数据 (600×50ms = 30s): 放弃, worker 自行退出并清 s_worker。
         * 本地会话超时是 600s, 不加这条的话一次中断会把设备锁死十分钟,
         * 期间 App 重发清单全被 s_worker!=NULL 挡成 ATT 14。 */
        if (++s_idle_polls > BLE_OTA_IDLE_POLLS_MAX) return -1;
        return 0;
    }
    s_idle_polls = 0;
    if (s_reader_chunk.len > capacity) return -1;
    memcpy(buffer, s_reader_chunk.data, s_reader_chunk.len);
    return s_reader_chunk.len;
}

static void ota_worker(void *arg) {
    (void)arg;
    OtaResult result = ota_service_start_local(&s_manifest, queue_reader, NULL);
    ESP_LOGI(TAG, "OTA worker finished: result=%s min_free_stack=%u bytes",
             ota_result_code(result), (unsigned)uxTaskGetStackHighWaterMark(NULL));
    if (result != OTA_RESULT_OK)
        set_status("failed", ota_result_code(result), 0, s_accepted_offset,
                   ota_result_code(result));
    OtaSnapshot snapshot = {0};
    const bool needs_reboot = result == OTA_RESULT_OK &&
                              s_manifest.target == OTA_TARGET_ESP &&
                              ota_service_get_snapshot(&snapshot) &&
                              snapshot.state == OTA_STATE_REBOOTING;
    const bool reboot_scheduled = !needs_reboot ||
                                  ota_service_schedule_reboot(1000);
    lifecycle_lock();
    QueueHandle_t queue = s_queue;
    s_queue = NULL;
    if (reboot_scheduled) s_worker = NULL;
    lifecycle_unlock();
    if (queue) vQueueDelete(queue);
    if (!reboot_scheduled) {
        ESP_LOGW(TAG, "reboot task allocation failed; rebooting from BLE worker");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
        lifecycle_lock();
        s_worker = NULL;  /* esp_restart should not return */
        lifecycle_unlock();
    }
    vTaskDelete(NULL);
}

static bool json_token(const cJSON *item, size_t max) {
    if (!cJSON_IsString(item)) return false;
    size_t n = strlen(item->valuestring);
    return n > 0 && n <= max &&
        strspn(item->valuestring,
               "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._:-") == n;
}

static bool binary_token(const char *transfer_id, uint32_t *token) {
    if (strlen(transfer_id) != 24) return false;
    uint32_t value = 0;
    for (unsigned i = 0; i < 24; ++i) {
        unsigned char c = (unsigned char)transfer_id[i];
        if (!isxdigit(c)) return false;
        if (i >= 8) continue;
        unsigned digit = c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10;
        value = (value << 4) | digit;
    }
    *token = value;
    return true;
}

static uint32_t read_le32(const uint8_t *bytes) {
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static int handle_ctrl(cJSON *root) {
    cJSON *type = cJSON_GetObjectItem(root, "type");
    cJSON *sid = cJSON_GetObjectItem(root, "session_id");
    cJSON *body = cJSON_GetObjectItem(root, "body");
    if (!cJSON_IsString(type) || !cJSON_IsString(sid) ||
        strcmp(sid->valuestring, ble_ct_get_session_id()) != 0 ||
        !cJSON_IsObject(body)) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    if (strcmp(type->valuestring, "ota.query") == 0) {
        OtaSnapshot snapshot;
        if (ota_service_get_snapshot(&snapshot)) ota_observer(&snapshot);
        else notify_status();
        return 0;
    }
    if (strcmp(type->valuestring, "ota.cancel") == 0) {
        OtaResult result = ota_service_cancel(s_manifest.task_id);
        set_status(result == OTA_RESULT_OK ? "cancelling" : "failed",
                   ota_result_code(result), 0, s_accepted_offset,
                   ota_result_code(result));
        return 0;
    }
    if (strcmp(type->valuestring, "ota.ctrl") != 0)
        return BLE_ATT_ERR_UNLIKELY;

    cJSON *transfer = cJSON_GetObjectItem(body, "transfer_id");
    cJSON *task = cJSON_GetObjectItem(body, "task_id");
    cJSON *target = cJSON_GetObjectItem(body, "target");
    cJSON *version = cJSON_GetObjectItem(body, "version");
    cJSON *size = cJSON_GetObjectItem(body, "size");
    cJSON *sha = cJSON_GetObjectItem(body, "sha256");
    cJSON *signature = cJSON_GetObjectItem(body, "signature");
    cJSON *security = cJSON_GetObjectItem(body, "security_version");
    cJSON *timeout = cJSON_GetObjectItem(body, "timeout_seconds");
    /* 2026-09-21 sha256/signature/security_version 改为可选（服务端固件
     * 记录可能未签名）；字段存在时仍按原格式校验，空值由 ota_service
     * 跳过验签与回滚检查。 */
    if (!json_token(transfer, 63) || !json_token(task, 63) ||
        !cJSON_IsString(target) || !json_token(version, 31) ||
        !cJSON_IsNumber(size) || !isfinite(size->valuedouble) ||
        size->valuedouble <= 0 || size->valuedouble > UINT32_MAX ||
        floor(size->valuedouble) != size->valuedouble ||
        !cJSON_IsString(sha) ||
        (sha->valuestring[0] &&
         (strlen(sha->valuestring) != 64 ||
          strspn(sha->valuestring, "0123456789abcdef") != 64)) ||
        !cJSON_IsString(signature) ||
        !cJSON_IsNumber(security) || !isfinite(security->valuedouble) ||
        security->valuedouble < 0 || security->valuedouble > UINT32_MAX ||
        floor(security->valuedouble) != security->valuedouble ||
        (timeout && (!cJSON_IsNumber(timeout) || !isfinite(timeout->valuedouble) ||
                     timeout->valuedouble < 0 || timeout->valuedouble > 3600 ||
                     floor(timeout->valuedouble) != timeout->valuedouble)))
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    OtaTarget ota_target;
    if (strcmp(target->valuestring, "communication_module") == 0)
        ota_target = OTA_TARGET_ESP;
    else if (strcmp(target->valuestring, "system_controller") == 0)
        ota_target = OTA_TARGET_ARM;
    else if (strcmp(target->valuestring, "dsp") == 0 ||
             strcmp(target->valuestring, "dsp_controller") == 0)
        ota_target = OTA_TARGET_DSP;
    else if (strcmp(target->valuestring, "bms") == 0)
        ota_target = OTA_TARGET_BMS;
    else return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;

    lifecycle_lock();
    if (s_worker || s_queue) {
        lifecycle_unlock();
        return BLE_ATT_ERR_UNLIKELY;
    }
    memset(&s_manifest, 0, sizeof(s_manifest));
    s_manifest.source = OTA_SOURCE_LOCAL;
    s_manifest.target = ota_target;
    snprintf(s_transfer_id, sizeof(s_transfer_id), "%s", transfer->valuestring);
    s_binary_token_valid = binary_token(s_transfer_id, &s_binary_token);
    s_owner_conn = s_conn;
    snprintf(s_manifest.task_id, sizeof(s_manifest.task_id), "%s", task->valuestring);
    snprintf(s_manifest.version, sizeof(s_manifest.version), "%s", version->valuestring);
    snprintf(s_manifest.sha256, sizeof(s_manifest.sha256), "%s", sha->valuestring);
    snprintf(s_manifest.signature, sizeof(s_manifest.signature), "%s", signature->valuestring);
    s_manifest.file_size = (uint32_t)size->valuedouble;
    s_manifest.security_version = (uint32_t)security->valuedouble;
    uint32_t requested_timeout = timeout ? (uint32_t)timeout->valuedouble : 600U;
    s_manifest.timeout_seconds = requested_timeout < 600U ? 600U : requested_timeout;
    s_accepted_offset = 0;
    s_idle_polls = 0;
    s_queue = create_queue_with_retry();
    if (!s_queue) {
        lifecycle_unlock();
        return BLE_ATT_ERR_UNLIKELY;
    }
    if (!create_worker_with_retry()) {
        QueueHandle_t queue = s_queue;
        s_queue = NULL;
        lifecycle_unlock();
        vQueueDelete(queue);
        return BLE_ATT_ERR_UNLIKELY;
    }
    lifecycle_unlock();
    set_status("accepted", "OK", 0, 0, "Manifest accepted");
    return 0;
}

static int handle_data(cJSON *root) {
    cJSON *body = cJSON_GetObjectItem(root, "body");
    cJSON *sid = cJSON_GetObjectItem(root, "session_id");
    cJSON *transfer = body ? cJSON_GetObjectItem(body, "transfer_id") : NULL;
    cJSON *offset = body ? cJSON_GetObjectItem(body, "offset") : NULL;
    cJSON *payload = body ? cJSON_GetObjectItem(body, "payload") : NULL;
    if (!cJSON_IsString(sid) ||
        strcmp(sid->valuestring, ble_ct_get_session_id()) != 0 ||
        !cJSON_IsString(transfer) || strcmp(transfer->valuestring, s_transfer_id) != 0 ||
        !cJSON_IsNumber(offset) || !isfinite(offset->valuedouble) ||
        offset->valuedouble < 0 || offset->valuedouble > UINT32_MAX ||
        floor(offset->valuedouble) != offset->valuedouble ||
        (uint32_t)offset->valuedouble != s_accepted_offset ||
        !cJSON_IsString(payload)) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    BleOtaChunk chunk = {0}; size_t decoded = 0;
    if (mbedtls_base64_decode(chunk.data, sizeof(chunk.data), &decoded,
            (const unsigned char *)payload->valuestring,
            strlen(payload->valuestring)) != 0 || decoded == 0)
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    chunk.len = (uint16_t)decoded;
    lifecycle_lock();
    if (!s_worker || !s_queue ||
        strcmp(transfer->valuestring, s_transfer_id) != 0 ||
        (uint32_t)offset->valuedouble != s_accepted_offset ||
        chunk.len > s_manifest.file_size - s_accepted_offset) {
        lifecycle_unlock();
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    /* flash 写偶尔慢于 BLE 到达: 先让 200ms, 别直接把整次传输打死;
     * 仍满才回 9 (App 侧映射为"写缓冲满", 区别于含糊的 14)。 */
    if (xQueueSend(s_queue, &chunk, pdMS_TO_TICKS(200)) != pdTRUE) {
        lifecycle_unlock();
        return BLE_ATT_ERR_PREPARE_QUEUE_FULL;
    }
    s_accepted_offset += chunk.len;
    const uint32_t accepted_offset = s_accepted_offset;
    const uint32_t file_size = s_manifest.file_size;
    lifecycle_unlock();
    set_status("receiving", "OK",
        (uint8_t)(((uint64_t)accepted_offset * 100U) / file_size),
        accepted_offset, "Chunk accepted");
    return 0;
}

static int handle_binary_data(uint16_t conn, const uint8_t *frame, size_t len) {
    if (len <= BLE_OTA_BINARY_HEADER ||
        len > BLE_OTA_BINARY_HEADER + BLE_OTA_CHUNK_MAX ||
        frame[0] != 0xB1 || frame[1] != 0x01)
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    const uint32_t token = read_le32(frame + 2);
    const uint32_t offset = read_le32(frame + 6);
    BleOtaChunk chunk = {.len = (uint16_t)(len - BLE_OTA_BINARY_HEADER)};
    memcpy(chunk.data, frame + BLE_OTA_BINARY_HEADER, chunk.len);
    lifecycle_lock();
    if (!s_worker || !s_queue || !s_binary_token_valid ||
        conn != s_owner_conn || token != s_binary_token ||
        offset != s_accepted_offset ||
        s_accepted_offset > s_manifest.file_size ||
        chunk.len > s_manifest.file_size - s_accepted_offset) {
        lifecycle_unlock();
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    if (xQueueSend(s_queue, &chunk, pdMS_TO_TICKS(200)) != pdTRUE) {
        lifecycle_unlock();
        return BLE_ATT_ERR_PREPARE_QUEUE_FULL;
    }
    const uint32_t previous = s_accepted_offset;
    s_accepted_offset += chunk.len;
    const uint32_t accepted = s_accepted_offset;
    const uint32_t size = s_manifest.file_size;
    lifecycle_unlock();
    const bool notify = accepted / BLE_OTA_ACK_BYTES > previous / BLE_OTA_ACK_BYTES ||
                        accepted == size;
    update_status("receiving", "OK", (uint8_t)(((uint64_t)accepted * 100U) / size),
                  accepted, "Chunk accepted", notify);
    if (notify) ESP_LOGI(TAG, "BLE OTA accepted %lu/%lu", (unsigned long)accepted,
                         (unsigned long)size);
    return 0;
}

static int access_cb(uint16_t conn, uint16_t attr,
                     struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)arg;
    /* 2026-09-22 产品决策：本地 OTA 不再要求绑定/鉴权（配网、本地升级
     * 免 PIN）。旧版此处的 ble_ct_auth_is_authenticated_for 门槛已移除；
     * 鉴权协议本身保留，供旧版 App 兼容使用。 */
    s_conn = conn;
    if (attr == ble_prov_get_ota_status_handle() &&
        ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        return os_mbuf_append(ctxt->om, s_status, strlen(s_status));
    }
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;
    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    char value[BLE_OTA_JSON_MAX];
    if (len == 0 || len >= sizeof(value) ||
        ble_hs_mbuf_to_flat(ctxt->om, value, len, NULL) != 0)
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    if (attr == ble_prov_get_ota_data_handle() && (uint8_t)value[0] == 0xB1)
        return handle_binary_data(conn, (const uint8_t *)value, len);
    if (memchr(value, '\0', len)) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    value[len] = '\0';
    cJSON *root = cJSON_ParseWithLengthOpts(value, len + 1, NULL, true);
    if (!root || !cJSON_IsNumber(cJSON_GetObjectItem(root, "v")) ||
        cJSON_GetObjectItem(root, "v")->valueint != 2) {
        cJSON_Delete(root); return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    int rc = attr == ble_prov_get_ota_ctrl_handle()
        ? handle_ctrl(root)
        : attr == ble_prov_get_ota_data_handle()
            ? handle_data(root) : BLE_ATT_ERR_UNLIKELY;
    cJSON_Delete(root);
    return rc;
}

/* BLE 断连: 立即取消本地会话, OTA worker 在一次队列轮询 (~50ms) 内自行退出
 * 并清 s_worker。不清的话 App 断连重连后重发清单会撞 s_worker!=NULL 的
 * ATT 14 —— 本地会话超时 600s, 等它自己超时等于把设备锁死十分钟。 */
static void ble_ota_on_conn(uint16_t conn) {
    s_conn = conn;
    if (conn != BLE_HS_CONN_HANDLE_NONE || !s_worker) return;
    ota_service_cancel(s_manifest.task_id);
    set_status("failed", "CANCELLED", 0, s_accepted_offset, "BLE disconnected");
}

bool ble_ota_init(void) {
    ota_service_set_observer(ota_observer);
    ble_prov_set_ota_access_cb((void *)access_cb);
    ble_prov_set_conn_callback(ble_ota_on_conn);
    ESP_LOGI(TAG, "BLE OTA adapter ready");
    return true;
}
