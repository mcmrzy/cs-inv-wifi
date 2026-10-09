/*
 * ota_mqtt_adapter.c — MQTT OTA 命令适配器实现
 *
 * 从 esp32c3_idf mqtt_handler.c (492-824 行) 提取, 仅保留 OTA 相关逻辑:
 *  - 解析 cs_inv/{sn}/ota/cmd 消息 (MqttOtaCommand)
 *  - 映射到 ota_service (start/cancel/query)
 *  - 状态/ACK 上报到 cs_inv/{sn}/ota/status 与 cs_inv/{sn}/cmd/response
 */
#include "ota_mqtt_adapter.h"
#include "ota_service.h"
#include "mqtt_handler.h"
#include "cs_protocol_defs.h"
#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <string.h>
#include <math.h>
#include <time.h>

static const char *TAG = "mqtt_ota";

/* Request correlation exists only while the MQTT event task handles a command.
 * OTA worker reports use the immutable snapshot stored by ota_service. */
static char g_request_task_id[64];
static char g_request_version[32];
static char g_request_target[8];
static OtaState g_last_published_state = OTA_STATE_IDLE;
static uint8_t g_last_published_progress;
static char g_last_published_stage[16];
static char g_last_published_task[64];

/* ---- 命令结构 (与云端协议一致) ---- */
typedef struct {
    uint16_t protocol_version;
    char action[16];
    char target[8];
    char firmware_id[64];
    char url[256];
    char version[32];
    char sha256[65];
    char signature[132];
    uint32_t security_version;
    uint32_t file_size;
    uint16_t timeout;
    char task_id[64];
    char ota_task_id[64];
    uint32_t timestamp;
} MqttOtaCommand;

/* 静态分配命令结构体，避免栈溢出（~700 字节） */
static MqttOtaCommand g_ota_cmd;

static bool copy_json_string(cJSON *root, const char *name,
                             char *destination, size_t capacity)
{
    cJSON *value = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!value) {
        destination[0] = '\0';
        return true;
    }
    if (!cJSON_IsString(value) || !value->valuestring ||
        strlen(value->valuestring) >= capacity) {
        ESP_LOGW(TAG, "copy_json_string failed: %s (is_str=%d val=%s cap=%d)",
                 name, cJSON_IsString(value),
                 value->valuestring ? value->valuestring : "NULL",
                 (int)capacity);
        return false;
    }
    strlcpy(destination, value->valuestring, capacity);
    return true;
}

static bool copy_json_u32(cJSON *root, const char *name, uint32_t *output)
{
    cJSON *value = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!value) return true;
    if (!cJSON_IsNumber(value) || !isfinite(value->valuedouble) ||
        value->valuedouble < 0 || value->valuedouble > UINT32_MAX ||
        floor(value->valuedouble) != value->valuedouble) {
        return false;
    }
    *output = (uint32_t)value->valuedouble;
    return true;
}

