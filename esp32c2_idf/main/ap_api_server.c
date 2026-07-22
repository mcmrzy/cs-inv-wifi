#include "ap_api_server.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "arm_ota.h"
#include "cJSON.h"
#include "config.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "ota_service.h"
#include "state_machine.h"

#define LOCAL_UPLOAD_TASK_STACK  6144
#define LOCAL_UPLOAD_TASK_PRIO   5

extern EventGroupHandle_t s_wifi_event_group;
extern const int WIFI_CONNECTED_BIT;

static const char* TAG = "ota_http";
static httpd_handle_t g_server;

typedef struct {
    httpd_req_t* request;
    OtaManifest manifest;
} LocalUploadContext;

static void set_json_headers(httpd_req_t* request)
{
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
}

static esp_err_t send_json(httpd_req_t* request, cJSON* root, const char* status)
{
    if (!root) return ESP_ERR_NO_MEM;
    char* body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) return ESP_ERR_NO_MEM;
    set_json_headers(request);
    if (status) httpd_resp_set_status(request, status);
    const esp_err_t result = httpd_resp_send(request, body, HTTPD_RESP_USE_STRLEN);
    cJSON_free(body);
    return result;
}

static esp_err_t send_result(httpd_req_t* request, bool ok,
                             const char* code, const char* message,
                             const char* status)
{
    cJSON* root = cJSON_CreateObject();
    if (!root) return ESP_ERR_NO_MEM;
    cJSON_AddBoolToObject(root, "ok", ok);
    cJSON_AddStringToObject(root, "code", code ? code : "OK");
    cJSON_AddStringToObject(root, "message", message ? message : "");
    return send_json(request, root, status);
}

static void add_snapshot(cJSON* root, const OtaSnapshot* snapshot)
{
    cJSON_AddStringToObject(root, "task_id", snapshot->task_id);
    cJSON_AddStringToObject(root, "target", ota_target_name(snapshot->target));
    cJSON_AddStringToObject(root, "source", ota_source_name(snapshot->source));
    cJSON_AddStringToObject(root, "version", snapshot->version);
    cJSON_AddStringToObject(root, "state", ota_state_name(snapshot->state));
    cJSON_AddNumberToObject(root, "progress", snapshot->progress);
    cJSON_AddNumberToObject(root, "received", snapshot->received);
    cJSON_AddNumberToObject(root, "total", snapshot->total);
    cJSON_AddStringToObject(root, "code", snapshot->error_code);
    cJSON_AddStringToObject(root, "message", snapshot->message);
}

static esp_err_t ota_progress_handler(httpd_req_t* request)
{
    OtaSnapshot snapshot;
    if (!ota_service_get_snapshot(&snapshot)) {
        return send_result(request, false, "INTERNAL_ERROR", "OTA service unavailable",
                           "500 Internal Server Error");
    }
    cJSON* root = cJSON_CreateObject();
    if (!root) return ESP_ERR_NO_MEM;
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddBoolToObject(root, "busy", ota_service_is_busy());
    add_snapshot(root, &snapshot);
    return send_json(request, root, NULL);
}

static esp_err_t ota_info_handler(httpd_req_t* request)
{
    cJSON* root = cJSON_CreateObject();
    if (!root) return ESP_ERR_NO_MEM;
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "esp_version", FIRMWARE_VERSION);
    const ArmFirmwareInfo* arm = arm_ota_get_firmware_info();
    cJSON_AddStringToObject(root, "arm_version",
                            arm && arm->valid ? arm->version : "unknown");
    cJSON_AddBoolToObject(root, "signature_required", true);
    cJSON_AddStringToObject(root, "signature_algorithm", "Ed25519");
    cJSON_AddStringToObject(root, "hash_algorithm", "SHA-256");
    return send_json(request, root, NULL);
}

static bool get_header(httpd_req_t* request, const char* name,
                       char* output, size_t capacity, bool required)
{
    const esp_err_t result = httpd_req_get_hdr_value_str(request, name, output, capacity);
    if (result == ESP_OK) return output[0] != '\0';
    output[0] = '\0';
    return !required && result == ESP_ERR_NOT_FOUND;
}

static bool parse_u32(const char* value, uint32_t* output)
{
    if (!value || !value[0]) return false;
    errno = 0;
    char* end = NULL;
    const unsigned long parsed = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed > UINT32_MAX) return false;
    *output = (uint32_t)parsed;
    return true;
}

static int local_stream_read(void* context, uint8_t* buffer, size_t capacity)
{
    httpd_req_t* request = (httpd_req_t*)context;
    const int result = httpd_req_recv(request, (char*)buffer, capacity);
    if (result == HTTPD_SOCK_ERR_TIMEOUT) return 0;
    return result > 0 ? result : -1;
}

static void local_upload_task(void* parameter)
{
    LocalUploadContext* context = (LocalUploadContext*)parameter;
    const OtaResult result = ota_service_start_local(
        &context->manifest, local_stream_read, context->request);
    const bool ok = result == OTA_RESULT_OK;

    OtaSnapshot snapshot = {0};
    ota_service_get_snapshot(&snapshot);
    cJSON* root = cJSON_CreateObject();
    if (root) {
        cJSON_AddBoolToObject(root, "ok", ok);
        add_snapshot(root, &snapshot);
        send_json(context->request, root,
                  ok ? NULL : "400 Bad Request");
    }
    httpd_req_async_handler_complete(context->request);

    if (ok && context->manifest.target == OTA_TARGET_ESP) {
        ota_service_schedule_reboot(1000);
    }
    free(context);
    vTaskDelete(NULL);
}

