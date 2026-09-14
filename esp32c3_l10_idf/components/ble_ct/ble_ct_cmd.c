#include "ble_ct_cmd.h"
#include "ble_ct.h"
#include "ble_ct_auth.h"
#include "ble_ct_clock.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "host/ble_hs.h"
#include "host/ble_gatt.h"
#include "cJSON.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "ble_ct_cmd";

/* 幂等缓存 */
typedef struct {
    char command_id[16];
    char result[256];
    uint32_t ts_ms;
    bool valid;
} CmdCache;

#define CACHE_SIZE 8
#define CACHE_EXPIRE_MS (10 * 60 * 1000)
static CmdCache s_cache[CACHE_SIZE];
static int s_cache_idx = 0;

/* 命令执行回调（主层注入，避免对 main 的编译依赖） */
static ble_cmd_write_fn s_write_fn = NULL;
static ble_cmd_read_fn  s_read_fn = NULL;

void ble_ct_cmd_set_executors(ble_cmd_write_fn write_fn, ble_cmd_read_fn read_fn) {
    s_write_fn = write_fn;
    s_read_fn = read_fn;
}

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static const char *cache_lookup(const char *id) {
    for (int i = 0; i < CACHE_SIZE; i++) {
        if (s_cache[i].valid && strcmp(s_cache[i].command_id, id) == 0) {
            if (now_ms() - s_cache[i].ts_ms > CACHE_EXPIRE_MS) { s_cache[i].valid = false; return NULL; }
            return s_cache[i].result;
        }
    }
    return NULL;
}

static void cache_add(const char *id, const char *result) {
    CmdCache *e = &s_cache[s_cache_idx];
    strncpy(e->command_id, id, sizeof(e->command_id) - 1);
    strncpy(e->result, result, sizeof(e->result) - 1);
    e->ts_ms = now_ms();
    e->valid = true;
    s_cache_idx = (s_cache_idx + 1) % CACHE_SIZE;
}

int ble_ct_cmd_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg) {
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;

    if (!ble_ct_auth_is_authenticated()) {
        ESP_LOGW(TAG, "Command rejected: not authenticated");
        return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
    }

    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    char buf[512] = {0};
    ble_hs_mbuf_to_flat(ctxt->om, buf, len, NULL);

    cJSON *root = cJSON_Parse(buf);
    if (!root) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;

    cJSON *j_id = cJSON_GetObjectItem(root, "command_id");
    cJSON *j_action = cJSON_GetObjectItem(root, "action");
    cJSON *j_params = cJSON_GetObjectItem(root, "params");
    cJSON *j_ts = cJSON_GetObjectItem(root, "ts");

    if (!cJSON_IsString(j_id) || !cJSON_IsString(j_action)) {
        cJSON_Delete(root);
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    const char *command_id = j_id->valuestring;
    const char *action = j_action->valuestring;

    /* 幂等检查 */
    const char *cached = cache_lookup(command_id);
    if (cached) {
        ble_ct_cmd_notify_result(cached, strlen(cached));
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
                cJSON_Delete(root);
                return 0;
            }
        }
    }
    /* ---- action 映射执行（协议 §7.3）；经主层注入的执行器落地 ---- */
    char resp[320];
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
            if (s_read_fn(j_pid->valuestring, &val)) {
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
        int rc = s_write_fn ? s_write_fn("output_priority", v) : BLE_CMD_UNKNOWN;
        n = snprintf(resp, sizeof(resp),
            "{\"command_id\":\"%s\",\"status\":\"%s\","
            "\"data\":{\"applied_priority\":%d}}",
            command_id, (rc == BLE_CMD_OK) ? "ok" : "error",
            (int)v);
    } else if (strcmp(action, "set_power") == 0) {
        /* 输出功率上限 → set_charge_limit（设计文档 §1.3, scale 10=0.1A） */
        cJSON *j_pw = (j_params) ? cJSON_GetObjectItem(j_params, "power_w") : NULL;
        if (!cJSON_IsNumber(j_pw)) {
            n = snprintf(resp, sizeof(resp),
                "{\"command_id\":\"%s\",\"status\":\"error\","
                "\"error\":{\"code\":\"INVALID_ARGS\",\"message\":\"missing power_w\"}}",
                command_id);
        } else {
            int rc = s_write_fn ? s_write_fn("set_charge_limit", j_pw->valuedouble)
                                : BLE_CMD_UNKNOWN;
            n = snprintf(resp, sizeof(resp),
                "{\"command_id\":\"%s\",\"status\":\"%s\","
                "\"data\":{\"applied_power_w\":%g}}",
                command_id, (rc == BLE_CMD_OK) ? "ok" : "error",
                j_pw->valuedouble);
        }
    } else if (strcmp(action, "set_param") == 0) {
        /* 写控制参数：param_id 即 cmd_handler 命令名, value 即工程单位值 */
        cJSON *j_pid = (j_params) ? cJSON_GetObjectItem(j_params, "param_id") : NULL;
        cJSON *j_val = (j_params) ? cJSON_GetObjectItem(j_params, "value") : NULL;
        if (!cJSON_IsString(j_pid) || !j_pid->valuestring[0] ||
            !cJSON_IsNumber(j_val) || !s_write_fn) {
            n = snprintf(resp, sizeof(resp),
                "{\"command_id\":\"%s\",\"status\":\"error\","
                "\"error\":{\"code\":\"INVALID_ARGS\",\"message\":\"need param_id and value\"}}",
                command_id);
        } else {
            int rc = s_write_fn(j_pid->valuestring, j_val->valuedouble);
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
        n = snprintf(resp, sizeof(resp),
            "{\"command_id\":\"%s\",\"status\":\"error\","
            "\"error\":{\"code\":\"INTERNAL\",\"message\":\"response too large\"}}",
            command_id);
    }
    ble_ct_cmd_notify_result(resp, (uint16_t)n);
    cache_add(command_id, resp);

    cJSON_Delete(root);
    return 0;
}

void ble_ct_cmd_notify_result(const char *json, uint16_t len) {
    uint16_t conn = ble_ct_get_conn_handle();
    if (conn == BLE_HS_CONN_HANDLE_NONE) return;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(json, len);
    if (om) {
        int rc = ble_gattc_notify_custom(conn, ble_ct_get_result_handle(), om);
        if (rc != 0) ESP_LOGW(TAG, "result notify failed rc=%d", rc);
    }
}
