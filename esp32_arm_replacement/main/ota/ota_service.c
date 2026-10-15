#include "ota_service.h"
#include "ota_compat.h"
#include "ota_health.h"
#include "ota_image_identity.h"
#include "firmware_identity.h"
#include "factory_identity.h"
#include "factory_service.h"
#include "direct_panel.h"
#include "direct_link.h"
#include "direct_dsp_ota_transport.h"
#include "direct_iap.h"
#include "ota_transfer_policy.h"
#include "offline_cache.h"
#include "telemetry.h"
#include "arm_poll.h"

/* OTA 静默窗口: 定义见 install_esp 之前 */
static void ota_quiet_begin(void);
static void ota_quiet_end(void);

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "arm_ota.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_spiffs.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"

#include "nvs_storage.h"
#include "mbedtls/base64.h"
#include "monocypher-ed25519.h"
#include "mqtt_handler.h"
#include "ble_prov.h"
#include "ota_release_pubkey.h"

/* HTTP buffering is separate from TLS records. Leave room for a complete
 * 16KB TLS record alongside MQTT, the full worker stack and staging buffer. */
#define OTA_BUFFER_SIZE       1024U
#define OTA_ARM_MAX_SIZE      (256U * 1024U)
#define OTA_DSP_MAX_SIZE      DIRECT_DSP_OTA_MAX_SIZE
#define OTA_BMS_MAX_SIZE      (256U * 1024U)
/* 12KB: ota_remote 的调用链峰值 = mbedtls TLS 读(~4KB 深栈) + 外围帧 ~2KB,
 * 6144 贴线运行——BMS 传输段/ESP 自升级下载写入中的间歇性 PANIC
 * (reset_reason=PANIC, 位置漂移) 与栈溢出特征吻合, 翻倍消除。 */
#define OTA_TASK_STACK_SIZE   12288U
#define OTA_TASK_PRIORITY     5U
/* The fragmented steady-state heap can still start this small coordinator.
 * It releases BLE before allocating the full TLS worker stack. */
#define OTA_COORDINATOR_STACK_SIZE 4096U
#define OTA_COORDINATOR_PRIORITY 4U
#define OTA_READ_IDLE_MS      10U
#define OTA_HEALTH_DELAY_MS   10000U
#define OTA_HEALTH_MAX_AGE_MS 2000U
#define OTA_HTTP_MAX_REDIRECTS 3U
/* 断点续传次数上限: 每次抖动重连一次, 超过即判失败 */
#define OTA_MAX_RESUMES       3U
#define OTA_ALLOC_ATTEMPTS     3U
#define OTA_ALLOC_RETRY_MS     50U
/* Direct completion requires packet ACKs and a fresh application response
 * after EOT. This DSP protocol has no independent version-readback command. */
#define OTA_TARGET_RESPONSE_WAIT_MS 8000U
#define OTA_TARGET_RESPONSE_POLL_MS 200U

static SemaphoreHandle_t g_lock;
static bool g_admin_busy;
static OtaSnapshot g_snapshot;
static OtaManifest g_manifest;
/* 多个适配器(MQTT 上报 + BLE 本地)都要收状态回调, 单槽会被后注册者覆盖:
 * ble_ota_init() 在 mqtt_ota_init() 之后执行, 曾把 MQTT 的 observer 顶掉,
 * 导致 MQTT OTA 全程不发 ota/status(进度/成功都上报不了)。各自按 source 过滤。 */
static OtaStatusObserver g_observers[OTA_MAX_OBSERVERS];
static size_t g_observer_count;
static TaskHandle_t g_remote_task;
static TaskHandle_t g_remote_coordinator;
static bool g_remote_cleanup_busy;
static bool g_remote_reboot_pending;
static atomic_bool g_cancel_requested;
static atomic_uint_fast32_t g_main_loop_tick;
static bool g_active;
/* Reserved OTA session owns this file; UART owner reads it only after staging. */
static FILE *g_direct_stage;
static bool g_boot_validation_required;
static StorageOtaPending g_boot_pending;
/* 传输缓冲按目标选择：BLE→ESP 的分片不超过 256B，其他路径保留 4608B。 */
static wifi_ps_type_t g_saved_wifi_ps = WIFI_PS_NONE;
/* Collector PULL (ARM/DSP) 阶段的失败原因, 由 install_collector_iap 填充并上报云端 */
static char g_arm_failure_detail[96];