static esp_err_t ota_upload_handler(httpd_req_t* request)
{
    if (request->content_len <= 0) {
        return send_result(request, false, "INVALID_SIZE", "Firmware body is empty",
                           "400 Bad Request");
    }

    LocalUploadContext* context = calloc(1, sizeof(*context));
    if (!context) return send_result(request, false, "NO_MEMORY", "Out of memory",
                                     "500 Internal Server Error");

    OtaManifest* manifest = &context->manifest;
    manifest->source = OTA_SOURCE_LOCAL;
    manifest->file_size = (uint32_t)request->content_len;
    manifest->timeout_seconds = 300;
    char target[8];
    char file_size[16];
    char security_version[16];
    char timeout[8];
    uint32_t declared_size = 0;
    const bool headers_ok =
        get_header(request, "X-OTA-Size", file_size, sizeof(file_size), true) &&
        get_header(request, "X-OTA-Target", target, sizeof(target), true) &&
        get_header(request, "X-OTA-Task-Id", manifest->task_id, sizeof(manifest->task_id), true) &&
        get_header(request, "X-OTA-Version", manifest->version, sizeof(manifest->version), true) &&
        get_header(request, "X-OTA-SHA256", manifest->sha256, sizeof(manifest->sha256), true) &&
        get_header(request, "X-OTA-Signature", manifest->signature, sizeof(manifest->signature), true) &&
        get_header(request, "X-OTA-Security-Version", security_version, sizeof(security_version), true) &&
        get_header(request, "X-OTA-Timeout", timeout, sizeof(timeout), false) &&
        parse_u32(file_size, &declared_size) &&
        declared_size == manifest->file_size &&
        parse_u32(security_version, &manifest->security_version);
    uint32_t timeout_value = manifest->timeout_seconds;
    const bool timeout_ok = !timeout[0] ||
        (parse_u32(timeout, &timeout_value) && timeout_value <= UINT16_MAX);
    if (!headers_ok || !timeout_ok) {
        free(context);
        return send_result(request, false, "INVALID_METADATA",
                           "Required X-OTA metadata is missing or invalid", "400 Bad Request");
    }
    manifest->timeout_seconds = (uint16_t)timeout_value;

    if (strcmp(target, "esp") == 0) manifest->target = OTA_TARGET_ESP;
    else if (strcmp(target, "arm") == 0) manifest->target = OTA_TARGET_ARM;
    else {
        free(context);
        return send_result(request, false, "INVALID_TARGET", "target must be esp or arm",
                           "400 Bad Request");
    }

    if (httpd_req_async_handler_begin(request, &context->request) != ESP_OK) {
        free(context);
        return send_result(request, false, "SERVER_BUSY", "Cannot start upload worker",
                           "503 Service Unavailable");
    }
    if (xTaskCreate(local_upload_task, "ota_local", LOCAL_UPLOAD_TASK_STACK,
                    context, LOCAL_UPLOAD_TASK_PRIO, NULL) != pdPASS) {
        send_result(context->request, false, "TASK_CREATE_FAILED", "Cannot start upload worker",
                    "500 Internal Server Error");
        httpd_req_async_handler_complete(context->request);
        free(context);
        return ESP_OK;
    }
    return ESP_OK;
}

static esp_err_t ota_cancel_handler(httpd_req_t* request)
{
    char body[128] = {0};
    char task_id[64] = {0};
    if (request->content_len > 0) {
        char* buf = body;
        size_t received = 0;
        while (received < (size_t)request->content_len && received < sizeof(body) - 1) {
            int count = httpd_req_recv(request, buf + received, request->content_len - received);
            if (count <= 0) break;
            received += count;
        }
        body[received] = '\0';
        cJSON* root = cJSON_Parse(body);
        cJSON* value = root ? cJSON_GetObjectItemCaseSensitive(root, "task_id") : NULL;
        if (cJSON_IsString(value) && value->valuestring) {
            strlcpy(task_id, value->valuestring, sizeof(task_id));
        }
        cJSON_Delete(root);
    }
    const OtaResult result = ota_service_cancel(task_id);
    return send_result(request, result == OTA_RESULT_OK, ota_result_code(result),
                       result == OTA_RESULT_OK ? "OTA cancelled" : ota_result_code(result),
                       result == OTA_RESULT_OK ? NULL : "409 Conflict");
}

static esp_err_t not_found_handler(httpd_req_t* request, httpd_err_code_t error)
{
    (void)error;
    return send_result(request, false, "NOT_FOUND", "API route not found", "404 Not Found");
}

bool ap_api_server_start(void)
{
    if (g_server) return true;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 4096;
    config.max_uri_handlers = 4;
    config.max_open_sockets = 2;
    config.recv_wait_timeout = 5;
    config.send_wait_timeout = 5;
    if (httpd_start(&g_server, &config) != ESP_OK) {
        g_server = NULL;
        return false;
    }

    const httpd_uri_t routes[] = {
        {.uri = "/ota/info",     .method = HTTP_GET,  .handler = ota_info_handler},
        {.uri = "/ota/progress", .method = HTTP_GET,  .handler = ota_progress_handler},
        {.uri = "/ota/upload",   .method = HTTP_POST, .handler = ota_upload_handler},
        {.uri = "/ota/cancel",   .method = HTTP_POST, .handler = ota_cancel_handler},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i) {
        if (httpd_register_uri_handler(g_server, &routes[i]) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register route %s", routes[i].uri);
            httpd_stop(g_server);
            g_server = NULL;
            return false;
        }
    }
    httpd_register_err_handler(g_server, HTTPD_404_NOT_FOUND, not_found_handler);
    ESP_LOGI(TAG, "OTA HTTP server started");
    return true;
}

void ap_api_server_stop(void)
{
    if (!g_server) return;
    httpd_stop(g_server);
    g_server = NULL;
}