static bool parse_ota_command(const uint8_t *payload, unsigned int length,
                              MqttOtaCommand *command)
{
    if (!payload || !command || length == 0 || length > 2048) return false;
    cJSON *root = cJSON_ParseWithLength((const char *)payload, length);
    if (!root) return false;

    memset(command, 0, sizeof(*command));
    bool valid = false;
    cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "v");
    if (!cJSON_IsNumber(version) || version->valueint != 1 ||
        version->valuedouble != 1.0) {
        goto done;
    }
    command->protocol_version = 1;

    if (!copy_json_string(root, "action", command->action, sizeof(command->action)) ||
        !copy_json_string(root, "target", command->target, sizeof(command->target)) ||
        !copy_json_string(root, "firmware_id", command->firmware_id, sizeof(command->firmware_id)) ||
        !copy_json_string(root, "url", command->url, sizeof(command->url)) ||
        !copy_json_string(root, "version", command->version, sizeof(command->version)) ||
        !copy_json_string(root, "sha256", command->sha256, sizeof(command->sha256)) ||
        !copy_json_string(root, "signature", command->signature, sizeof(command->signature)) ||
        !copy_json_string(root, "task_id", command->task_id, sizeof(command->task_id)) ||
        !copy_json_string(root, "ota_task_id", command->ota_task_id, sizeof(command->ota_task_id))) {
        goto done;
    }

    cJSON *size = cJSON_GetObjectItemCaseSensitive(root, "size");
    if (!size) size = cJSON_GetObjectItemCaseSensitive(root, "file_size");
    if (size) {
        if (!cJSON_IsNumber(size) || !isfinite(size->valuedouble) ||
            size->valuedouble <= 0 || size->valuedouble > UINT32_MAX ||
            floor(size->valuedouble) != size->valuedouble) {
            goto done;
        }
        command->file_size = (uint32_t)size->valuedouble;
    }
    if (!copy_json_u32(root, "security_version", &command->security_version)) goto done;

    uint32_t timeout = 300;
    if (!copy_json_u32(root, "timeout", &timeout) || timeout > UINT16_MAX) goto done;
    command->timeout = (uint16_t)timeout;

    cJSON *timestamp = cJSON_GetObjectItemCaseSensitive(root, "t");
    if (!timestamp) timestamp = cJSON_GetObjectItemCaseSensitive(root, "timestamp");
    if (!timestamp || !cJSON_IsNumber(timestamp) || !isfinite(timestamp->valuedouble) ||
        timestamp->valuedouble <= 0 || timestamp->valuedouble > UINT32_MAX ||
        floor(timestamp->valuedouble) != timestamp->valuedouble) {
        goto done;
    }
    command->timestamp = (uint32_t)timestamp->valuedouble;
    valid = command->action[0] && command->task_id[0];

done:
    cJSON_Delete(root);
    if (!valid) {
        ESP_LOGW(TAG, "parse_ota_command failed");
    } else {
        ESP_LOGI(TAG, "OTA parsed: action=%s target=%s ver=%s size=%lu",
                 command->action, command->target, command->version,
                 (unsigned long)command->file_size);
    }
    return valid;
}

/* ---- 无 snprintf 的轻量 JSON 构建器（避免栈溢出） ----
 * 构建缓冲是静态共享的, 而"构建+发布"整段会被两个任务并发进入:
 *   - MQTT 命令任务: send_ota_ack / publish_failure / handle_query
 *   - OTA worker:    update_snapshot → ota_status_observer → publish_snapshot_json
 * (start_remote 里的 reserve_manifest 与 finish_failure 也会在命令任务上回调
 *  observer, 所以同一次 OTA 的两端确实会同时进这个临界区。)
 * 无锁时后进入者会顶掉前者的内容: 表现为 ack 载荷被发到 ota/status(或反之),
 * 以及一条状态/进度上报被整条吞掉 —— 云端因此卡在 accepted, 收不到失败原因。
 * 故用互斥量把 json_begin..json_publish 串行化; 除 publish_snapshot /
 * send_ota_ack / publish_failure / ota_status_observer 外, 本文件不再有裸的
 * 构建调用点。 */
static char s_json_buf[1024];
static int s_json_pos;
static SemaphoreHandle_t s_json_lock;

static bool json_lock(void)
{
    return !s_json_lock || xSemaphoreTake(s_json_lock, portMAX_DELAY) == pdTRUE;
}

static void json_unlock(void)
{
    if (s_json_lock) xSemaphoreGive(s_json_lock);
}

static void json_begin(void) { s_json_pos = 0; s_json_buf[0] = '{'; s_json_pos = 1; }

static void json_str(const char *key, const char *val) {
    int n = s_json_pos;
    if (n > 1) s_json_buf[n++] = ',';
    s_json_buf[n++] = '"';
    while (*key && n < (int)sizeof(s_json_buf) - 4) s_json_buf[n++] = *key++;
    s_json_buf[n++] = '"'; s_json_buf[n++] = ':';
    s_json_buf[n++] = '"';
    while (*val && n < (int)sizeof(s_json_buf) - 4) s_json_buf[n++] = *val++;
    s_json_buf[n++] = '"';
    s_json_pos = n;
}