static bool create_ota_task_with_retry(TaskFunction_t entry, const char* name,
                                       uint32_t stack_size, void* parameter,
                                       TaskHandle_t* handle)
{
    for (unsigned attempt = 1; attempt <= OTA_ALLOC_ATTEMPTS; ++attempt) {
        if (xTaskCreate(entry, name, stack_size, parameter,
                        OTA_TASK_PRIORITY, handle) == pdPASS) return true;
        if (handle) *handle = NULL;
        ESP_LOGW("OTA", "%s task allocation failed (%u/%u), free=%u largest=%u",
                 name, attempt, OTA_ALLOC_ATTEMPTS,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        if (attempt < OTA_ALLOC_ATTEMPTS)
            vTaskDelay(pdMS_TO_TICKS(OTA_ALLOC_RETRY_MS));
    }
    return false;
}

static void notify_snapshot(const OtaSnapshot* snapshot)
{
    OtaStatusObserver observers[OTA_MAX_OBSERVERS];
    size_t count = 0;
    if (g_lock && xSemaphoreTake(g_lock, portMAX_DELAY) == pdTRUE) {
        count = g_observer_count;
        for (size_t i = 0; i < count; ++i) observers[i] = g_observers[i];
        xSemaphoreGive(g_lock);
    }
    for (size_t i = 0; i < count; ++i) {
        if (observers[i]) observers[i](snapshot);
    }
}

static void update_snapshot_stage(OtaState state, uint8_t progress,
                                  uint32_t received, OtaResult result,
                                  const char* message,
                                  const char* stage, uint8_t stage_progress)
{
    OtaSnapshot copy;
    if (!g_lock || xSemaphoreTake(g_lock, portMAX_DELAY) != pdTRUE) return;
    g_snapshot.state = state;
    g_snapshot.progress = progress > 100 ? 100 : progress;
    g_snapshot.stage_progress = stage_progress > 100 ? 100 : stage_progress;
    strlcpy(g_snapshot.stage, stage ? stage : ota_state_name(state),
            sizeof(g_snapshot.stage));
    g_snapshot.received = received;
    strlcpy(g_snapshot.error_code, ota_result_code(result), sizeof(g_snapshot.error_code));
    strlcpy(g_snapshot.message, message ? message : "", sizeof(g_snapshot.message));
    copy = g_snapshot;
    xSemaphoreGive(g_lock);
    notify_snapshot(&copy);
}

/* 默认阶段名与状态同名; 需要更细阶段的路径(直连 DSP/BMS)调用
 * update_snapshot_stage() 显式给出阶段名与阶段内百分比。 */
static void update_snapshot(OtaState state, uint8_t progress,
                            uint32_t received, OtaResult result,
                            const char* message)
{
    update_snapshot_stage(state, progress, received, result, message,
                          ota_state_name(state), progress);
}

static void release_active(void)
{
    if (!g_lock || xSemaphoreTake(g_lock, portMAX_DELAY) != pdTRUE) return;
    g_active = false;
    xSemaphoreGive(g_lock);
}

const char* ota_target_name(OtaTarget target)
{
    if (target == OTA_TARGET_ARM) return "arm";
    if (target == OTA_TARGET_DSP) return "dsp";
    if (target == OTA_TARGET_BMS) return "bms";
    return "esp";
}

const char* ota_source_name(OtaSource source)
{
    return source == OTA_SOURCE_LOCAL ? "local" : "mqtt";
}

const char* ota_state_name(OtaState state)
{
    static const char* names[] = {
        "idle", "accepted", "downloading", "receiving", "verifying",
        "installing", "cancelling", "cancelled", "rebooting",
        "succeeded", "failed", "rolled_back",
    };
    return state <= OTA_STATE_ROLLED_BACK ? names[state] : "failed";
}

const char* ota_result_code(OtaResult result)
{
    static const char* codes[] = {
        "OK", "DUPLICATE", "INVALID_PARAMS", "INVALID_TARGET", "TLS_REQUIRED",
        "SIGNATURE_REQUIRED", "SIGNATURE_INVALID", "SECURITY_VERSION_READ_FAILED",
        "ROLLBACK_REJECTED", "OTA_BUSY", "FILE_TOO_LARGE", "SIZE_MISMATCH",
        "HASH_MISMATCH", "HTTP_ERROR", "TIMEOUT", "READ_FAILED",
        "FLASH_WRITE_FAILED", "TARGET_TRANSFER_FAILED", "PERSIST_FAILED",
        "OTA_NOT_CANCELLABLE", "TASK_CREATE_FAILED", "INTERNAL_ERROR",
        "TARGET_UNSUPPORTED",
        "INSUFFICIENT_STORAGE",
        "DSP_TRANSFER_FAILED", "BMS_TRANSFER_FAILED", "IMAGE_FORMAT_INVALID",
        "TARGET_NOT_READY", "TARGET_RESPONSE_TIMEOUT", "TARGET_VERSION_UNCONFIRMED",
        "DIAGNOSTIC_ONLY", "TASK_REPLAY_BLOCKED",
    };
    return (unsigned)result < sizeof(codes)/sizeof(codes[0]) ? codes[result] : "INTERNAL_ERROR";
}

bool ota_service_init(void)
{
    if (g_lock) return true;
    g_lock = xSemaphoreCreateMutex();
    if (!g_lock) return false;
    memset(&g_snapshot, 0, sizeof(g_snapshot));
    g_snapshot.state = OTA_STATE_IDLE;
    g_snapshot.target = OTA_TARGET_ESP;
    g_snapshot.source = OTA_SOURCE_MQTT;
    strlcpy(g_snapshot.error_code, "OK", sizeof(g_snapshot.error_code));
    atomic_init(&g_cancel_requested, false);
    atomic_init(&g_main_loop_tick, 0);
    arm_ota_set_cancel_check(NULL);

    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t image_state = ESP_OTA_IMG_UNDEFINED;
    const bool state_available = running &&
        esp_ota_get_state_partition(running, &image_state) == ESP_OK;
    const bool has_pending = storage_get_ota_pending(&g_boot_pending);

    if (has_pending && running &&
        running->address != g_boot_pending.partition_address) {
        strlcpy(g_snapshot.task_id, g_boot_pending.task_id, sizeof(g_snapshot.task_id));
        strlcpy(g_snapshot.version, g_boot_pending.version, sizeof(g_snapshot.version));
        g_snapshot.source = g_boot_pending.source == OTA_SOURCE_LOCAL
            ? OTA_SOURCE_LOCAL : OTA_SOURCE_MQTT;
        g_snapshot.target = OTA_TARGET_ESP;
        g_snapshot.state = OTA_STATE_ROLLED_BACK;
        strlcpy(g_snapshot.error_code, "ROLLED_BACK", sizeof(g_snapshot.error_code));
        strlcpy(g_snapshot.message, "Previous ESP image was rolled back",
                sizeof(g_snapshot.message));
        storage_clear_ota_pending();
    } else if (state_available && image_state == ESP_OTA_IMG_PENDING_VERIFY) {
        g_boot_validation_required = true;
        g_active = true;
        g_snapshot.source = has_pending && g_boot_pending.source == OTA_SOURCE_LOCAL
            ? OTA_SOURCE_LOCAL : OTA_SOURCE_MQTT;
        g_snapshot.target = OTA_TARGET_ESP;
        g_snapshot.state = OTA_STATE_INSTALLING;
        if (has_pending) {
            strlcpy(g_snapshot.task_id, g_boot_pending.task_id, sizeof(g_snapshot.task_id));
            strlcpy(g_snapshot.version, g_boot_pending.version, sizeof(g_snapshot.version));
        } else {
            strlcpy(g_snapshot.task_id, "boot-validation", sizeof(g_snapshot.task_id));
        }
        strlcpy(g_snapshot.message, "Waiting for application health confirmation",
                sizeof(g_snapshot.message));
    } else if (has_pending && state_available && image_state == ESP_OTA_IMG_VALID) {
        strlcpy(g_snapshot.task_id, g_boot_pending.task_id, sizeof(g_snapshot.task_id));
        strlcpy(g_snapshot.version, g_boot_pending.version, sizeof(g_snapshot.version));
        g_snapshot.source = g_boot_pending.source == OTA_SOURCE_LOCAL
            ? OTA_SOURCE_LOCAL : OTA_SOURCE_MQTT;
        g_snapshot.target = OTA_TARGET_ESP;
        /* security_version==0 时 storage_* 返回 false 属"无需持久化", 不是失败。
         * 但仍必须清掉待确认记录 —— 否则这条陈旧记录会在之后每次开机都判一次
         * 失败(并把已成功的升级误报成 PERSIST_FAILED)。 */
        const bool sec_ok = (g_boot_pending.security_version == 0) ||
            storage_set_ota_security_version_for_target(
                "esp", g_boot_pending.security_version);
        if (sec_ok && storage_clear_ota_pending()) {
            g_snapshot.state = OTA_STATE_SUCCEEDED;
            g_snapshot.progress = 100;
            strlcpy(g_snapshot.message, "ESP OTA confirmed", sizeof(g_snapshot.message));
        } else {
            g_snapshot.state = OTA_STATE_FAILED;
            strlcpy(g_snapshot.error_code, "PERSIST_FAILED", sizeof(g_snapshot.error_code));
            strlcpy(g_snapshot.message, "Unable to persist ESP OTA confirmation",
                    sizeof(g_snapshot.message));
        }
    }
    return true;
}

void ota_service_set_observer(OtaStatusObserver observer)
{
    OtaSnapshot copy;
    bool replay = false;
    if (!observer || !g_lock || xSemaphoreTake(g_lock, portMAX_DELAY) != pdTRUE) return;
    bool already = false;
    for (size_t i = 0; i < g_observer_count; ++i) {
        if (g_observers[i] == observer) { already = true; break; }
    }
    if (!already) {
        if (g_observer_count >= OTA_MAX_OBSERVERS) {
            xSemaphoreGive(g_lock);
            return;
        }
        g_observers[g_observer_count++] = observer;
    }
    copy = g_snapshot;
    /* 重放当前状态: 适配器注册得比 OTA 启动晚时(开机即处于 boot-validation),
     * 需要立刻知道进行中的任务, 否则这一次的状态变更会被漏掉。 */
    replay = copy.state != OTA_STATE_IDLE;
    xSemaphoreGive(g_lock);
    if (replay) observer(&copy);
}

void ota_service_note_main_loop_alive(void)
{
    atomic_store(&g_main_loop_tick, (uint32_t)xTaskGetTickCount());
}

static void boot_health_task(void* parameter)
{
    (void)parameter;
    vTaskDelay(pdMS_TO_TICKS(OTA_HEALTH_DELAY_MS));
    const uint32_t now = (uint32_t)xTaskGetTickCount();
    const uint32_t last_alive = (uint32_t)atomic_load(&g_main_loop_tick);
    factory_record_t identity;
    factory_result_t identity_result=factory_identity_read(&identity);
    bool identity_healthy=identity_result==FACTORY_OK;
#if !CONFIG_DIRECT_IDENTITY_PRODUCTION
    identity_healthy=identity_healthy||identity_result==FACTORY_UNPROVISIONED;
#endif
    const uint32_t missing=direct_ota_health_missing(now,last_alive,
        pdMS_TO_TICKS(OTA_HEALTH_MAX_AGE_MS),direct_panel_software_healthy(),
        direct_link_software_healthy(),factory_service_software_healthy(),identity_healthy);
    if (missing) {
        char detail[128];snprintf(detail,sizeof(detail),
            "Startup health failed mask=0x%02lX (main=1 display=2 UART=4 USB=8 identity=16)",(unsigned long)missing);
        update_snapshot(OTA_STATE_FAILED, 0, 0, OTA_RESULT_INTERNAL,
                        detail);
        /* The SDK checks that a previously valid image exists. If unavailable,
         * remain failed and keep USB recovery; never confirm an unhealthy image. */
        esp_err_t rollback=esp_ota_mark_app_invalid_rollback_and_reboot();
        ESP_LOGE("OTA","Startup unhealthy, rollback unavailable: %s",esp_err_to_name(rollback));
        vTaskDelete(NULL);
        return;
    }

    if (esp_ota_mark_app_valid_cancel_rollback() != ESP_OK) {
        update_snapshot(OTA_STATE_FAILED, 0, 0, OTA_RESULT_INTERNAL,
                        "Unable to confirm running ESP image");
        vTaskDelete(NULL);
        return;
    }

    const bool security_saved=g_boot_pending.security_version==0 ||
        storage_set_ota_security_version_for_target("esp",g_boot_pending.security_version);
    if (!security_saved || !storage_clear_ota_pending()) {
        update_snapshot(OTA_STATE_FAILED, 0, 0, OTA_RESULT_PERSIST_FAILED,
                        "Unable to persist ESP OTA confirmation");
        vTaskDelete(NULL);
        return;
    }

    OtaSnapshot snapshot = {0};
    ota_service_get_snapshot(&snapshot);
    update_snapshot(OTA_STATE_SUCCEEDED, 100, snapshot.total,
                    OTA_RESULT_OK, "ESP OTA health confirmed");
    g_boot_validation_required = false;
    release_active();
    vTaskDelete(NULL);
}

bool ota_service_start_boot_health_check(void)
{
    if (!g_boot_validation_required) return true;
    /* 与 ota_remote 同栈: 健康确认要经 update_snapshot → mqtt_publish 上报
     * succeeded, 而 mqtt_publish 在栈上有 2KB 的整包缓冲。原来给 4096 会栈溢出
     * panic 重启 —— 表现为"升级成功但云端永远收不到成功状态", 且重启时刻正好在
     * 开机 +10s (OTA_HEALTH_DELAY_MS)。 */
    return create_ota_task_with_retry(boot_health_task, "ota_health",
                                      OTA_TASK_STACK_SIZE, NULL, NULL);
}

static bool valid_ascii_token(const char* value, size_t max_length)
{
    if (!value) return false;
    const size_t length = strlen(value);
    if (length == 0 || length > max_length) return false;
    for (size_t i = 0; i < length; ++i) {
        const unsigned char c = (unsigned char)value[i];
        if (c < 0x21 || c > 0x7e) return false;
    }
    return true;
}

static bool valid_sha256(const char* sha256)
{
    if (!sha256 || strlen(sha256) != 64) return false;
    for (size_t i = 0; i < 64; ++i) {
        const char c = sha256[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

#define ED25519_SIGNATURE_SIZE 64U
#define ED25519_BASE64_SIZE    88U

static bool valid_target(const char *target)
{
    return target != NULL &&
           (strcmp(target, "esp") == 0 || strcmp(target, "arm") == 0 ||
            strcmp(target, "dsp") == 0 || strcmp(target, "bms") == 0);
}

static bool valid_version(const char *version)
{
    if (version == NULL) {
        return false;
    }

    const size_t length = strlen(version);
    if (length == 0 || length > 31) {
        return false;
    }

    for (size_t i = 0; i < length; ++i) {
        const char c = version[i];
        const bool allowed = (c >= 'A' && c <= 'Z') ||
                             (c >= 'a' && c <= 'z') ||
                             (c >= '0' && c <= '9') ||
                             c == '.' || c == '_' || c == '+' || c == '-';
        if (!allowed) {
            return false;
        }
    }
    return true;
}

static bool valid_base64_signature(const char *signature_base64)
{
    if (signature_base64 == NULL || strlen(signature_base64) != ED25519_BASE64_SIZE ||
        signature_base64[86] != '=' || signature_base64[87] != '=') {
        return false;
    }

    for (size_t i = 0; i < 86; ++i) {
        const char c = signature_base64[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '+' || c == '/')) {
            return false;
        }
    }
    return true;
}

static bool ota_verify_release_signature(const char *target, const char *version,
                                          uint32_t size, const char *sha256,
                                          uint32_t security_version,
                                          const char *signature_base64)
{
    if (!valid_target(target) || !valid_version(version) || size == 0 ||
        !valid_sha256(sha256) || security_version == 0 ||
        !valid_base64_signature(signature_base64)) {
        return false;
    }

    uint8_t signature[ED25519_SIGNATURE_SIZE];
    size_t signature_len = 0;
    const int decode_result = mbedtls_base64_decode(
        signature, sizeof(signature), &signature_len,
        (const unsigned char *)signature_base64, ED25519_BASE64_SIZE);
    if (decode_result != 0 || signature_len != 64) {
        crypto_wipe(signature, sizeof(signature));
        return false;
    }

    char message[256];
    const int message_len = snprintf(
        message, sizeof(message),
        "CS_INV_OTA_V1\ntarget=%s\nversion=%s\nsize=%lu\nsha256=%s\n"
        "security_version=%lu\n",
        target, version, (unsigned long)size, sha256,
        (unsigned long)security_version);
    if (message_len <= 0 || message_len >= (int)sizeof(message)) {
        crypto_wipe(signature, sizeof(signature));
        crypto_wipe(message, sizeof(message));
        return false;
    }

    const bool verified = crypto_ed25519_check(
        signature, OTA_RELEASE_PUBLIC_KEY,
        (const uint8_t *)message, (size_t)message_len) == 0;
    crypto_wipe(signature, sizeof(signature));
    crypto_wipe(message, sizeof(message));
    return verified;
}

static OtaResult validate_manifest_cheap(const OtaManifest* manifest)
{
    if (!manifest || !valid_ascii_token(manifest->task_id, 63) ||
        !valid_ascii_token(manifest->version, 31) ||
        manifest->file_size == 0 ||
        manifest->timeout_seconds < 60 ||
        manifest->timeout_seconds > 3600) {
        return OTA_RESULT_INVALID_PARAMS;
    }
    /* 2026-09-21 sha256 改为可选：提供时必须为 64 位小写十六进制 */
    if (manifest->sha256[0] && !valid_sha256(manifest->sha256)) {
        return OTA_RESULT_INVALID_PARAMS;
    }
    if (manifest->target != OTA_TARGET_ESP && manifest->target != OTA_TARGET_ARM &&
        manifest->target != OTA_TARGET_DSP && manifest->target != OTA_TARGET_BMS) {
        return OTA_RESULT_INVALID_TARGET;
    }
#if CONFIG_DIRECT_IDENTITY_PRODUCTION
    factory_record_t identity;
    if(factory_identity_read(&identity)!=FACTORY_OK ||
       !direct_ota_compatible(true,manifest->model,manifest->hardware,identity.model,identity.hw_controller))return OTA_RESULT_INVALID_TARGET;
    if(!manifest->sha256[0])return OTA_RESULT_HASH_MISMATCH;
#endif
    /* DSP/BMS use their own UART owners; the unpopulated ARM is not a target. */
    if (manifest->target == OTA_TARGET_ARM) {
        return OTA_RESULT_INVALID_TARGET;
    }
    /* DSP and BMS are dispatched to independent direct UART owners. */
    if (manifest->target == OTA_TARGET_BMS && manifest->file_size > OTA_BMS_MAX_SIZE) {
        return OTA_RESULT_FILE_TOO_LARGE;
    }
    /* 本地开发时允许 HTTP + 无签名（生产环境应强制 HTTPS + 签名） */
    if (manifest->source == OTA_SOURCE_MQTT && manifest->url[0]) {
        if (strncmp(manifest->url, "https://", 8) != 0 &&
            strncmp(manifest->url, "http://", 7) != 0) {
            return OTA_RESULT_TLS_REQUIRED;
        }
    }
    /* 签名为空时跳过校验（本地测试）；非空时需验证 */
    /* if (!manifest->signature[0]) return OTA_RESULT_SIGNATURE_REQUIRED; */

    if (manifest->target == OTA_TARGET_ARM && manifest->file_size > OTA_ARM_MAX_SIZE) {
        return OTA_RESULT_FILE_TOO_LARGE;
    }
    if (manifest->target == OTA_TARGET_DSP && manifest->file_size > OTA_DSP_MAX_SIZE) {
        return OTA_RESULT_FILE_TOO_LARGE;
    }
    if (manifest->target == OTA_TARGET_ESP) {
        const esp_partition_t* partition = esp_ota_get_next_update_partition(NULL);
        if (!partition || manifest->file_size > partition->size) return OTA_RESULT_FILE_TOO_LARGE;
    }
    return OTA_RESULT_OK;
}

static OtaResult check_available(const OtaManifest* manifest)
{
    if (!g_lock || xSemaphoreTake(g_lock, portMAX_DELAY) != pdTRUE) {
        return OTA_RESULT_INTERNAL;
    }
    OtaResult result = OTA_RESULT_OK;
    if(g_admin_busy){xSemaphoreGive(g_lock);return OTA_RESULT_BUSY;}
    if (g_active || g_remote_cleanup_busy) {
        result = strcmp(g_snapshot.task_id, manifest->task_id) == 0
            ? OTA_RESULT_DUPLICATE : OTA_RESULT_BUSY;
    }
    xSemaphoreGive(g_lock);
    return result;
}

static OtaResult authenticate_manifest(const OtaManifest* manifest)
{
    /* 2026-09-21 产品决策：取消强制签名与强制安全版本（本地/云端一致）——
     * 服务端固件记录可能未签名。签名非空时仍执行 Ed25519 验证；
     * security_version>0 时仍执行防回滚检查；缺省时按未签名固件放行。 */
    if (manifest->signature[0] &&
        !ota_verify_release_signature(
            ota_target_name(manifest->target), manifest->version,
            manifest->file_size, manifest->sha256,
            manifest->security_version, manifest->signature)) {
        return OTA_RESULT_SIGNATURE_INVALID;
    }

    if (manifest->security_version > 0) {
        uint32_t installed = 0;
        if (!storage_get_ota_security_version_for_target(
                ota_target_name(manifest->target), &installed)) {
            return OTA_RESULT_SECURITY_READ_FAILED;
        }
        if (manifest->security_version <= installed) {
            return OTA_RESULT_ROLLBACK_REJECTED;
        }
    }

    return OTA_RESULT_OK;
}

static void disable_wifi_ps(void)
{
    wifi_ps_type_t current_ps = WIFI_PS_NONE;
    if (esp_wifi_get_ps(&current_ps) == ESP_OK && current_ps != WIFI_PS_NONE) {
        g_saved_wifi_ps = current_ps;
        esp_wifi_set_ps(WIFI_PS_NONE);
        ESP_LOGI("OTA", "WiFi PS disabled for OTA (was %d)", current_ps);
    }
}

static void restore_wifi_ps(void)
{
    if (g_saved_wifi_ps != WIFI_PS_NONE) {
        esp_wifi_set_ps(g_saved_wifi_ps);
        ESP_LOGI("OTA", "WiFi PS restored to %d", g_saved_wifi_ps);
        g_saved_wifi_ps = WIFI_PS_NONE;
    }
}

static OtaResult reserve_manifest(const OtaManifest* manifest)
{
    if (!g_lock || xSemaphoreTake(g_lock, portMAX_DELAY) != pdTRUE) {
        return OTA_RESULT_INTERNAL;
    }
    if(g_admin_busy){xSemaphoreGive(g_lock);return OTA_RESULT_BUSY;}
    if (g_active || g_remote_cleanup_busy) {
        const bool duplicate = strcmp(g_snapshot.task_id, manifest->task_id) == 0;
        xSemaphoreGive(g_lock);
        return duplicate ? OTA_RESULT_DUPLICATE : OTA_RESULT_BUSY;
    }

    if(manifest->target==OTA_TARGET_DSP||manifest->target==OTA_TARGET_BMS){
        bool duplicate=false;
        if(!storage_claim_direct_ota_task(ota_target_name(manifest->target),manifest->task_id,&duplicate)){
            xSemaphoreGive(g_lock);return OTA_RESULT_PERSIST_FAILED;
        }
        if(duplicate){xSemaphoreGive(g_lock);return OTA_RESULT_TASK_REPLAY_BLOCKED;}
    }
    g_active = true;
    g_manifest = *manifest;
    memset(&g_snapshot, 0, sizeof(g_snapshot));
    strlcpy(g_snapshot.task_id, manifest->task_id, sizeof(g_snapshot.task_id));
    strlcpy(g_snapshot.version, manifest->version, sizeof(g_snapshot.version));
    g_snapshot.source = manifest->source;
    g_snapshot.target = manifest->target;
    g_snapshot.state = OTA_STATE_ACCEPTED;
    g_snapshot.total = manifest->file_size;
    g_snapshot.progress = 0;
    g_snapshot.stage_progress = 0;
    strlcpy(g_snapshot.stage, "accepted", sizeof(g_snapshot.stage));
    strlcpy(g_snapshot.error_code, "OK", sizeof(g_snapshot.error_code));
    atomic_store(&g_cancel_requested, false);
    OtaSnapshot copy = g_snapshot;
    xSemaphoreGive(g_lock);
    disable_wifi_ps();
    notify_snapshot(&copy);
    return OTA_RESULT_OK;
}

static void finish_failure(OtaResult result, const char* message)
{
    const OtaState state = result == OTA_RESULT_OK ? OTA_STATE_CANCELLED : OTA_STATE_FAILED;
    OtaSnapshot snapshot = {0};
    ota_service_get_snapshot(&snapshot);
    update_snapshot(state, state == OTA_STATE_CANCELLED ? snapshot.progress : 0,
                    snapshot.received, result, message);
    restore_wifi_ps();
    release_active();
}

static bool cancel_requested(void)
{
    return atomic_load(&g_cancel_requested);
}

static void hash_to_hex(const uint8_t hash[32], char hex[65])
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < 32; ++i) {
        hex[i * 2] = digits[hash[i] >> 4];
        hex[i * 2 + 1] = digits[hash[i] & 0x0f];
    }
    hex[64] = '\0';
}

/* 断点续传钩子: 远端下载(https/http)在读流中断后, 重新建连并请求 Range:
 * bytes=<already_received>- 继续。设备侧 WiFi/BLE 共存下长下载被打断是常态
 * (2026-09-22 实测: 下到 410112/1270688 时 READ_FAILED, 期间 MQTT 断连重连),
 * 一次抖动就废掉整包升级不划算。本地(BLE)通道传 NULL, 行为不变。 */
typedef bool (*OtaResumeFn)(void* context, uint32_t already_received);

static OtaResult read_and_stage_buffered(const OtaManifest* manifest,
                                         OtaStreamReader reader, void* context,
                                         esp_ota_handle_t* esp_handle,
                                         OtaResumeFn resume, unsigned max_resumes,
                                         uint8_t* transfer_buffer,
                                         size_t transfer_capacity)
{
    uint32_t received = 0;
    uint8_t last_progress = 0;
    int64_t last_idle_trace_ms = 0;
    const int64_t started_ms = esp_timer_get_time() / 1000;
    int64_t last_data_ms = started_ms;
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    if (mbedtls_sha256_starts(&sha, 0) != 0) {
        mbedtls_sha256_free(&sha);
        return OTA_RESULT_INTERNAL;
    }

    while (received < manifest->file_size) {
        if (cancel_requested()) {
            mbedtls_sha256_free(&sha);
            return OTA_RESULT_OK;
        }
        if ((esp_timer_get_time() / 1000) - started_ms >
            (int64_t)manifest->timeout_seconds * 1000) {
            mbedtls_sha256_free(&sha);
            return OTA_RESULT_TIMEOUT;
        }

        const size_t remaining = manifest->file_size - received;
        const size_t capacity = remaining < transfer_capacity
            ? remaining : transfer_capacity;
        const int read_count = reader(context, transfer_buffer, capacity);
        if (read_count < 0 || (size_t)read_count > capacity) {
            if (read_count < 0 && resume && max_resumes > 0 &&
                !cancel_requested()) {
                /* 读流中断 → 续传一次(最多 max_resumes 次), 从 received 继续 */
                char line[96];
                snprintf(line, sizeof(line), "read failed at %u/%u, resuming",
                         (unsigned)received, (unsigned)manifest->file_size);
                mqtt_publish_ota_trace("http-resume", line);
                ESP_LOGW("OTA", "%s", line);
                if (resume(context, received)) {
                    --max_resumes;
                    last_idle_trace_ms = 0;
                    last_data_ms = esp_timer_get_time() / 1000;
                    continue;
                }
            }
            mbedtls_sha256_free(&sha);
            return OTA_RESULT_READ_FAILED;
        }
        if (read_count == 0) {
            /* chunked/无长度响应: 数据完整到达时提前收尾; 总长与 SHA-256
             * 在循环外兜底 (本地 OTA 的 context 不是 HTTP 客户端, 跳过) */
            if (manifest->source == OTA_SOURCE_MQTT &&
                esp_http_client_is_complete_data_received(
                    (esp_http_client_handle_t)context)) {
                break;
            }
            /* 仅真实连续 5s 无数据才记 idle；本地 BLE 与 HTTP 分开标记。
             * 过去用 started_ms 报告的 "no data 569s" 其实是总耗时，
             * 即使当前仍在收数据也会不断误报 http-idle。 */
            const int64_t idle_now_ms = esp_timer_get_time() / 1000;
            if (idle_now_ms - last_data_ms >= 5000 &&
                idle_now_ms - last_idle_trace_ms >= 5000) {
                last_idle_trace_ms = idle_now_ms;
                char line[80];
                snprintf(line, sizeof(line), "no data %ds, at %u/%u (%s)",
                         (int)((idle_now_ms - last_data_ms) / 1000),
                         (unsigned)received, (unsigned)manifest->file_size,
                         manifest->source == OTA_SOURCE_LOCAL ? "local BLE" : "HTTP upstream");
                mqtt_publish_ota_trace(manifest->source == OTA_SOURCE_LOCAL
                                           ? "ble-idle" : "http-idle", line);
            }
            vTaskDelay(pdMS_TO_TICKS(OTA_READ_IDLE_MS));
            continue;
        }

        if (mbedtls_sha256_update(&sha, transfer_buffer, (size_t)read_count) != 0) {
            mbedtls_sha256_free(&sha);
            return OTA_RESULT_INTERNAL;
        }
        if (manifest->target == OTA_TARGET_ESP) {
            if (esp_ota_write(*esp_handle, transfer_buffer, (size_t)read_count) != ESP_OK) {
                mbedtls_sha256_free(&sha);
                return OTA_RESULT_FLASH_WRITE_FAILED;
            }
        } else if (!g_direct_stage ||
                   fwrite(transfer_buffer, 1, (size_t)read_count, g_direct_stage) != (size_t)read_count) {
            mbedtls_sha256_free(&sha);
            return OTA_RESULT_FLASH_WRITE_FAILED;
        }

        received += (uint32_t)read_count;
        last_data_ms = esp_timer_get_time() / 1000;
        /* 进度上报: 每 1% 一条 (用户指定取消节流)。8KB 缓冲下 1.27MB 自升级
         * 仅 ~156 条 (1KB 缓冲时代是 624 条), 风暴风险已大幅降低。 */
        const uint8_t progress = (uint8_t)(((uint64_t)received * 100U) / manifest->file_size);
        if (progress != last_progress) {
            last_progress = progress;
            update_snapshot_stage(manifest->source == OTA_SOURCE_MQTT ? OTA_STATE_DOWNLOADING : OTA_STATE_RECEIVING,
                                  progress, received, OTA_RESULT_OK, "Receiving firmware",
                                  "downloading", progress);
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    uint8_t digest[32];
    char digest_hex[65];
    /* 提前收到 EOF (chunked/连接截断) 时以实际字节数兜底, 报尺寸错误而非哈希错 */
    if (received != manifest->file_size) {
        mbedtls_sha256_free(&sha);
        return OTA_RESULT_SIZE_MISMATCH;
    }
    const int hash_result = mbedtls_sha256_finish(&sha, digest);
    mbedtls_sha256_free(&sha);
    if (hash_result != 0) return OTA_RESULT_INTERNAL;
    hash_to_hex(digest, digest_hex);
    /* manifest 未提供 sha256 时跳过比对（此时仅依赖传输层完整性） */
    if (manifest->sha256[0] &&
        strcmp(digest_hex, manifest->sha256) != 0) return OTA_RESULT_HASH_MISMATCH;
    return OTA_RESULT_OK;
}

static OtaResult read_and_stage(const OtaManifest* manifest,
                                OtaStreamReader reader, void* context,
                                esp_ota_handle_t* esp_handle,
                                OtaResumeFn resume, unsigned max_resumes)
{
    const size_t transfer_capacity = ota_transfer_policy(manifest).buffer_size;
    uint8_t* transfer_buffer = NULL;
    for (unsigned attempt = 1; attempt <= OTA_ALLOC_ATTEMPTS; ++attempt) {
        transfer_buffer = malloc(transfer_capacity);
        if (transfer_buffer) break;
        ESP_LOGW("OTA", "transfer buffer allocation failed (%u/%u), size=%u free=%u largest=%u",
                 attempt, OTA_ALLOC_ATTEMPTS, (unsigned)transfer_capacity,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        if (attempt < OTA_ALLOC_ATTEMPTS)
            vTaskDelay(pdMS_TO_TICKS(OTA_ALLOC_RETRY_MS));
    }
    if (!transfer_buffer) return OTA_RESULT_INTERNAL;

    const OtaResult result = read_and_stage_buffered(
        manifest, reader, context, esp_handle, resume, max_resumes,
        transfer_buffer, transfer_capacity);
    free(transfer_buffer);
    return result;
}

static OtaResult install_esp(const OtaManifest* manifest,
                             OtaStreamReader reader, void* context,
                             OtaResumeFn resume, unsigned max_resumes)
{
    /* 远程 ESP 的静默窗口已在 HTTP/TLS 建连前开启；本地 BLE 没有 remote_worker，
     * 在这里自行管理，避免重复 begin/end。 */
    const bool owns_quiet_window = manifest->source != OTA_SOURCE_MQTT;
    if (owns_quiet_window) ota_quiet_begin();
    const esp_partition_t* update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition || manifest->file_size > update_partition->size) {
        if (owns_quiet_window) ota_quiet_end();
        return OTA_RESULT_FILE_TOO_LARGE;
    }
    {
        char line[80];
        snprintf(line, sizeof(line), "ESP 写入开始 size=%u part=%s",
                 (unsigned)manifest->file_size,
                 update_partition ? update_partition->label : "?");
        if (manifest->source == OTA_SOURCE_MQTT)
            mqtt_publish_ota_trace("esp-install", line);
        else
            ESP_LOGI("OTA", "%s", line);
    }

    esp_ota_handle_t handle = 0;
    const size_t erase_hint = ota_transfer_policy(manifest).sequential_erase
        ? OTA_WITH_SEQUENTIAL_WRITES : manifest->file_size;
    if (esp_ota_begin(update_partition, erase_hint, &handle) != ESP_OK) {
        if (owns_quiet_window) ota_quiet_end();
        return OTA_RESULT_FLASH_WRITE_FAILED;
    }

    OtaResult result = read_and_stage(manifest, reader, context, &handle,
                                      resume, max_resumes);
    if (result != OTA_RESULT_OK || cancel_requested()) {
        esp_ota_abort(handle);
        if (owns_quiet_window) ota_quiet_end();
        return cancel_requested() ? OTA_RESULT_OK : result;
    }

    if (owns_quiet_window) ota_quiet_end();
    update_snapshot(OTA_STATE_VERIFYING, 100, manifest->file_size,
                    OTA_RESULT_OK, "Verifying ESP image");
    if (esp_ota_end(handle) != ESP_OK) return OTA_RESULT_FLASH_WRITE_FAILED;
    uint8_t metadata[DIRECT_FW_METADATA_SIZE];
    if(esp_partition_read(update_partition,DIRECT_FW_METADATA_OFFSET,metadata,sizeof(metadata))!=ESP_OK ||
       !direct_ota_image_identity_matches(metadata,sizeof(metadata),direct_firmware_metadata()[6])){
        ESP_LOGE("OTA","ESP image product/profile mismatch; boot partition unchanged");
        return OTA_RESULT_INVALID_TARGET;
    }

    StorageOtaPending pending = {
        .security_version = manifest->security_version,
        .partition_address = update_partition->address,
        .source = (uint8_t)manifest->source,
    };
    strlcpy(pending.task_id, manifest->task_id, sizeof(pending.task_id));
    strlcpy(pending.version, manifest->version, sizeof(pending.version));
    if (!storage_set_ota_pending(&pending)) return OTA_RESULT_PERSIST_FAILED;

    if (esp_ota_set_boot_partition(update_partition) != ESP_OK) {
        storage_clear_ota_pending();
        return OTA_RESULT_FLASH_WRITE_FAILED;
    }
    update_snapshot(OTA_STATE_REBOOTING, 100, manifest->file_size,
                    OTA_RESULT_OK, "ESP update ready; rebooting");
    return OTA_RESULT_OK;
}

/* DSP uses the F280039C packed App verbatim. BMS retains its raw format. */
static bool direct_stage_read(void *ctx,size_t offset,uint8_t *out,size_t size)
{
    FILE *file=ctx;
    return fseek(file,(long)offset,SEEK_SET)==0 && fread(out,1,size,file)==size;
}
static bool direct_stage_cancel(void *ctx)
{
    (void)ctx;return cancel_requested();
}
static void direct_stage_progress(void *ctx,size_t done,size_t total)
{
    (void)ctx;
    const uint8_t percent=total?(uint8_t)((uint64_t)done*100U/total):0;
    update_snapshot_stage(OTA_STATE_INSTALLING,percent,
                    g_manifest.file_size,OTA_RESULT_OK,"Writing target Boot image",
                    "transferring",percent);
}

/* A fresh application response is liveness evidence, not image/version proof. */
static bool target_response_seen(bool dsp,uint32_t since_ms)
{
    if(dsp){
        RunParamDef measurement;uint32_t received_ms=0;
        return direct_dsp_get_measurement(&measurement,&received_ms)&&
               (int32_t)(received_ms-since_ms)>=0;
    }
    direct_bms_measurement_t measurement;uint32_t age_ms=0;
    return direct_bms_get_snapshot(&measurement,&age_ms)&&
           age_ms<=OTA_TARGET_RESPONSE_POLL_MS*2U;
}

/* Waiting time is not a measured fraction of target verification. */
static bool wait_target_response(bool dsp,uint32_t since_ms)
{
    const int64_t start=esp_timer_get_time()/1000;
    /* 先宣告"校验中": 目标立刻回来时也要让 App/云端看到这一格, 而不是直接跳到成功。 */
    update_snapshot_stage(OTA_STATE_VERIFYING,0,g_manifest.file_size,OTA_RESULT_OK,
                          "Waiting for target application response","verifying",0);
    for(;;){
        if(cancel_requested())return false;
        if(target_response_seen(dsp,since_ms))return true;
        const uint32_t elapsed=(uint32_t)((esp_timer_get_time()/1000)-start);
        if(elapsed>=OTA_TARGET_RESPONSE_WAIT_MS)return false;
        vTaskDelay(pdMS_TO_TICKS(OTA_TARGET_RESPONSE_POLL_MS));
    }
}
static OtaResult install_direct_iap(const OtaManifest* manifest,
                                  OtaStreamReader reader,void* context,
                                  bool* source_closed)
{
    const char *path="/cache/direct_target_ota.bin";
    OtaResult result=OTA_RESULT_INSUFFICIENT_STORAGE;
    bool quiet=false;
    g_arm_failure_detail[0]=0;
    /* Cache is mounted by app startup. Never format it on an upgrade error. */
    remove(path); /* Fixed staging path; stale downloads are never resumed. */
    if(!offline_cache_reserve_space(manifest->file_size+16384U))goto done;
    g_direct_stage=fopen(path,"wb+");
    if(!g_direct_stage)goto done;
    esp_ota_handle_t unused=0;
    result=read_and_stage(manifest,reader,context,&unused,NULL,0);
    if(manifest->source==OTA_SOURCE_MQTT && context){
        esp_http_client_close((esp_http_client_handle_t)context);
        if(source_closed)*source_closed=true;
    }
    if(result!=OTA_RESULT_OK || cancel_requested())goto done;
    if(fflush(g_direct_stage)!=0){result=OTA_RESULT_FLASH_WRITE_FAILED;goto done;}
    ESP_LOGW("OTA","direct task=%s target=%s: one accepted command, one transfer session",
             manifest->task_id,ota_target_name(manifest->target));
    /* Stop polling/configuration writes while the same UART owner enters Boot.
     * The owner finishes its current normal transaction before taking this job. */
    ota_quiet_begin();quiet=true;
    direct_iap_image_t image={
        .target=manifest->target==OTA_TARGET_DSP?DIRECT_IAP_DSP:DIRECT_IAP_BMS,
        .size=manifest->file_size
    };
    snprintf(image.version,sizeof(image.version),"%s",manifest->version);
    direct_iap_io_t io={.read_image=direct_stage_read,.cancelled=direct_stage_cancel,
                       .progress=direct_stage_progress,.context=g_direct_stage};
#if CONFIG_DIRECT_DSP_OTA_PACKET0_PROBE
    io.probe_packet0=manifest->target==OTA_TARGET_DSP;
#endif
    if(io.probe_packet0){
        ESP_LOGW("OTA","PACKET0 DIAGNOSTIC ONLY: erase DSP; send 02 01 FE once; capture 2000ms; no EOT or upgrade success");
    }
    if(manifest->target==OTA_TARGET_DSP){
        uint8_t header[6]={0};
        if(direct_stage_read(g_direct_stage,0,header,sizeof(header))){
            char line[96];
            snprintf(line,sizeof(line),"DSP image size=%lu head=%02X%02X %02X%02X %02X%02X",
                (unsigned long)manifest->file_size,header[0],header[1],header[2],header[3],header[4],header[5]);
            ESP_LOGW("OTA","%s",line);
            if(manifest->source==OTA_SOURCE_MQTT)mqtt_publish_ota_trace("dsp-image",line);
        }
    }
    /* 传输阶段包含进入 Boot 的全部等待(升级请求/HR/擦除/CR)与逐包写入。进度条
     * 在收到 HR 之前停在 0%, message 会说清卡在哪一步。 */
    update_snapshot_stage(OTA_STATE_INSTALLING,0,manifest->file_size,
                          OTA_RESULT_OK,"Entering direct target Boot (waiting for HR)",
                          "transferring",0);
    direct_iap_result_t wire=direct_link_run_iap(&image,&io);
    if(wire!=DIRECT_IAP_OK){
        snprintf(g_arm_failure_detail,sizeof(g_arm_failure_detail),
                 "%s direct OTA: %s (%d)",ota_target_name(manifest->target),
                 direct_iap_result_name(wire),(int)wire);
        if(wire==DIRECT_IAP_DIAGNOSTIC_COMPLETE)result=OTA_RESULT_DIAGNOSTIC_ONLY;
        else if(wire==DIRECT_IAP_CANCELLED)result=OTA_RESULT_OK;
        else if(wire==DIRECT_IAP_BUSY)result=OTA_RESULT_BUSY;
        else if(wire==DIRECT_IAP_NOT_READY)result=OTA_RESULT_TARGET_NOT_READY;
        else if(wire==DIRECT_IAP_IMAGE_SIZE_INVALID || wire==DIRECT_IAP_IMAGE_OPCODE_INVALID ||
                wire==DIRECT_IAP_IMAGE_MAGIC_INVALID)result=OTA_RESULT_IMAGE_FORMAT_INVALID;
        else result=manifest->target==OTA_TARGET_DSP?OTA_RESULT_DSP_TRANSFER_FAILED:OTA_RESULT_BMS_TRANSFER_FAILED;
        goto done;
    }
    /* 先恢复轮询, 否则新 App 起来了也没人问它 03/04。 */
    if(quiet){ota_quiet_end();quiet=false;}
    const uint32_t transfer_done_ms=(uint32_t)(esp_timer_get_time()/1000);
    const bool responded=wait_target_response(manifest->target==OTA_TARGET_DSP,
                                              transfer_done_ms);
    if(cancel_requested()){result=OTA_RESULT_OK;goto done;}
    if(!responded){
        snprintf(g_arm_failure_detail,sizeof(g_arm_failure_detail),
                 "%s: transfer ACKs received; no application response; upgrade unconfirmed",
                 ota_target_name(manifest->target));
        result=OTA_RESULT_TARGET_RESPONSE_TIMEOUT;goto done;
    }
    if(manifest->security_version!=0 && !storage_set_ota_security_version_for_target(
        ota_target_name(manifest->target),manifest->security_version)){
        result=OTA_RESULT_PERSIST_FAILED;goto done;
    }
    char done_message[96];
    snprintf(done_message,sizeof(done_message),"Target transfer complete; application responded");
    if(manifest->source==OTA_SOURCE_MQTT)mqtt_publish_ota_trace("target-done",done_message);
    update_snapshot_stage(OTA_STATE_SUCCEEDED,100,manifest->file_size,OTA_RESULT_OK,
                    done_message,"succeeded",100);
    result=OTA_RESULT_OK;
done:
    if(g_direct_stage){fclose(g_direct_stage);g_direct_stage=NULL;}
    remove(path);
    if(quiet)ota_quiet_end();
    if(result==OTA_RESULT_OK && !cancel_requested()){
        restore_wifi_ps();release_active();
    }
    return result;
}

static OtaResult install_stream(const OtaManifest* manifest,
                                OtaStreamReader reader, void* context,
                                OtaResumeFn resume, unsigned max_resumes,
                                bool* source_closed)
{
    if (!reader) return OTA_RESULT_INVALID_PARAMS;
    return manifest->target == OTA_TARGET_ESP
        ? install_esp(manifest, reader, context, resume, max_resumes)
        : install_direct_iap(manifest, reader, context, source_closed);
}

static int http_stream_read(void* context, uint8_t* buffer, size_t capacity)
{
    return esp_http_client_read((esp_http_client_handle_t)context,
                                (char*)buffer, (int)capacity);
}

/* The reserved remote OTA session owns this state. Capture response headers;
 * esp_http_client_get_header() only returns outgoing request headers. */
typedef struct {
    bool collecting;
    bool seen;
    bool invalid;
    uint32_t start;
    uint32_t end;
    uint32_t total;
} OtaHttpRange;
static OtaHttpRange g_http_range;

static bool http_range_number(const char **cursor, uint32_t *value)
{
    const char *p = *cursor;
    uint32_t number = 0;
    if (*p < '0' || *p > '9') return false;
    do {
        const uint32_t digit = (uint32_t)(*p - '0');
        if (number > (UINT32_MAX - digit) / 10U) return false;
        number = number * 10U + digit;
        ++p;
    } while (*p >= '0' && *p <= '9');
    *cursor = p;
    *value = number;
    return true;
}

static bool http_range_value(const char *value, OtaHttpRange *range)
{
    if (!value || strncmp(value, "bytes ", 6) != 0) return false;
    const char *p = value + 6;
    if (!http_range_number(&p, &range->start) || *p++ != '-' ||
        !http_range_number(&p, &range->end) || *p++ != '/' ||
        !http_range_number(&p, &range->total)) return false;
    while (*p == ' ' || *p == '\t') ++p;
    return *p == '\0' && range->start <= range->end &&
           range->end < range->total;
}

static esp_err_t ota_http_event_handler(esp_http_client_event_t *event)
{
    if (!g_http_range.collecting || event->event_id != HTTP_EVENT_ON_HEADER ||
        !event->header_key) return ESP_OK;
    const char *name = "content-range";
    for (size_t i = 0; ; ++i) {
        char c = event->header_key[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c + ('a' - 'A'));
        if (c != name[i]) return ESP_OK;
        if (c == '\0') break;
    }
    if (g_http_range.seen || !http_range_value(event->header_value, &g_http_range))
        g_http_range.invalid = true;
    g_http_range.seen = true;
    return ESP_OK;
}

/* 断点续传: 复用同一个 client, 带 Range 头重连, 只接受"从 received 起的剩余量"
 * (206 + Content-Length/Content-Range 精确匹配), 其余情况一律放弃。
 * 返回 true 表示已就绪, 可继续 read。 */
static bool http_stream_resume(void* context, uint32_t already_received)
{
    if (already_received >= g_manifest.file_size) return false;
    esp_http_client_handle_t client = (esp_http_client_handle_t)context;
    char range[40];
    char line[96];
    snprintf(range, sizeof(range), "bytes=%lu-", (unsigned long)already_received);
    esp_http_client_close(client);
    memset(&g_http_range, 0, sizeof(g_http_range));
    if (esp_http_client_set_header(client, "Range", range) != ESP_OK) return false;
    g_http_range.collecting = true;
    if (esp_http_client_open(client, 0) != ESP_OK) {
        g_http_range.collecting = false;
        ESP_LOGW("OTA", "resume: open failed");
        return false;
    }
    const int64_t len = esp_http_client_fetch_headers(client);
    g_http_range.collecting = false;
    const int status = esp_http_client_get_status_code(client);
    const uint32_t expect = g_manifest.file_size - already_received;
    if (status != 206 || len < 0 || (uint64_t)len != expect ||
        !g_http_range.seen || g_http_range.invalid ||
        g_http_range.start != already_received ||
        g_http_range.end != g_manifest.file_size - 1U ||
        g_http_range.total != g_manifest.file_size) {
        snprintf(line, sizeof(line), "resume refused: status=%d len=%lld expect=%lu",
                 status, (long long)len, (unsigned long)expect);
        ESP_LOGW("OTA", "%s", line);
        mqtt_publish_ota_trace("http-resume", line);
        esp_http_client_close(client);
        return false;
    }
    snprintf(line, sizeof(line), "resumed at %lu (206 len=%lld)",
             (unsigned long)already_received, (long long)len);
    ESP_LOGW("OTA", "%s", line);
    return true;
}

static OtaResult prepare_http_download(esp_http_client_handle_t client,
                                       uint32_t expected_size,
                                       int* status_out,
                                       int64_t* content_length_out,
                                       char* detail, size_t detail_size)
{
    for (unsigned redirect_count = 0;
         redirect_count <= OTA_HTTP_MAX_REDIRECTS; ++redirect_count) {
        const esp_err_t open_result = esp_http_client_open(client, 0);
        if (open_result != ESP_OK) {
            snprintf(detail, detail_size, "HTTP open failed: %s",
                     esp_err_to_name(open_result));
            ESP_LOGE("OTA", "%s", detail);
            return OTA_RESULT_HTTP_ERROR;
        }

        const int64_t content_length = esp_http_client_fetch_headers(client);
        const int status = esp_http_client_get_status_code(client);
        const bool chunked = esp_http_client_is_chunked_response(client);
        *status_out = status;
        *content_length_out = content_length;
        ESP_LOGI("OTA", "HTTP response status=%d content_length=%lld chunked=%d redirect=%u",
                 status, (long long)content_length, chunked, redirect_count);

        if (status == 301 || status == 302 || status == 303 ||
            status == 307 || status == 308) {
            if (redirect_count == OTA_HTTP_MAX_REDIRECTS ||
                esp_http_client_set_redirection(client) != ESP_OK) {
                snprintf(detail, detail_size,
                         "HTTP redirect failed: status=%d", status);
                return OTA_RESULT_HTTP_ERROR;
            }
            esp_http_client_close(client);
            continue;
        }

        if (status < 200 || status >= 300) {
            snprintf(detail, detail_size, "HTTP status=%d len=%lld",
                     status, (long long)content_length);
            return OTA_RESULT_HTTP_ERROR;
        }
        /* chunked/无 Content-Length = CDN 冷缓存回源流: 允许下载 (用户要求
         * 无缓存也能升级), 由 read_and_stage 的 300s 超时 + SHA-256 + 总长
         * 核对兜底。回源慢时 read idle 每 5s 发一条 http-idle trace 可见。 */
        if (!chunked && (uint64_t)content_length != expected_size) {
            snprintf(detail, detail_size,
                     "SIZE_MISMATCH status=%d len=%lld expected=%lu",
                     status, (long long)content_length, (unsigned long)expected_size);
            return OTA_RESULT_SIZE_MISMATCH;
        }
        return OTA_RESULT_OK;
    }

    snprintf(detail, detail_size, "HTTP redirect limit exceeded");
    return OTA_RESULT_HTTP_ERROR;
}

/* 把堆余量追加到 detail 尾部: esp_http_client_init 要连做 11 次 calloc 再
 * malloc(接收缓冲), 任一步失败就返回 NULL(串口分别打 "Error allocate memory" /
 * "Allocation failed"), 但 OTA 失败报文原本只有一句 "HTTP client init failed",
 * 从云端看不出是不是内存问题。带上 free/largest 让下一次失败自带证据。 */
/* OTA 静默窗口 (2026-09-22, 用户授权): ESP 自升级期间停掉 ARM 轮询、周期心跳与
 * 离线补传 —— 下载/写入是内存与 CPU 最紧的一段。online(带堆与进度曲线)与
 * ota/status 保持不动, 进度必须实时可见。
 * 只在 ESP 目标开: ARM/DSP/BMS 升级的传输通道正是那条 UART, 停了就没法升级。 */
static void ota_quiet_begin(void)
{
    arm_poll_set_quiet(true);
    telemetry_set_quiet(true);
    offline_cache_set_quiet(true);
    ESP_LOGI("OTA", "quiet window ON (arm poll / heartbeat / cache flush paused)");
}

static void ota_quiet_end(void)
{
    arm_poll_set_quiet(false);
    telemetry_set_quiet(false);
    offline_cache_set_quiet(false);
}

static void detail_append_heap(char *detail, size_t detail_size)
{
    const size_t used = strlen(detail);
    if (used + 1 >= detail_size) return;
    snprintf(detail + used, detail_size - used, " free=%u max=%u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

/* Keep HTTP RX small even when a large block is currently available: that
 * block belongs to TLS. Fallback to 512B only for HTTP allocation failure. */
static esp_http_client_handle_t http_client_init_with_fallback(
        const OtaManifest *manifest, int *buffer_size_out)
{
    static const int sizes[] = { (int)OTA_BUFFER_SIZE, 512 };
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        esp_http_client_config_t config = {
            .url = manifest->url,
            .timeout_ms = (int)manifest->timeout_seconds * 1000,
            /* 自有固定服务器: 不配置 CA, 配合 CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY 跳过证书验证 (固件有 Ed25519 签名兜底) */
            .keep_alive_enable = true,
            .event_handler = ota_http_event_handler,
            .buffer_size = sizes[i],
            /* Allocate/retain full TLS RX immediately after handshake, before
             * staging/flash allocations can fragment the freed record block.
             * Global dynamic buffers remain enabled for MQTT and BLE coexistence. */
            .tls_dyn_buf_strategy = HTTP_TLS_DYN_BUF_RX_STATIC,
            .skip_cert_common_name_check = true,
            .max_redirection_count = OTA_HTTP_MAX_REDIRECTS,
        };
        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (client) {
            *buffer_size_out = sizes[i] ? sizes[i] : 512;   /* 0 = IDF 默认 */
            if (sizes[i] != (int)OTA_BUFFER_SIZE) {
                ESP_LOGW("OTA", "http client init fell back to rx=%d", *buffer_size_out);
            }
            return client;
        }
        ESP_LOGW("OTA", "http client init failed at rx=%d free=%u max=%u",
                 sizes[i], (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    }
    return NULL;
}

/* Keep HTTPS failures aligned with the configured TLS record capacity. */
static void detail_append_tls_hint(const char *url, char *detail, size_t detail_size)
{
    if (!url || strncmp(url, "https://", 8) != 0) return;
    const size_t used = strlen(detail);
    if (used + 1 >= detail_size) return;
    snprintf(detail + used, detail_size - used,
             " | https needs TLS heap; record limit=%u",
             (unsigned)CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN);
}

static void remote_worker(void* parameter)
{
    OtaManifest manifest = *(const OtaManifest*)parameter;
    int rx_buffer = (int)OTA_BUFFER_SIZE;
    esp_http_client_handle_t client = http_client_init_with_fallback(&manifest, &rx_buffer);
    OtaResult result = OTA_RESULT_HTTP_ERROR;
    bool client_closed = false;
    int status = -1;
    int64_t content_length = -1;
    char detail[128];
    g_arm_failure_detail[0] = '\0';
    snprintf(detail, sizeof(detail), "HTTP client init failed");
    detail_append_heap(detail, sizeof(detail));

    if (client) {
        result = prepare_http_download(client, manifest.file_size,
                                       &status, &content_length,
                                       detail, sizeof(detail));
        {
            /* 下载侧上下文也留痕: 直接看出 CDN/源站握手是否成功、长度对不对;
             * 附上实际用的接收缓冲与堆余量, 退让和碎片化一眼可见 */
            char line[96];
            snprintf(line, sizeof(line), "target=%s status=%d len=%lld rx=%d",
                     ota_target_name(manifest.target), status,
                     (long long)content_length, rx_buffer);
            detail_append_heap(line, sizeof(line));
            mqtt_publish_ota_trace("http", line);
        }
        if (result == OTA_RESULT_OK) {
            result = install_stream(&manifest, http_stream_read, client,
                                    http_stream_resume, OTA_MAX_RESUMES,
                                    &client_closed);
            if (result != OTA_RESULT_OK) {
                if (g_arm_failure_detail[0]) {
                    /* ARM 阶段的真实原因比 HTTP 上下文有用: 错误码已在 code 字段,
                     * message 用来说明是暂存写不进去还是 ARM 协议没走通 */
                    snprintf(detail, sizeof(detail), "%s", g_arm_failure_detail);
                } else {
                    snprintf(detail, sizeof(detail), "%s status=%d len=%lld",
                             ota_result_code(result), status,
                             (long long)content_length);
                    detail_append_heap(detail, sizeof(detail));
                }
                ESP_LOGE("OTA", "%s", detail);
                mqtt_publish_ota_trace("result", detail);
            }
        } else {
            /* HTTP 阶段就失败 (含 TLS 握手): detail 里有具体原因, 再补堆余量与 https 提示 */
            detail_append_heap(detail, sizeof(detail));
            detail_append_tls_hint(manifest.url, detail, sizeof(detail));
            mqtt_publish_ota_trace("http-fail", detail);
        }
    }
    if (client) {
        if (!client_closed) esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }

    if (cancel_requested()) {
        finish_failure(OTA_RESULT_OK, "OTA cancelled");
    } else if (result != OTA_RESULT_OK) {
        finish_failure(result, detail);
    } else if (manifest.target == OTA_TARGET_ESP) {
        g_remote_reboot_pending = ota_service_schedule_reboot(1000);
        if (!g_remote_reboot_pending)
            finish_failure(OTA_RESULT_TASK_CREATE_FAILED, "Unable to schedule OTA reboot");
    }

    ESP_LOGI("OTA", "remote worker stack_min_free=%u",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    /* The coordinator deletes this worker before restoring BLE. Self-delete
     * would defer stack reclamation to idle and race BLE allocations. */
    xTaskNotifyGive(g_remote_coordinator);
    vTaskSuspend(NULL);
}

static void remote_coordinator(void* parameter)
{
    (void)parameter;
    const bool quiet = g_manifest.target == OTA_TARGET_ESP;
    if (quiet) ota_quiet_begin();
    ESP_LOGI("OTA", "prepare remote OTA free=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    if (!ble_prov_suspend_for_ota()) {
        finish_failure(OTA_RESULT_INTERNAL, "Unable to pause BLE for remote OTA");
    } else if (cancel_requested()) {
        finish_failure(OTA_RESULT_OK, "OTA cancelled");
    } else if (!create_ota_task_with_retry(remote_worker, "ota_remote",
                                          OTA_TASK_STACK_SIZE, &g_manifest,
                                          &g_remote_task)) {
        finish_failure(OTA_RESULT_TASK_CREATE_FAILED, "Unable to create OTA worker");
    } else {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        vTaskDelete(g_remote_task);
        g_remote_task = NULL;
    }
    if (!g_remote_reboot_pending && !ble_prov_restore_after_ota()) {
        ESP_LOGE("OTA", "BLE restore failed after remote OTA");
        mqtt_publish_ota_trace("ble-restore", "BLE restore failed; restart required");
    }
    if (quiet) ota_quiet_end();
    if (g_lock && xSemaphoreTake(g_lock, portMAX_DELAY) == pdTRUE) {
        g_remote_cleanup_busy = false;
        g_remote_coordinator = NULL;
        xSemaphoreGive(g_lock);
    }
    vTaskDelete(NULL);
}

OtaResult ota_service_start_remote(const OtaManifest* manifest)
{
    OtaResult result = validate_manifest_cheap(manifest);
    if (result != OTA_RESULT_OK) return result;
    result = check_available(manifest);
    if (result != OTA_RESULT_OK) return result;
    result = authenticate_manifest(manifest);
    if (result != OTA_RESULT_OK) return result;
    result = reserve_manifest(manifest);
    if (result != OTA_RESULT_OK) return result;

    /* ESP 自升级用不到 ARM IAP 工作区 (~20KB): 先归还给堆, 让任务栈/接收缓冲/
     * (https 时的)TLS 上下文有机会拿到连续块。此刻 g_active 已置位, 不可能有
     * ARM 会话在跑, 释放是可证明安全的。 */
    if (manifest->target == OTA_TARGET_ESP) arm_ota_workspace_release();

    if (xSemaphoreTake(g_lock, portMAX_DELAY) != pdTRUE) {
        finish_failure(OTA_RESULT_INTERNAL, "Unable to prepare OTA worker");
        return OTA_RESULT_INTERNAL;
    }
    g_remote_cleanup_busy = true;
    g_remote_reboot_pending = false;
    xSemaphoreGive(g_lock);
    /* Lower priority also lets xTaskCreate return the coordinator handle
     * before the full worker can report completion. */
    if (xTaskCreate(remote_coordinator, "ota_prepare",
                    OTA_COORDINATOR_STACK_SIZE, NULL,
                    OTA_COORDINATOR_PRIORITY, &g_remote_coordinator) != pdPASS) {
        xSemaphoreTake(g_lock, portMAX_DELAY);
        g_remote_cleanup_busy = false;
        g_remote_coordinator = NULL;
        xSemaphoreGive(g_lock);
        finish_failure(OTA_RESULT_TASK_CREATE_FAILED, "Unable to create OTA coordinator");
        return OTA_RESULT_TASK_CREATE_FAILED;
    }
    return OTA_RESULT_OK;
}

OtaResult ota_service_start_local(const OtaManifest* manifest,
                                  OtaStreamReader reader, void* reader_context)
{
    OtaResult result = validate_manifest_cheap(manifest);
    if (result != OTA_RESULT_OK) return result;
    result = check_available(manifest);
    if (result != OTA_RESULT_OK) return result;
    result = authenticate_manifest(manifest);
    if (result != OTA_RESULT_OK) return result;
    result = reserve_manifest(manifest);
    if (result != OTA_RESULT_OK) return result;

    /* BLE 本地 ESP 升级同理: 先归还 ARM IAP 工作区 (~20KB) */
    if (manifest->target == OTA_TARGET_ESP) arm_ota_workspace_release();

    result = install_stream(manifest, reader, reader_context, NULL, 0, NULL);
    if (cancel_requested()) {
        finish_failure(OTA_RESULT_OK, "OTA cancelled");
        return OTA_RESULT_OK;
    }
    if (result != OTA_RESULT_OK) finish_failure(result,
        manifest->target!=OTA_TARGET_ESP && g_arm_failure_detail[0]
            ? g_arm_failure_detail : ota_result_code(result));
    return result;
}

OtaResult ota_service_cancel(const char* task_id)
{
    if (!g_lock || xSemaphoreTake(g_lock, portMAX_DELAY) != pdTRUE) {
        return OTA_RESULT_INTERNAL;
    }
    if (!g_active) {
        xSemaphoreGive(g_lock);
        return OTA_RESULT_INVALID_PARAMS;
    }
    if (task_id && task_id[0] && strcmp(task_id, g_snapshot.task_id) != 0) {
        xSemaphoreGive(g_lock);
        return OTA_RESULT_INVALID_PARAMS;
    }
    if (g_snapshot.state == OTA_STATE_INSTALLING || g_snapshot.state == OTA_STATE_VERIFYING || g_snapshot.state == OTA_STATE_REBOOTING || g_snapshot.state == OTA_STATE_SUCCEEDED) {
        xSemaphoreGive(g_lock);
        return OTA_RESULT_NOT_CANCELLABLE;
    }
    g_snapshot.state = OTA_STATE_CANCELLING;
    OtaSnapshot copy = g_snapshot;
    atomic_store(&g_cancel_requested, true);
    xSemaphoreGive(g_lock);
    notify_snapshot(&copy);
    return OTA_RESULT_OK;
}

bool ota_service_get_snapshot(OtaSnapshot* snapshot)
{
    if (!snapshot || !g_lock || xSemaphoreTake(g_lock, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    *snapshot = g_snapshot;
    xSemaphoreGive(g_lock);
    return true;
}

bool ota_service_is_busy(void)
{
    bool active = false;
    if (g_lock && xSemaphoreTake(g_lock, portMAX_DELAY) == pdTRUE) {
        active = g_active || g_admin_busy || g_remote_cleanup_busy;
        xSemaphoreGive(g_lock);
    }
    return active;
}
bool ota_service_admin_begin(void){
    if(!g_lock||xSemaphoreTake(g_lock,pdMS_TO_TICKS(100))!=pdTRUE)return false;
    bool allowed=!g_active&&!g_admin_busy&&!g_remote_cleanup_busy;
    if(allowed)g_admin_busy=true;
    xSemaphoreGive(g_lock);return allowed;
}
void ota_service_admin_end(void){
    if(g_lock&&xSemaphoreTake(g_lock,portMAX_DELAY)==pdTRUE){g_admin_busy=false;xSemaphoreGive(g_lock);}
}

const char* ota_service_get_task_id(void)
{
    static char task_id[64];
    OtaSnapshot snapshot;
    if (!ota_service_get_snapshot(&snapshot)) return "";
    strlcpy(task_id, snapshot.task_id, sizeof(task_id));
    return task_id;
}

void ota_service_process(void)
{
    /* Transfer workers own ARM progression. Kept for API compatibility. */
}

static void reboot_task(void* parameter)
{
    const uint32_t delay_ms = (uint32_t)(uintptr_t)parameter;
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
    esp_restart();
}

bool ota_service_schedule_reboot(uint32_t delay_ms)
{
    return create_ota_task_with_retry(reboot_task, "ota_reboot", 2048,
                                      (void*)(uintptr_t)delay_ms, NULL);
}
