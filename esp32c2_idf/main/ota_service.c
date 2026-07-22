#include "ota_service.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "arm_ota.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"

#include "nvs_storage.h"
#include "mbedtls/base64.h"
#include "monocypher-ed25519.h"
#include "ota_release_pubkey.h"

#define OTA_BUFFER_SIZE       4096U
#define OTA_ARM_MAX_SIZE      (256U * 1024U)
#define OTA_TASK_STACK_SIZE   8192U
#define OTA_TASK_PRIORITY     5U
#define OTA_READ_IDLE_MS      10U
#define OTA_HEALTH_DELAY_MS   10000U
#define OTA_HEALTH_MAX_AGE_MS 2000U

static SemaphoreHandle_t g_lock;
static OtaSnapshot g_snapshot;
static OtaManifest g_manifest;
static OtaStatusObserver g_observer;
static TaskHandle_t g_remote_task;
static atomic_bool g_cancel_requested;
static atomic_uint_fast32_t g_main_loop_tick;
static bool g_active;
static bool g_boot_validation_required;
static StorageOtaPending g_boot_pending;
/* The service admits only one transfer at a time, so one shared buffer avoids
 * placing 4 KiB on either the HTTP server or OTA worker stack. */
static uint8_t g_transfer_buffer[OTA_BUFFER_SIZE];

static void notify_snapshot(const OtaSnapshot* snapshot)
{
    OtaStatusObserver observer = NULL;
    if (g_lock && xSemaphoreTake(g_lock, portMAX_DELAY) == pdTRUE) {
        observer = g_observer;
        xSemaphoreGive(g_lock);
    }
    if (observer) observer(snapshot);
}

static void update_snapshot(OtaState state, uint8_t progress,
                            uint32_t received, OtaResult result,
                            const char* message)
{
    OtaSnapshot copy;
    if (!g_lock || xSemaphoreTake(g_lock, portMAX_DELAY) != pdTRUE) return;
    g_snapshot.state = state;
    g_snapshot.progress = progress > 100 ? 100 : progress;
    g_snapshot.received = received;
    strlcpy(g_snapshot.error_code, ota_result_code(result), sizeof(g_snapshot.error_code));
    strlcpy(g_snapshot.message, message ? message : "", sizeof(g_snapshot.message));
    copy = g_snapshot;
    xSemaphoreGive(g_lock);
    notify_snapshot(&copy);
}

static void release_active(void)
{
    if (!g_lock || xSemaphoreTake(g_lock, portMAX_DELAY) != pdTRUE) return;
    g_active = false;
    xSemaphoreGive(g_lock);
}

