#include "mqtt_ota_handler.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "cs_protocol_defs.h"
#include "esp_log.h"
#include "mqtt_handler.h"
#include "ota_service.h"

static const char* TAG = "mqtt_ota";

/* Request correlation exists only while the MQTT event task handles a command.
 * OTA worker reports use the immutable snapshot stored by ota_service. */
static char g_request_task_id[64];
static char g_request_version[32];
static char g_request_target[8];
static OtaState g_last_published_state = OTA_STATE_IDLE;
static uint8_t g_last_published_progress;

static bool copy_json_string(cJSON* root, const char* name,
                             char* destination, size_t capacity)
{
    cJSON* value = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!value) {
        destination[0] = '\0';
        return true;
    }
    if (!cJSON_IsString(value) || !value->valuestring ||
        strlen(value->valuestring) >= capacity) {
        return false;
    }
    strlcpy(destination, value->valuestring, capacity);
    return true;
}

static bool copy_json_u32(cJSON* root, const char* name, uint32_t* output)
{
    cJSON* value = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!value) return true;
    if (!cJSON_IsNumber(value) || !isfinite(value->valuedouble) ||
        value->valuedouble < 0 || value->valuedouble > UINT32_MAX ||
        floor(value->valuedouble) != value->valuedouble) {
        return false;
    }
    *output = (uint32_t)value->valuedouble;
    return true;
}

static bool parse_ota_command(const uint8_t* payload, unsigned int length,
                              struct MqttOtaCommand* command)
{
    if (!payload || !command || length == 0 || length > 2048) return false;
    cJSON* root = cJSON_ParseWithLength((const char*)payload, length);
    if (!root) return false;

    memset(command, 0, sizeof(*command));
    bool valid = false;
    cJSON* version = cJSON_GetObjectItemCaseSensitive(root, "v");
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

    cJSON* size = cJSON_GetObjectItemCaseSensitive(root, "size");
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

    cJSON* timestamp = cJSON_GetObjectItemCaseSensitive(root, "t");
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
    return valid;
}

static void publish_object(cJSON* root, const char* topic)
{
    if (!root) return;
    char* payload = cJSON_PrintUnformatted(root);
    if (payload) {
        mqtt_publish(topic, payload, (int)strlen(payload), 1, 0);
        cJSON_free(payload);
    }
    cJSON_Delete(root);
}

static void publish_snapshot(const OtaSnapshot* snapshot, const char* request_task_id)
{
    if (!snapshot) return;
    cJSON* root = cJSON_CreateObject();
    if (!root) return;
    cJSON_AddStringToObject(root, "task_id", snapshot->task_id);
    if (request_task_id && request_task_id[0] &&
        strcmp(request_task_id, snapshot->task_id) != 0) {
        cJSON_AddStringToObject(root, "request_task_id", request_task_id);
    }
    cJSON_AddStringToObject(root, "target", ota_target_name(snapshot->target));
    cJSON_AddStringToObject(root, "version", snapshot->version);
    cJSON_AddStringToObject(root, "source", ota_source_name(snapshot->source));
    cJSON_AddStringToObject(root, "state", ota_state_name(snapshot->state));
    cJSON_AddNumberToObject(root, "progress", snapshot->progress);
    cJSON_AddNumberToObject(root, "received", snapshot->received);
    cJSON_AddNumberToObject(root, "total", snapshot->total);
    cJSON_AddStringToObject(root, "code", snapshot->error_code);
    cJSON_AddStringToObject(root, "message", snapshot->message);
    publish_object(root, PROTO_TOPIC_OTA_STATUS);
}

static void ota_status_observer(const OtaSnapshot* snapshot)
{
    if (!snapshot || snapshot->source != OTA_SOURCE_MQTT) return;

    /* Preserve full local progress while limiting MQTT traffic to state changes
     * and roughly 5% increments. */
    const bool state_changed = snapshot->state != g_last_published_state;
    const bool progress_due = snapshot->progress >= g_last_published_progress + 5;
    if (!state_changed && !progress_due && snapshot->progress != 100) return;

    g_last_published_state = snapshot->state;
    g_last_published_progress = snapshot->progress;
    publish_snapshot(snapshot, NULL);
}

void mqtt_ota_init(void)
{
    g_request_task_id[0] = '\0';
    g_request_version[0] = '\0';
    g_request_target[0] = '\0';
    g_last_published_state = OTA_STATE_IDLE;
    g_last_published_progress = 0;
    ota_service_set_observer(ota_status_observer);
    ESP_LOGI(TAG, "MQTT OTA command adapter initialized");
}

void mqtt_ota_report_status(const char* target, const char* state, int progress,
                            const char* code, const char* detail)
{
    cJSON* root = cJSON_CreateObject();
    if (!root) return;
    cJSON_AddStringToObject(root, "task_id", g_request_task_id);
    cJSON_AddStringToObject(root, "target", target ? target : g_request_target);
    cJSON_AddStringToObject(root, "version", g_request_version);
    cJSON_AddStringToObject(root, "state", state ? state : PROTO_OTA_STATE_ACCEPTED);
    cJSON_AddNumberToObject(root, "progress", progress);
    cJSON_AddStringToObject(root, "code", code ? code : PROTO_ERR_OK);
    cJSON_AddStringToObject(root, "message", detail ? detail : "");
    publish_object(root, PROTO_TOPIC_OTA_STATUS);
}