static void json_num(const char *key, int val) {
    int n = s_json_pos;
    if (n > 1) s_json_buf[n++] = ',';
    s_json_buf[n++] = '"';
    while (*key && n < (int)sizeof(s_json_buf) - 20) s_json_buf[n++] = *key++;
    s_json_buf[n++] = '"'; s_json_buf[n++] = ':';
    /* 手动 int→str */
    char tmp[12]; int t = 0;
    if (val == 0) { tmp[t++] = '0'; }
    else { if (val < 0) { s_json_buf[n++] = '-'; val = -val; } while (val > 0 && t < 10) { tmp[t++] = '0' + (val % 10); val /= 10; } }
    while (t > 0 && n < (int)sizeof(s_json_buf) - 1) s_json_buf[n++] = tmp[--t];
    s_json_pos = n;
}

static void json_end(void) {
    s_json_buf[s_json_pos++] = '}';
    s_json_buf[s_json_pos] = '\0';
}

static void json_publish(const char *topic) {
    mqtt_publish(topic, s_json_buf, s_json_pos, 1, 0);
}

/* 调用方必须已持有 s_json_lock */
static void publish_snapshot_json(const OtaSnapshot *snapshot,
                                  const char *request_task_id)
{
    if (!snapshot) return;
    json_begin();
    json_str("task_id", snapshot->task_id);
    if (request_task_id && request_task_id[0] &&
        strcmp(request_task_id, snapshot->task_id) != 0) {
        json_str("request_task_id", request_task_id);
    }
    json_str("target", ota_target_name(snapshot->target));
    json_str("version", snapshot->version);
    json_str("source", ota_source_name(snapshot->source));
    json_str("state", ota_state_name(snapshot->state));
    json_num("progress", (int)snapshot->progress);
    json_str("stage", snapshot->stage);
    json_num("stage_progress", (int)snapshot->stage_progress);
    json_num("overall_progress", (int)snapshot->overall_progress);
    json_num("received", (int)snapshot->received);
    json_num("total", (int)snapshot->total);
    json_str("code", snapshot->error_code);
    json_str("message", snapshot->message);
    json_end();
    json_publish(PROTO_TOPIC_OTA_STATUS);
}

static void publish_snapshot(const OtaSnapshot *snapshot,
                             const char *request_task_id)
{
    if (!snapshot || !json_lock()) return;
    publish_snapshot_json(snapshot, request_task_id);
    json_unlock();
}

static void ota_status_observer(const OtaSnapshot *snapshot)
{
    if (!snapshot || snapshot->source != OTA_SOURCE_MQTT) return;

    /* 节流状态与构建缓冲同属一个临界区: 并发进入时"先比较后写入"会让同一条
     * 状态被重复发布, 或让该发的状态变化被漏掉(例如 downloading@1 被吞)。 */
    if (!json_lock()) return;
    const bool state_changed = snapshot->state != g_last_published_state ||
        strcmp(snapshot->stage, g_last_published_stage) != 0 ||
        strcmp(snapshot->task_id, g_last_published_task) != 0;
    const bool progress_due = snapshot->stage_progress != g_last_published_progress;
    if (state_changed || progress_due) {
        g_last_published_state = snapshot->state;
        g_last_published_progress = snapshot->stage_progress;
        strlcpy(g_last_published_stage, snapshot->stage, sizeof(g_last_published_stage));
        strlcpy(g_last_published_task, snapshot->task_id, sizeof(g_last_published_task));
        publish_snapshot_json(snapshot, NULL);
    }
    json_unlock();
}

void mqtt_ota_init(void)
{
    g_request_task_id[0] = '\0';
    g_request_version[0] = '\0';
    g_request_target[0] = '\0';
    g_last_published_state = OTA_STATE_IDLE;
    g_last_published_progress = 0;
    g_last_published_stage[0] = '\0';
    g_last_published_task[0] = '\0';
    if (!s_json_lock) {
        s_json_lock = xSemaphoreCreateMutex();
        if (!s_json_lock) {
            ESP_LOGE(TAG, "json lock create failed: OTA reports may interleave");
        }
    }
    ota_service_set_observer(ota_status_observer);
    ESP_LOGI(TAG, "MQTT OTA command adapter initialized");
}