const char* ota_target_name(OtaTarget target)
{
    return target == OTA_TARGET_ARM ? "arm" : "esp";
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
        "FLASH_WRITE_FAILED", "ARM_TRANSFER_FAILED", "PERSIST_FAILED",
        "OTA_NOT_CANCELLABLE", "TASK_CREATE_FAILED", "INTERNAL_ERROR",
    };
    return result <= OTA_RESULT_INTERNAL ? codes[result] : "INTERNAL_ERROR";
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
        if (storage_set_ota_security_version_for_target(
                "esp", g_boot_pending.security_version) && storage_clear_ota_pending()) {
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
    if (!g_lock || xSemaphoreTake(g_lock, portMAX_DELAY) != pdTRUE) return;
    g_observer = observer;
    copy = g_snapshot;
    replay = observer && copy.state != OTA_STATE_IDLE;
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
    if (last_alive == 0 ||
        (now - last_alive) > pdMS_TO_TICKS(OTA_HEALTH_MAX_AGE_MS)) {
        update_snapshot(OTA_STATE_FAILED, 0, 0, OTA_RESULT_INTERNAL,
                        "Main loop health confirmation timed out");
        vTaskDelete(NULL);
        return;
    }

    if (esp_ota_mark_app_valid_cancel_rollback() != ESP_OK) {
        update_snapshot(OTA_STATE_FAILED, 0, 0, OTA_RESULT_INTERNAL,
                        "Unable to confirm running ESP image");
        vTaskDelete(NULL);
        return;
    }

    if (g_boot_pending.security_version != 0 &&
        (!storage_set_ota_security_version_for_target(
             "esp", g_boot_pending.security_version) ||
         !storage_clear_ota_pending())) {
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
    return xTaskCreate(boot_health_task, "ota_health", 3072, NULL,
                       OTA_TASK_PRIORITY, NULL) == pdPASS;
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
        !valid_sha256(manifest->sha256) || manifest->file_size == 0 ||
        manifest->security_version == 0 || manifest->timeout_seconds < 60 ||
        manifest->timeout_seconds > 3600) {
        return OTA_RESULT_INVALID_PARAMS;
    }
    if (manifest->target != OTA_TARGET_ESP && manifest->target != OTA_TARGET_ARM) {
        return OTA_RESULT_INVALID_TARGET;
    }
    if (manifest->source == OTA_SOURCE_MQTT && strncmp(manifest->url, "https://", 8) != 0) {
        return OTA_RESULT_TLS_REQUIRED;
    }
    if (!manifest->signature[0]) return OTA_RESULT_SIGNATURE_REQUIRED;

    if (manifest->target == OTA_TARGET_ARM && manifest->file_size > OTA_ARM_MAX_SIZE) {
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
    if (g_active) {
        result = strcmp(g_snapshot.task_id, manifest->task_id) == 0
            ? OTA_RESULT_DUPLICATE : OTA_RESULT_BUSY;
    }
    xSemaphoreGive(g_lock);
    return result;
}

static OtaResult authenticate_manifest(const OtaManifest* manifest)
{
    /* Exactly one Ed25519 verification is performed when a task is admitted.
     * Chunk integrity is covered separately by the streaming SHA-256 below. */
    if (!ota_verify_release_signature(
            ota_target_name(manifest->target), manifest->version,
            manifest->file_size, manifest->sha256,
            manifest->security_version, manifest->signature)) {
        return OTA_RESULT_SIGNATURE_INVALID;
    }

    uint32_t installed = 0;
    if (!storage_get_ota_security_version_for_target(
            ota_target_name(manifest->target), &installed)) {
        return OTA_RESULT_SECURITY_READ_FAILED;
    }
    if (manifest->security_version <= installed) return OTA_RESULT_ROLLBACK_REJECTED;

    return OTA_RESULT_OK;
}

static OtaResult reserve_manifest(const OtaManifest* manifest)
{
    if (!g_lock || xSemaphoreTake(g_lock, portMAX_DELAY) != pdTRUE) {
        return OTA_RESULT_INTERNAL;
    }
    if (g_active) {
        const bool duplicate = strcmp(g_snapshot.task_id, manifest->task_id) == 0;
        xSemaphoreGive(g_lock);
        return duplicate ? OTA_RESULT_DUPLICATE : OTA_RESULT_BUSY;
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
    strlcpy(g_snapshot.error_code, "OK", sizeof(g_snapshot.error_code));
    atomic_store(&g_cancel_requested, false);
    OtaSnapshot copy = g_snapshot;
    xSemaphoreGive(g_lock);
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

static OtaResult read_and_stage(const OtaManifest* manifest,
                                OtaStreamReader reader, void* context,
                                esp_ota_handle_t* esp_handle)
{
    uint32_t received = 0;
    uint8_t last_progress = 0;
    const int64_t started_ms = esp_timer_get_time() / 1000;
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
        const size_t capacity = remaining < sizeof(g_transfer_buffer)
            ? remaining : sizeof(g_transfer_buffer);
        const int read_count = reader(context, g_transfer_buffer, capacity);
        if (read_count < 0 || (size_t)read_count > capacity) {
            mbedtls_sha256_free(&sha);
            return OTA_RESULT_READ_FAILED;
        }
        if (read_count == 0) {
            vTaskDelay(pdMS_TO_TICKS(OTA_READ_IDLE_MS));
            continue;
        }

        if (mbedtls_sha256_update(&sha, g_transfer_buffer, (size_t)read_count) != 0) {
            mbedtls_sha256_free(&sha);
            return OTA_RESULT_INTERNAL;
        }
        if (manifest->target == OTA_TARGET_ESP) {
            if (esp_ota_write(*esp_handle, g_transfer_buffer, (size_t)read_count) != ESP_OK) {
                mbedtls_sha256_free(&sha);
                return OTA_RESULT_FLASH_WRITE_FAILED;
            }
        } else if (!arm_ota_feed(g_transfer_buffer, (size_t)read_count)) {
            mbedtls_sha256_free(&sha);
            return OTA_RESULT_ARM_TRANSFER_FAILED;
        }

        received += (uint32_t)read_count;
        const uint8_t progress = (uint8_t)(((uint64_t)received * 100U) / manifest->file_size);
        if (progress != last_progress) {
            last_progress = progress;
            update_snapshot(manifest->source == OTA_SOURCE_MQTT ? OTA_STATE_DOWNLOADING : OTA_STATE_RECEIVING,
                            progress, received, OTA_RESULT_OK, "Receiving firmware");
        }
    }

    uint8_t digest[32];
    char digest_hex[65];
    const int hash_result = mbedtls_sha256_finish(&sha, digest);
    mbedtls_sha256_free(&sha);
    if (hash_result != 0) return OTA_RESULT_INTERNAL;
    hash_to_hex(digest, digest_hex);
    if (strcmp(digest_hex, manifest->sha256) != 0) return OTA_RESULT_HASH_MISMATCH;
    return OTA_RESULT_OK;
}

static OtaResult install_esp(const OtaManifest* manifest,
                             OtaStreamReader reader, void* context)
{
    const esp_partition_t* update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition || manifest->file_size > update_partition->size) {
        return OTA_RESULT_FILE_TOO_LARGE;
    }

    esp_ota_handle_t handle = 0;
    if (esp_ota_begin(update_partition, manifest->file_size, &handle) != ESP_OK) {
        return OTA_RESULT_FLASH_WRITE_FAILED;
    }

    OtaResult result = read_and_stage(manifest, reader, context, &handle);
    if (result != OTA_RESULT_OK || cancel_requested()) {
        esp_ota_abort(handle);
        return cancel_requested() ? OTA_RESULT_OK : result;
    }

    update_snapshot(OTA_STATE_VERIFYING, 100, manifest->file_size,
                    OTA_RESULT_OK, "Verifying ESP image");
    if (esp_ota_end(handle) != ESP_OK) return OTA_RESULT_FLASH_WRITE_FAILED;

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

static void arm_progress(ArmOtaState state, int percent, const char* message)
{
    if (!ota_service_is_busy() || g_manifest.target != OTA_TARGET_ARM) return;
    if (state == ARM_OTA_SENDING || state == ARM_OTA_WAIT_ACK || state == ARM_OTA_VERIFYING) {
        const uint8_t progress = (uint8_t)(70 + (percent * 29) / 100);
        update_snapshot(OTA_STATE_INSTALLING, progress, g_manifest.file_size,
                        OTA_RESULT_OK, message);
    }
}

static OtaResult install_arm(const OtaManifest* manifest,
                             OtaStreamReader reader, void* context)
{
    if (!arm_ota_start_streaming(manifest->file_size)) {
        return OTA_RESULT_ARM_TRANSFER_FAILED;
    }

    esp_ota_handle_t unused = 0;
    OtaResult result = read_and_stage(manifest, reader, context, &unused);
    if (result != OTA_RESULT_OK || cancel_requested()) {
        arm_ota_cancel();
        return cancel_requested() ? OTA_RESULT_OK : result;
    }

    update_snapshot(OTA_STATE_INSTALLING, 70, manifest->file_size,
                    OTA_RESULT_OK, "Installing ARM image");
    arm_ota_set_callback(arm_progress);
    arm_ota_set_cancel_check(cancel_requested);
    const bool transfer_started = arm_ota_end_streaming();
    arm_ota_set_cancel_check(NULL);
    if (!transfer_started) {
        arm_ota_set_callback(NULL);
        return cancel_requested() ? OTA_RESULT_OK : OTA_RESULT_ARM_TRANSFER_FAILED;
    }

    const int64_t deadline = esp_timer_get_time() / 1000 + 10000;
    while (esp_timer_get_time() / 1000 < deadline) {
        if (cancel_requested()) {
            arm_ota_cancel();
            arm_ota_set_callback(NULL);
            return OTA_RESULT_OK;
        }
        arm_ota_process();
        const ArmOtaState state = arm_ota_get_state();
        if (state == ARM_OTA_DONE) {
            arm_ota_set_callback(NULL);
            if (!storage_set_ota_security_version_for_target(
                    "arm", manifest->security_version)) {
                return OTA_RESULT_PERSIST_FAILED;
            }
            update_snapshot(OTA_STATE_SUCCEEDED, 100, manifest->file_size,
                            OTA_RESULT_OK, "ARM OTA completed");
            release_active();
            return OTA_RESULT_OK;
        }
        if (state == ARM_OTA_ERROR || state == ARM_OTA_TIMEOUT) break;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    arm_ota_set_callback(NULL);
    arm_ota_cancel();
    return OTA_RESULT_ARM_TRANSFER_FAILED;
}

static OtaResult install_stream(const OtaManifest* manifest,
                                OtaStreamReader reader, void* context)
{
    if (!reader) return OTA_RESULT_INVALID_PARAMS;
    return manifest->target == OTA_TARGET_ESP
        ? install_esp(manifest, reader, context)
        : install_arm(manifest, reader, context);
}

static int http_stream_read(void* context, uint8_t* buffer, size_t capacity)
{
    return esp_http_client_read((esp_http_client_handle_t)context,
                                (char*)buffer, (int)capacity);
}

static void remote_worker(void* parameter)
{
    OtaManifest manifest = *(const OtaManifest*)parameter;
    esp_http_client_config_t config = {
        .url = manifest.url,
        .timeout_ms = (int)manifest.timeout_seconds * 1000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = true,
        .buffer_size = OTA_BUFFER_SIZE,
        .skip_cert_common_name_check = false,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    OtaResult result = OTA_RESULT_HTTP_ERROR;

    if (client && esp_http_client_open(client, 0) == ESP_OK) {
        const int64_t content_length = esp_http_client_fetch_headers(client);
        const int status = esp_http_client_get_status_code(client);
        if (status >= 200 && status < 300 && content_length >= 0) {
            if ((uint64_t)content_length == manifest.file_size) {
                result = install_stream(&manifest, http_stream_read, client);
            } else {
                result = OTA_RESULT_SIZE_MISMATCH;
            }
        }
    }
    if (client) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }

    if (cancel_requested()) {
        finish_failure(OTA_RESULT_OK, "OTA cancelled");
    } else if (result != OTA_RESULT_OK) {
        finish_failure(result, ota_result_code(result));
    } else if (manifest.target == OTA_TARGET_ESP) {
        ota_service_schedule_reboot(1000);
    }

    if (g_lock && xSemaphoreTake(g_lock, portMAX_DELAY) == pdTRUE) {
        g_remote_task = NULL;
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

    if (xTaskCreate(remote_worker, "ota_remote", OTA_TASK_STACK_SIZE,
                    &g_manifest, OTA_TASK_PRIORITY, &g_remote_task) != pdPASS) {
        finish_failure(OTA_RESULT_TASK_CREATE_FAILED, "Unable to create OTA worker");
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

    result = install_stream(manifest, reader, reader_context);
    if (cancel_requested()) {
        finish_failure(OTA_RESULT_OK, "OTA cancelled");
        return OTA_RESULT_OK;
    }
    if (result != OTA_RESULT_OK) finish_failure(result, ota_result_code(result));
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
    if (g_snapshot.state == OTA_STATE_REBOOTING || g_snapshot.state == OTA_STATE_SUCCEEDED) {
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
        active = g_active;
        xSemaphoreGive(g_lock);
    }
    return active;
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
    return xTaskCreate(reboot_task, "ota_reboot", 2048,
                       (void*)(uintptr_t)delay_ms, OTA_TASK_PRIORITY, NULL) == pdPASS;
}
