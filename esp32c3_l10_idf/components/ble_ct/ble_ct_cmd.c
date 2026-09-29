#include "ble_ct_cmd.h"
#include "ble_ct.h"
#include "ble_ct_clock.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "host/ble_hs.h"
#include "host/ble_gatt.h"
#include "cJSON.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

static const char *TAG = "ble_ct_cmd";
#define TOKEN_MAX_LEN 64
#define RESULT_CAPACITY 512

static bool valid_token(const cJSON *item) {
    if (!cJSON_IsString(item)) return false;
    size_t len = strlen(item->valuestring);
    return len > 0 && len <= TOKEN_MAX_LEN &&
        strspn(item->valuestring, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._:-") == len;
}

/* 幂等缓存 */
typedef struct {
    char command_id[TOKEN_MAX_LEN + 1];
    char canonical_request[384];
    char result[RESULT_CAPACITY];
    uint32_t ts_ms;
    bool valid;
} CmdCache;

#define CACHE_SIZE 8
#define CACHE_EXPIRE_MS (10 * 60 * 1000)
static CmdCache s_cache[CACHE_SIZE];
static int s_cache_idx = 0;

typedef struct {
    bool active;
    bool acked;
    char operation_id[TOKEN_MAX_LEN + 1];
    char command_id[TOKEN_MAX_LEN + 1];
    char message_id[TOKEN_MAX_LEN + 1];
    char session_id[TOKEN_MAX_LEN + 1];
    char param_id[TOKEN_MAX_LEN + 1];
} PendingWrite;

static PendingWrite s_pending_write;

static bool prepare_pending_write(const char *operation_id, const char *command_id,
                                  const char *message_id, const char *param_id) {
    if (s_pending_write.active) return false;
    memset(&s_pending_write, 0, sizeof(s_pending_write));
    s_pending_write.active = true;
    snprintf(s_pending_write.operation_id, sizeof(s_pending_write.operation_id), "%s", operation_id);
    snprintf(s_pending_write.command_id, sizeof(s_pending_write.command_id), "%s", command_id);
    snprintf(s_pending_write.message_id, sizeof(s_pending_write.message_id), "%s", message_id);
    snprintf(s_pending_write.session_id, sizeof(s_pending_write.session_id), "%s", ble_ct_get_session_id());
    snprintf(s_pending_write.param_id, sizeof(s_pending_write.param_id), "%s", param_id);
    return true;
}

/* 命令执行回调（主层注入，避免对 main 的编译依赖） */
static ble_cmd_write_fn s_write_fn = NULL;
static ble_cmd_read_fn  s_read_fn = NULL;

void ble_ct_cmd_set_executors(ble_cmd_write_fn write_fn, ble_cmd_read_fn read_fn) {
    s_write_fn = write_fn;
    s_read_fn = read_fn;
}

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static const char *cache_lookup(const char *id, const char *canonical_request,
                                bool *conflict) {
    *conflict = false;
    for (int i = 0; i < CACHE_SIZE; i++) {
        if (s_cache[i].valid && strcmp(s_cache[i].command_id, id) == 0) {
            if (now_ms() - s_cache[i].ts_ms > CACHE_EXPIRE_MS) { s_cache[i].valid = false; return NULL; }
            if (strcmp(s_cache[i].canonical_request, canonical_request) != 0) {
                *conflict = true;
                return NULL;
            }
            return s_cache[i].result;
        }
    }
    return NULL;
}

static void cache_add(const char *id, const char *canonical_request, const char *result) {
    CmdCache *e = &s_cache[s_cache_idx];
    strncpy(e->command_id, id, sizeof(e->command_id) - 1);
    strncpy(e->result, result, sizeof(e->result) - 1);
    strncpy(e->canonical_request, canonical_request, sizeof(e->canonical_request) - 1);
    e->command_id[sizeof(e->command_id) - 1] = '\0';
    e->result[sizeof(e->result) - 1] = '\0';
    e->canonical_request[sizeof(e->canonical_request) - 1] = '\0';
    e->ts_ms = now_ms();
    e->valid = true;
    s_cache_idx = (s_cache_idx + 1) % CACHE_SIZE;
}

static void cache_replace_result(const char *id, const char *result) {
    for (int i = 0; i < CACHE_SIZE; ++i) {
        if (s_cache[i].valid && strcmp(s_cache[i].command_id, id) == 0) {
            strncpy(s_cache[i].result, result, sizeof(s_cache[i].result) - 1);
            s_cache[i].result[sizeof(s_cache[i].result) - 1] = '\0';
            return;
        }
    }
}

static void finish_pending_write(const char *status, const char *error_code,
                                 double actual, uint32_t revision) {
    if (!s_pending_write.active) return;
    char response[RESULT_CAPACITY];
    int n;
    if (error_code) {
        n = snprintf(response, sizeof(response),
            "{\"v\":2,\"type\":\"control.result\",\"message_id\":\"final-%s\","
            "\"in_reply_to\":\"%s\",\"session_id\":\"%s\",\"body\":{"
            "\"operation_id\":\"%s\",\"command_id\":\"%s\",\"status\":\"%s\","
            "\"result_id\":\"final-%s\",\"error\":{\"code\":\"%s\","
            "\"message\":\"ARM write confirmation failed\"},\"actual\":{},"
            "\"parameter_revision\":%lu}}",
            s_pending_write.command_id, s_pending_write.message_id,
            s_pending_write.session_id, s_pending_write.operation_id,
            s_pending_write.command_id, status, s_pending_write.command_id,
            error_code, (unsigned long)revision);
    } else {
        n = snprintf(response, sizeof(response),
            "{\"v\":2,\"type\":\"control.result\",\"message_id\":\"final-%s\","
            "\"in_reply_to\":\"%s\",\"session_id\":\"%s\",\"body\":{"
            "\"operation_id\":\"%s\",\"command_id\":\"%s\",\"status\":\"applied\","
            "\"result_id\":\"final-%s\",\"error\":null,\"actual\":{"
            "\"param_id\":\"%s\",\"value\":%g},\"parameter_revision\":%lu}}",
            s_pending_write.command_id, s_pending_write.message_id,
            s_pending_write.session_id, s_pending_write.operation_id,
            s_pending_write.command_id, s_pending_write.command_id,
            s_pending_write.param_id, actual, (unsigned long)revision);
    }
    if (n > 0 && n < (int)sizeof(response)) {
        cache_replace_result(s_pending_write.command_id, response);
        if (strcmp(s_pending_write.session_id, ble_ct_get_session_id()) == 0) {
            ble_ct_cmd_notify_result(response, (uint16_t)n);
        }
    }
    memset(&s_pending_write, 0, sizeof(s_pending_write));
}

void ble_ct_cmd_on_write_ack(const char *param_id, int err) {
    if (!s_pending_write.active || !param_id ||
        strcmp(s_pending_write.param_id, param_id) != 0) return;
    if (err != 0) {
        const char *code = err == -1 ? "ARM_TIMEOUT"
            : err == -2 ? "READBACK_MISMATCH" : "ARM_REJECTED";
        finish_pending_write("failed", code, 0, 0);
        return;
    }
    s_pending_write.acked = true;
}

void ble_ct_cmd_on_write_readback(const char *param_id, double actual,
                                  uint32_t parameter_revision) {
    if (!s_pending_write.active || !s_pending_write.acked || !param_id ||
        strcmp(s_pending_write.param_id, param_id) != 0) return;
    finish_pending_write("applied", NULL, actual, parameter_revision);
}

int ble_ct_cmd_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle;
    /* 2026-09-22 起控制不再要求鉴权: App 侧的 checkPin 已无调用点, 原门槛
     * (BLE_ATT_ERR_INSUFFICIENT_AUTHEN) 只会把 App 的控制请求全部拒掉。 */
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;

    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    char buf[512] = {0};
    if (len == 0 || len >= sizeof(buf)) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, len, NULL) != 0 ||
        memchr(buf, '\0', len) != NULL) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    buf[len] = '\0';
    /* cJSON does not retain decoded string lengths; reject escaped NUL before parsing. */
    if (strstr(buf, "\\u0000") != NULL) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;

    cJSON *root = cJSON_ParseWithLengthOpts(buf, (size_t)len + 1, NULL, true);
    if (!root) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;

    bool is_v2 = false;
    const char *message_id = NULL;
    const char *operation_id = NULL;
    cJSON *command = root;
    cJSON *j_version = cJSON_GetObjectItem(root, "v");
    if (cJSON_IsNumber(j_version) && j_version->valueint == 2) {
        cJSON *j_type = cJSON_GetObjectItem(root, "type");
        cJSON *j_message = cJSON_GetObjectItem(root, "message_id");
        cJSON *j_session = cJSON_GetObjectItem(root, "session_id");
        cJSON *j_body = cJSON_GetObjectItem(root, "body");
        if (!valid_token(j_type) || strcmp(j_type->valuestring, "control.request") != 0 ||
            !valid_token(j_message) || !valid_token(j_session) ||
            strcmp(j_session->valuestring, ble_ct_get_session_id()) != 0 ||
            !cJSON_IsObject(j_body)) {
            cJSON_Delete(root);
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        cJSON *j_body_session = cJSON_GetObjectItem(j_body, "session_id");
        cJSON *j_operation = cJSON_GetObjectItem(j_body, "operation_id");
        cJSON *j_sequence = cJSON_GetObjectItem(j_body, "sequence");
        if (!valid_token(j_body_session) ||
            strcmp(j_body_session->valuestring, ble_ct_get_session_id()) != 0 ||
            !valid_token(j_operation) || !cJSON_IsNumber(j_sequence) ||
            !isfinite(j_sequence->valuedouble) || j_sequence->valuedouble < 1 ||
            floor(j_sequence->valuedouble) != j_sequence->valuedouble) {
            cJSON_Delete(root);
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        is_v2 = true;
        message_id = j_message->valuestring;
        operation_id = j_operation->valuestring;
        command = j_body;
    }

    cJSON *j_id = cJSON_GetObjectItem(command, "command_id");
    cJSON *j_action = cJSON_GetObjectItem(command, "action");
    cJSON *j_params = cJSON_GetObjectItem(command, "params");
    cJSON *j_ts = cJSON_GetObjectItem(command, "ts");

    cJSON *j_pid = j_params ? cJSON_GetObjectItem(j_params, "param_id") : NULL;
    if (!valid_token(j_id) || !valid_token(j_action) ||
        (j_pid && !valid_token(j_pid)) ||
        (j_ts && (!cJSON_IsNumber(j_ts) || !isfinite(j_ts->valuedouble) ||
                  j_ts->valuedouble < 0 || j_ts->valuedouble > UINT32_MAX ||
                  floor(j_ts->valuedouble) != j_ts->valuedouble))) {
        cJSON_Delete(root);
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    const char *command_id = j_id->valuestring;
    const char *action = j_action->valuestring;

    /* 幂等检查：同一 command_id 仅允许完全相同的 canonical body。 */
    char *canonical_request = cJSON_PrintUnformatted(command);
    if (!canonical_request || strlen(canonical_request) >= sizeof(s_cache[0].canonical_request)) {
        cJSON_free(canonical_request);
        cJSON_Delete(root);
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    bool idempotency_conflict = false;
    const char *cached = cache_lookup(command_id, canonical_request, &idempotency_conflict);
    if (cached) {
        ble_ct_cmd_notify_result(cached, strlen(cached));
        cJSON_free(canonical_request);
        cJSON_Delete(root);
        return 0;
    }
    if (idempotency_conflict) {
        char conflict[RESULT_CAPACITY];
        int conflict_len;
        if (is_v2) {
            conflict_len = snprintf(conflict, sizeof(conflict),
                "{\"v\":2,\"type\":\"control.result\",\"message_id\":\"conflict-%s\","
                "\"in_reply_to\":\"%s\",\"session_id\":\"%s\",\"body\":{"
                "\"operation_id\":\"%s\",\"command_id\":\"%s\",\"status\":\"rejected\","
                "\"result_id\":\"conflict-%s\",\"error\":{\"code\":\"IDEMPOTENCY_CONFLICT\","
                "\"message\":\"command_id reused with different payload\"},"
                "\"actual\":{},\"parameter_revision\":0}}",
                command_id, message_id, ble_ct_get_session_id(), operation_id,
                command_id, command_id);
        } else {
            conflict_len = snprintf(conflict, sizeof(conflict),
                "{\"command_id\":\"%s\",\"status\":\"error\",\"error\":{"
                "\"code\":\"IDEMPOTENCY_CONFLICT\",\"message\":\"payload differs\"}}",
                command_id);
        }
        if (conflict_len > 0 && conflict_len < (int)sizeof(conflict))
            ble_ct_cmd_notify_result(conflict, (uint16_t)conflict_len);
        cJSON_free(canonical_request);
        cJSON_Delete(root);
        return 0;
    }

    /* 时间戳校验 ±300s（协议 §7.1）：防重放；设备未校时（ble_ct_now()==0）跳过 */
    if (cJSON_IsNumber(j_ts)) {
        uint32_t now = ble_ct_now();
        if (now > 0) {
            uint32_t ts = (uint32_t)j_ts->valuedouble;
            int32_t diff = (int32_t)(now - ts);
            if (diff < -300 || diff > 300) {
                ESP_LOGW(TAG, "cmd %s: ts out of window (now=%u, ts=%u)", command_id, now, ts);
                char err[256];
                snprintf(err, sizeof(err),
                    "{\"command_id\":\"%s\",\"status\":\"error\","
                    "\"error\":{\"code\":\"INTERNAL\",\"message\":\"timestamp out of range\"}}",
                    command_id);
                ble_ct_cmd_notify_result(err, strlen(err));
                cJSON_free(canonical_request);
                cJSON_Delete(root);
                return 0;
            }
        }
    }
    /* ---- action 映射执行（协议 §7.3）；经主层注入的执行器落地 ---- */
    char resp[RESULT_CAPACITY];
    int n = 0;

    if (strcmp(action, "get_param") == 0) {
        /* 读控制参数：值经 data 返回 */
        cJSON *j_pid = (j_params) ? cJSON_GetObjectItem(j_params, "param_id") : NULL;
        if (!s_read_fn || !cJSON_IsString(j_pid) || !j_pid->valuestring[0]) {
            n = snprintf(resp, sizeof(resp),
                "{\"command_id\":\"%s\",\"status\":\"error\","
                "\"error\":{\"code\":\"INVALID_ARGS\",\"message\":\"missing param_id\"}}",
                command_id);
        } else {
            double val = 0;
            if (s_read_fn(j_pid->valuestring, &val) == BLE_CMD_OK && isfinite(val)) {
                n = snprintf(resp, sizeof(resp),
                    "{\"command_id\":\"%s\",\"status\":\"ok\","
                    "\"data\":{\"param_id\":\"%s\",\"value\":%g}}",
                    command_id, j_pid->valuestring, val);
            } else {
                n = snprintf(resp, sizeof(resp),
                    "{\"command_id\":\"%s\",\"status\":\"error\","
                    "\"error\":{\"code\":\"UNKNOWN_ACTION\",\"message\":\"unknown or unreadable param_id\"}}",
                    command_id);
            }
        }
    } else if (strcmp(action, "power_on") == 0 ||
               strcmp(action, "power_off") == 0) {
        /* 语义化别名：output_priority 0=开机, 1=关机（设计文档 §1.3） */
        double v = (strcmp(action, "power_on") == 0) ? 0.0 : 1.0;
        bool prepared = !is_v2 || prepare_pending_write(
            operation_id, command_id, message_id, "output_priority");
        int rc = !prepared ? BLE_CMD_BUSY
            : (s_write_fn ? s_write_fn("output_priority", v) : BLE_CMD_UNKNOWN);
        if (is_v2 && rc != BLE_CMD_OK && prepared)
            memset(&s_pending_write, 0, sizeof(s_pending_write));
        n = snprintf(resp, sizeof(resp),
            "{\"command_id\":\"%s\",\"status\":\"%s\","
            "\"data\":{\"applied_priority\":%d}}",
            command_id, (rc == BLE_CMD_OK) ? "ok" : "error",
            (int)v);
    } else if (strcmp(action, "set_param") == 0) {
        /* 写控制参数：param_id 即 cmd_handler 命令名, value 即工程单位值 */
        cJSON *j_pid = (j_params) ? cJSON_GetObjectItem(j_params, "param_id") : NULL;
        cJSON *j_val = (j_params) ? cJSON_GetObjectItem(j_params, "value") : NULL;
        if (!cJSON_IsString(j_pid) || !j_pid->valuestring[0] ||
            !cJSON_IsNumber(j_val) || !isfinite(j_val->valuedouble) || !s_write_fn) {
            n = snprintf(resp, sizeof(resp),
                "{\"command_id\":\"%s\",\"status\":\"error\","
                "\"error\":{\"code\":\"INVALID_ARGS\",\"message\":\"need param_id and value\"}}",
                command_id);
        } else {
            bool prepared = !is_v2 || prepare_pending_write(
                operation_id, command_id, message_id, j_pid->valuestring);
            int rc = !prepared ? BLE_CMD_BUSY
                : s_write_fn(j_pid->valuestring, j_val->valuedouble);
            if (is_v2 && rc != BLE_CMD_OK && prepared)
                memset(&s_pending_write, 0, sizeof(s_pending_write));
            if (rc == BLE_CMD_OK) {
                n = snprintf(resp, sizeof(resp),
                    "{\"command_id\":\"%s\",\"status\":\"ok\","
                    "\"data\":{\"applied\":true}}",
                    command_id);
            } else {
                const char *err = (rc == BLE_CMD_OUT_OF_RANGE) ? "OUT_OF_RANGE"
                                 : (rc == BLE_CMD_BUSY) ? "BUSY"
                                 : "UNKNOWN_ACTION";
                n = snprintf(resp, sizeof(resp),
                    "{\"command_id\":\"%s\",\"status\":\"error\","
                    "\"error\":{\"code\":\"%s\",\"message\":\"%s\"}}",
                    command_id, err, err);
            }
        }
    } else {
        n = snprintf(resp, sizeof(resp),
            "{\"command_id\":\"%s\",\"status\":\"error\","
            "\"error\":{\"code\":\"UNKNOWN_ACTION\",\"message\":\"unknown action\"}}",
            command_id);
    }

    ESP_LOGI(TAG, "cmd %s action=%s -> %.160s", command_id, action, resp);

    if (n <= 0 || n >= (int)sizeof(resp)) {
        static const char fallback[] = "{\"status\":\"error\",\"error\":{\"code\":\"INTERNAL\"}}";
        memcpy(resp, fallback, sizeof(fallback));
        n = sizeof(fallback) - 1;
    }
    if (is_v2) {
        cJSON *legacy = cJSON_Parse(resp);
        cJSON *legacy_status = legacy ? cJSON_GetObjectItem(legacy, "status") : NULL;
        cJSON *legacy_data = legacy ? cJSON_GetObjectItem(legacy, "data") : NULL;
        cJSON *legacy_error = legacy ? cJSON_GetObjectItem(legacy, "error") : NULL;
        const bool ok = cJSON_IsString(legacy_status) &&
            strcmp(legacy_status->valuestring, "ok") == 0;
        const bool needs_readback = strcmp(action, "set_param") == 0 ||
            strcmp(action, "power_on") == 0 || strcmp(action, "power_off") == 0;
        cJSON *envelope = cJSON_CreateObject();
        cJSON *body = cJSON_CreateObject();
        if (!legacy || !envelope || !body) {
            cJSON_Delete(legacy); cJSON_Delete(envelope); cJSON_Delete(body);
            cJSON_free(canonical_request);
            cJSON_Delete(root); return BLE_ATT_ERR_UNLIKELY;
        }
        cJSON_AddNumberToObject(envelope, "v", 2);
        cJSON_AddStringToObject(envelope, "type", "control.result");
        char result_message_id[80];
        snprintf(result_message_id, sizeof(result_message_id), "result-%s", command_id);
        cJSON_AddStringToObject(envelope, "message_id", result_message_id);
        cJSON_AddStringToObject(envelope, "in_reply_to", message_id);
        cJSON_AddStringToObject(envelope, "session_id", ble_ct_get_session_id());
        cJSON_AddStringToObject(body, "operation_id", operation_id);
        cJSON_AddStringToObject(body, "command_id", command_id);
        cJSON_AddStringToObject(body, "status",
            ok ? (needs_readback ? "accepted" : "applied") : "rejected");
        cJSON_AddStringToObject(body, "result_id", result_message_id);
        if (legacy_error) cJSON_AddItemToObject(body, "error", cJSON_Duplicate(legacy_error, true));
        else cJSON_AddNullToObject(body, "error");
        if (legacy_data) cJSON_AddItemToObject(body, "actual", cJSON_Duplicate(legacy_data, true));
        else cJSON_AddItemToObject(body, "actual", cJSON_CreateObject());
        cJSON_AddNumberToObject(body, "parameter_revision", 0);
        cJSON_AddItemToObject(envelope, "body", body);
        char *encoded = cJSON_PrintUnformatted(envelope);
        if (!encoded || strlen(encoded) >= sizeof(resp)) {
            cJSON_free(encoded); cJSON_Delete(envelope); cJSON_Delete(legacy);
            cJSON_free(canonical_request);
            cJSON_Delete(root); return BLE_ATT_ERR_UNLIKELY;
        }
        strcpy(resp, encoded);
        n = (int)strlen(resp);
        cJSON_free(encoded); cJSON_Delete(envelope); cJSON_Delete(legacy);
    }
    ble_ct_cmd_notify_result(resp, (uint16_t)n);
    cache_add(command_id, canonical_request, resp);
    cJSON_free(canonical_request);

    cJSON_Delete(root);
    return 0;
}

void ble_ct_cmd_notify_result(const char *json, uint16_t len) {
    if (!json || len == 0 || len >= RESULT_CAPACITY) return;
    uint16_t conn = ble_ct_get_conn_handle();
    if (conn == BLE_HS_CONN_HANDLE_NONE) return;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(json, len);
    if (om) {
        int rc = ble_gattc_notify_custom(conn, ble_ct_get_result_handle(), om);
        if (rc != 0) ESP_LOGW(TAG, "result notify failed rc=%d", rc);
    }
}