static void send_ota_ack(const char *task_id, bool accepted, const char *message)
{
    if (!json_lock()) return;
    json_begin();
    json_str("task_id", task_id ? task_id : "");
    json_str("ack", accepted ? "true" : "false");
    json_str("message", message ? message : "");
    json_end();
    json_publish(PROTO_TOPIC_CMD_RESP);
    json_unlock();
}

static bool command_expired(const MqttOtaCommand *command)
{
    const time_t now = time(NULL);
    /* 时钟未同步(未过 2024-01-01)时不做时间判断, 沿用原行为 */
    if (now < 1704067200) return false;

    const uint32_t now_s = (uint32_t)now;
    /* 未来时间戳不等于重放: 云端时钟比设备快是常态(实测差 1 秒), 小偏差必须放行,
     * 否则刚发出的命令会被当成"来自未来"而拒绝(2026-09-22 的 EXPIRED 就是这么来的)。 */
    if (command->timestamp > now_s) {
        return command->timestamp - now_s > PROTO_CMD_FUTURE_SLACK_S;
    }
    return now_s - command->timestamp > PROTO_CMD_EXPIRE_S;
}

/* 失败态上报 (服务拒绝 / 命令过期 / 动作不支持 / JSON 解析失败共用同一形状):
 * 收敛成一个加锁的 helper, 免得四处各拼一遍时漏锁、或被他人的构建顶掉。 */
static void publish_failure(const char *task_id, const char *target,
                            const char *version, const char *code,
                            const char *message)
{
    if (!json_lock()) return;
    json_begin();
    json_str("task_id", task_id ? task_id : "");
    json_str("target", target ? target : "");
    json_str("version", version ? version : "");
    json_str("state", PROTO_OTA_STATE_FAILED);
    json_num("progress", 0);
    json_str("code", code ? code : "");
    json_str("message", message ? message : "");
    json_end();
    json_publish(PROTO_TOPIC_OTA_STATUS);
    json_unlock();
}

static void report_service_result(const MqttOtaCommand *command, OtaResult result)
{
    const char *code = ota_result_code(result);
    if (result == OTA_RESULT_DUPLICATE) {
        send_ota_ack(command->task_id, true, "Duplicate OTA task already active");
        OtaSnapshot snapshot;
        if (ota_service_get_snapshot(&snapshot)) publish_snapshot(&snapshot, command->task_id);
        return;
    }
    send_ota_ack(command->task_id, false, code);
    publish_failure(command->task_id, command->target, command->version, code, code);
}

static void handle_start(const MqttOtaCommand *command)
{
    /* signature 和 security_version 可为空/0（本地测试时无签名） */
    ESP_LOGI(TAG, "handle_start: target=[%s] url=[%s] ver=[%s] sha256=[%s] size=%lu",
             command->target, command->url, command->version,
             command->sha256, (unsigned long)command->file_size);
    if (!command->target[0] || !command->url[0] || !command->version[0] ||
        !command->sha256[0] || command->file_size == 0) {
        ESP_LOGW(TAG, "INVALID_PARAMS: target=%s url=%s ver=%s sha256=%s size=%lu",
                 command->target, command->url, command->version,
                 command->sha256, (unsigned long)command->file_size);
        report_service_result(command, OTA_RESULT_INVALID_PARAMS);
        return;
    }
    if (command_expired(command)) {
        send_ota_ack(command->task_id, false, PROTO_ERR_EXPIRED);
        publish_failure(command->task_id, command->target, command->version,
                        PROTO_ERR_EXPIRED, "OTA command expired");
        return;
    }

    OtaTarget target;
    if (strcmp(command->target, "esp") == 0) target = OTA_TARGET_ESP;
    else if (strcmp(command->target, "arm") == 0) target = OTA_TARGET_ARM;
    else if (strcmp(command->target, "dsp") == 0) target = OTA_TARGET_DSP;
    else if (strcmp(command->target, "bms") == 0) target = OTA_TARGET_BMS;
    else {
        report_service_result(command, OTA_RESULT_INVALID_TARGET);
        return;
    }

    OtaManifest manifest = {
        .source = OTA_SOURCE_MQTT,
        .target = target,
        .security_version = command->security_version,
        .file_size = command->file_size,
        .timeout_seconds = command->timeout,
        .timestamp = command->timestamp,
    };
    strlcpy(manifest.task_id, command->task_id, sizeof(manifest.task_id));
    strlcpy(manifest.version, command->version, sizeof(manifest.version));
    strlcpy(manifest.url, command->url, sizeof(manifest.url));
    strlcpy(manifest.sha256, command->sha256, sizeof(manifest.sha256));
    strlcpy(manifest.signature, command->signature, sizeof(manifest.signature));

    ESP_LOGI(TAG, "OTA manifest: task=%s ver=%s url=%s sha256=%s sig=%s size=%lu",
             manifest.task_id, manifest.version, manifest.url,
             manifest.sha256, manifest.signature,
             (unsigned long)manifest.file_size);

    const OtaResult result = ota_service_start_remote(&manifest);
    ESP_LOGI(TAG, "OTA start_remote result=%d (%s)", result, ota_result_code(result));
    if (result == OTA_RESULT_OK) {
        send_ota_ack(command->task_id, true, "OTA task accepted");
    } else {
        report_service_result(command, result);
    }
}