void mqtt_ota_report_error(const char* target, const char* error_code,
                           const char* error_message)
{
    cJSON* root = cJSON_CreateObject();
    if (!root) return;
    cJSON_AddStringToObject(root, "task_id", g_request_task_id);
    cJSON_AddStringToObject(root, "target", target ? target : g_request_target);
    cJSON_AddStringToObject(root, "version", g_request_version);
    cJSON_AddStringToObject(root, "state", PROTO_OTA_STATE_FAILED);
    cJSON_AddNumberToObject(root, "progress", 0);
    cJSON_AddStringToObject(root, "code", error_code ? error_code : PROTO_ERR_EXEC_FAILED);
    cJSON_AddStringToObject(root, "message", error_message ? error_message : "OTA failed");
    publish_object(root, PROTO_TOPIC_OTA_STATUS);
}

static void send_ota_ack(const char* task_id, bool accepted, const char* message)
{
    cJSON* root = cJSON_CreateObject();
    if (!root) return;
    cJSON_AddStringToObject(root, "task_id", task_id ? task_id : "");
    cJSON_AddBoolToObject(root, "ack", accepted);
    cJSON_AddStringToObject(root, "message", message ? message : "");
    publish_object(root, PROTO_TOPIC_CMD_RESP);
}

static bool command_expired(const struct MqttOtaCommand* command)
{
    const time_t now = time(NULL);
    return now >= 1704067200 &&
        ((uint32_t)now < command->timestamp ||
         (uint32_t)now - command->timestamp > PROTO_CMD_EXPIRE_S);
}

static void report_service_result(const struct MqttOtaCommand* command, OtaResult result)
{
    if (result == OTA_RESULT_DUPLICATE) {
        send_ota_ack(command->task_id, true, "Duplicate OTA task already active");
        OtaSnapshot snapshot;
        if (ota_service_get_snapshot(&snapshot)) publish_snapshot(&snapshot, command->task_id);
        return;
    }
    send_ota_ack(command->task_id, false, ota_result_code(result));
    mqtt_ota_report_error(command->target, ota_result_code(result), ota_result_code(result));
}

static void handle_start(const struct MqttOtaCommand* command)
{
    if (!command->target[0] || !command->url[0] || !command->version[0] ||
        !command->sha256[0] || !command->signature[0] ||
        command->file_size == 0 || command->security_version == 0) {
        report_service_result(command, OTA_RESULT_INVALID_PARAMS);
        return;
    }
    if (command_expired(command)) {
        send_ota_ack(command->task_id, false, PROTO_ERR_EXPIRED);
        mqtt_ota_report_error(command->target, PROTO_ERR_EXPIRED, "OTA command expired");
        return;
    }

    OtaTarget target;
    if (strcmp(command->target, "esp") == 0) target = OTA_TARGET_ESP;
    else if (strcmp(command->target, "arm") == 0) target = OTA_TARGET_ARM;
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

    const OtaResult result = ota_service_start_remote(&manifest);
    if (result == OTA_RESULT_OK) {
        send_ota_ack(command->task_id, true, "OTA task accepted");
    } else {
        report_service_result(command, result);
    }
}

static void handle_cancel(const struct MqttOtaCommand* command)
{
    const char* active_task = command->ota_task_id[0]
        ? command->ota_task_id : ota_service_get_task_id();
    const OtaResult result = ota_service_cancel(active_task);
    if (result == OTA_RESULT_OK) {
        send_ota_ack(command->task_id, true, "OTA cancellation requested");
    } else {
        report_service_result(command, result);
    }
}

static void handle_query(const struct MqttOtaCommand* command)
{
    OtaSnapshot snapshot;
    if (!ota_service_get_snapshot(&snapshot)) {
        report_service_result(command, OTA_RESULT_INTERNAL);
        return;
    }
    send_ota_ack(command->task_id, true, "OTA status follows");
    publish_snapshot(&snapshot, command->task_id);
}

bool mqtt_ota_handle_command(const char* topic, const uint8_t* payload, unsigned int length)
{
    (void)topic;
    struct MqttOtaCommand command;
    if (!parse_ota_command(payload, length, &command)) {
        g_request_task_id[0] = '\0';
        g_request_version[0] = '\0';
        g_request_target[0] = '\0';
        mqtt_ota_report_error("unknown", "JSON_ERROR", "Invalid OTA command");
        return false;
    }

    strlcpy(g_request_task_id, command.task_id, sizeof(g_request_task_id));
    strlcpy(g_request_version, command.version, sizeof(g_request_version));
    strlcpy(g_request_target, command.target, sizeof(g_request_target));

    if (strcmp(command.action, PROTO_OTA_ACTION_START) == 0) {
        handle_start(&command);
    } else if (strcmp(command.action, PROTO_OTA_ACTION_CANCEL) == 0) {
        handle_cancel(&command);
    } else if (strcmp(command.action, PROTO_OTA_ACTION_QUERY) == 0) {
        handle_query(&command);
    } else {
        send_ota_ack(command.task_id, false, PROTO_ERR_NOT_SUPPORTED);
        mqtt_ota_report_error(command.target, PROTO_ERR_NOT_SUPPORTED, "Unsupported OTA action");
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

const char* mqtt_ota_get_task_id(void)
{
    return ota_service_get_task_id();
}