static void handle_cancel(const MqttOtaCommand *command)
{
    const char *active_task = command->ota_task_id[0]
        ? command->ota_task_id : ota_service_get_task_id();
    const OtaResult result = ota_service_cancel(active_task);
    if (result == OTA_RESULT_OK) {
        send_ota_ack(command->task_id, true, "OTA cancellation requested");
    } else {
        report_service_result(command, result);
    }
}

static void handle_query(const MqttOtaCommand *command)
{
    OtaSnapshot snapshot;
    if (!ota_service_get_snapshot(&snapshot)) {
        report_service_result(command, OTA_RESULT_INTERNAL);
        return;
    }
    send_ota_ack(command->task_id, true, "OTA status follows");
    publish_snapshot(&snapshot, command->task_id);
}

bool mqtt_ota_handle_command(const char *topic, const uint8_t *payload,
                             unsigned int length)
{
    (void)topic;
    /* 使用静态分配的命令结构体，避免栈溢出 */
    if (!parse_ota_command(payload, length, &g_ota_cmd)) {
        g_request_task_id[0] = '\0';
        g_request_version[0] = '\0';
        g_request_target[0] = '\0';
        publish_failure("", "unknown", "", "JSON_ERROR", "Invalid OTA command");
        return false;
    }

    strlcpy(g_request_task_id, g_ota_cmd.task_id, sizeof(g_request_task_id));
    strlcpy(g_request_version, g_ota_cmd.version, sizeof(g_request_version));
    strlcpy(g_request_target, g_ota_cmd.target, sizeof(g_request_target));

    if (strcmp(g_ota_cmd.action, PROTO_OTA_ACTION_START) == 0) {
        handle_start(&g_ota_cmd);
    } else if (strcmp(g_ota_cmd.action, PROTO_OTA_ACTION_CANCEL) == 0) {
        handle_cancel(&g_ota_cmd);
    } else if (strcmp(g_ota_cmd.action, PROTO_OTA_ACTION_QUERY) == 0) {
        handle_query(&g_ota_cmd);
    } else {
        send_ota_ack(g_ota_cmd.task_id, false, PROTO_ERR_NOT_SUPPORTED);
        publish_failure(g_ota_cmd.task_id, g_ota_cmd.target, g_ota_cmd.version,
                        PROTO_ERR_NOT_SUPPORTED, "Unsupported OTA action");
    }

    g_request_task_id[0] = '\0';
    g_request_version[0] = '\0';
    g_request_target[0] = '\0';
    return true;
}

void mqtt_ota_process(void)
{
    ota_service_process();
}

bool mqtt_ota_is_busy(void)
{
    return ota_service_is_busy();
}

const char *mqtt_ota_get_task_id(void)
{
    return ota_service_get_task_id();
}
