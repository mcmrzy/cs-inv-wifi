#include "ota_manager.h"
#include "arm_ota.h"
#include "led_status.h"
#include "nvs_storage.h"

#include <string.h>

#include "esp_wifi.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "OTA";

#define OTA_ESP_MAX_SIZE        (2 * 1024 * 1024)
#define OTA_ARM_MAX_SIZE        (256 * 1024)
#define OTA_DOWNLOAD_TIMEOUT_MS 60000
#define OTA_HTTP_BUF_SIZE       4096
#define NVS_OTA_NAMESPACE       "inv_cfg"
#define KEY_ARM_VER             "arm_ver"

static OtaStatus   g_ota_status   = OTA_STATUS_IDLE;
static OtaTarget   g_ota_target   = OTA_TARGET_ESP;
static int         g_ota_progress = 0;
static char        g_ota_message[128] = {0};
static OtaProgressCallback g_ota_callback = NULL;

static bool g_esp_ota_active = false;
static bool g_arm_ota_active = false;

static esp_ota_handle_t g_ota_handle = 0;
static const esp_partition_t* g_ota_partition = NULL;
static int g_esp_content_len = 0;
static int g_esp_downloaded = 0;
static esp_http_client_handle_t g_http_client = NULL;
static bool g_http_connected = false;

static char g_arm_version[32] = {0};

static bool is_valid_url(const char* url) {
    if (!url || strlen(url) < 8) return false;
    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) {
        return false;
    }
    return true;
}

static void notify_callback(OtaTarget target, int percent, OtaStatus status, const char* message) {
    if (g_ota_callback) {
        g_ota_callback(target, percent, status, message);
    }
}

static void set_state(OtaStatus status, const char* msg) {
    g_ota_status = status;
    if (msg) {
        strncpy(g_ota_message, msg, sizeof(g_ota_message) - 1);
        g_ota_message[sizeof(g_ota_message) - 1] = '\0';
    }
}

static void load_arm_version(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_OTA_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(g_arm_version);
        if (nvs_get_str(h, KEY_ARM_VER, g_arm_version, &len) != ESP_OK) {
            g_arm_version[0] = '\0';
        }
        nvs_close(h);
    }
}

static void save_arm_version(const char* version) {
    strncpy(g_arm_version, version, sizeof(g_arm_version) - 1);
    g_arm_version[sizeof(g_arm_version) - 1] = '\0';

    nvs_handle_t h;
    if (nvs_open(NVS_OTA_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, KEY_ARM_VER, version);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void arm_ota_progress_cb(ArmOtaState state, int percent, const char* message) {
    switch (state) {
        case ARM_OTA_IDLE:
            break;
        case ARM_OTA_WAIT_READY:
            set_state(OTA_STATUS_DOWNLOADING, message);
            notify_callback(OTA_TARGET_ARM, 0, OTA_STATUS_DOWNLOADING, message);
            break;
        case ARM_OTA_SENDING:
        case ARM_OTA_WAIT_ACK:
            g_ota_progress = percent;
            set_state(OTA_STATUS_WRITING, message);
            notify_callback(OTA_TARGET_ARM, percent, OTA_STATUS_WRITING, message);
            break;
        case ARM_OTA_VERIFYING:
            set_state(OTA_STATUS_VERIFYING, message);
            notify_callback(OTA_TARGET_ARM, 100, OTA_STATUS_VERIFYING, message);
            break;
        case ARM_OTA_DONE:
            g_arm_ota_active = false;
            set_state(OTA_STATUS_DONE, "ARM OTA completed");
            notify_callback(OTA_TARGET_ARM, 100, OTA_STATUS_DONE, "ARM OTA completed");
            {
                const ArmFirmwareInfo* info = arm_ota_get_firmware_info();
                if (info && info->valid && strlen(info->version) > 0) {
                    save_arm_version(info->version);
                }
            }
            break;
        case ARM_OTA_ERROR:
            g_arm_ota_active = false;
            set_state(OTA_STATUS_ERROR, message);
            notify_callback(OTA_TARGET_ARM, g_ota_progress, OTA_STATUS_ERROR, message);
            break;
        case ARM_OTA_TIMEOUT:
            g_arm_ota_active = false;
            set_state(OTA_STATUS_ERROR, message);
            notify_callback(OTA_TARGET_ARM, g_ota_progress, OTA_STATUS_ERROR, message);
            break;
    }
}

static void esp_ota_cleanup(void) {
    if (g_http_client) {
        esp_http_client_close(g_http_client);
        esp_http_client_cleanup(g_http_client);
        g_http_client = NULL;
    }
    g_http_connected = false;
    g_esp_ota_active = false;
}

static void do_esp_ota_begin(const char* url) {
    ESP_LOGI(TAG, "ESP OTA start: %s", url);

    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = OTA_DOWNLOAD_TIMEOUT_MS,
        .buffer_size = OTA_HTTP_BUF_SIZE,
    };

    g_http_client = esp_http_client_init(&config);
    if (!g_http_client) {
        set_state(OTA_STATUS_ERROR, "HTTP init failed");
        g_esp_ota_active = false;
        notify_callback(OTA_TARGET_ESP, 0, OTA_STATUS_ERROR, g_ota_message);
        return;
    }

    esp_err_t err = esp_http_client_open(g_http_client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
        set_state(OTA_STATUS_ERROR, "HTTP open failed");
        esp_http_client_cleanup(g_http_client);
        g_http_client = NULL;
        g_esp_ota_active = false;
        notify_callback(OTA_TARGET_ESP, 0, OTA_STATUS_ERROR, g_ota_message);
        return;
    }

    g_esp_content_len = esp_http_client_fetch_headers(g_http_client);
    int status = esp_http_client_get_status_code(g_http_client);
    if (status != 200) {
        ESP_LOGE(TAG, "HTTP status: %d", status);
        set_state(OTA_STATUS_ERROR, "HTTP GET failed");
        esp_ota_cleanup();
        notify_callback(OTA_TARGET_ESP, 0, OTA_STATUS_ERROR, g_ota_message);
        return;
    }

    if (g_esp_content_len <= 0 || g_esp_content_len > OTA_ESP_MAX_SIZE) {
        ESP_LOGE(TAG, "Invalid firmware size: %d", g_esp_content_len);
        set_state(OTA_STATUS_ERROR, "Invalid firmware size");
        esp_ota_cleanup();
        notify_callback(OTA_TARGET_ESP, 0, OTA_STATUS_ERROR, g_ota_message);
        return;
    }

    ESP_LOGI(TAG, "Firmware size: %d bytes", g_esp_content_len);

    g_ota_partition = esp_ota_get_next_update_partition(NULL);
    if (!g_ota_partition) {
        ESP_LOGE(TAG, "No OTA partition found");
        set_state(OTA_STATUS_ERROR, "No OTA partition");
        esp_ota_cleanup();
        notify_callback(OTA_TARGET_ESP, 0, OTA_STATUS_ERROR, g_ota_message);
        return;
    }

    err = esp_ota_begin(g_ota_partition, OTA_SIZE_UNKNOWN, &g_ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        set_state(OTA_STATUS_ERROR, "OTA begin failed");
        esp_ota_cleanup();
        notify_callback(OTA_TARGET_ESP, 0, OTA_STATUS_ERROR, g_ota_message);
        return;
    }

    g_esp_downloaded = 0;
    g_http_connected = true;
    set_state(OTA_STATUS_DOWNLOADING, "Downloading firmware...");
    notify_callback(OTA_TARGET_ESP, 0, OTA_STATUS_DOWNLOADING, g_ota_message);
}

static void esp_ota_process(void) {
    if (!g_http_connected) return;

    uint8_t buf[OTA_HTTP_BUF_SIZE];
    int read_len = esp_http_client_read(g_http_client, (char*)buf, sizeof(buf));
    if (read_len > 0) {
        g_esp_downloaded += read_len;
        int percent = (g_esp_content_len > 0)
                      ? (int)((int64_t)g_esp_downloaded * 100 / g_esp_content_len)
                      : 0;

        if (percent < 90) {
            set_state(OTA_STATUS_DOWNLOADING, "Downloading firmware...");
        } else {
            set_state(OTA_STATUS_WRITING, "Writing firmware...");
        }

        g_ota_progress = percent;
        notify_callback(OTA_TARGET_ESP, percent, g_ota_status, g_ota_message);

        esp_err_t err = esp_ota_write(g_ota_handle, buf, read_len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            esp_ota_abort(g_ota_handle);
            esp_ota_cleanup();
            set_state(OTA_STATUS_ERROR, "Flash write error");
            notify_callback(OTA_TARGET_ESP, percent, OTA_STATUS_ERROR, g_ota_message);
            return;
        }
    }

    if (g_esp_downloaded >= g_esp_content_len) {
        g_http_connected = false;
        esp_http_client_close(g_http_client);
        esp_http_client_cleanup(g_http_client);
        g_http_client = NULL;

        set_state(OTA_STATUS_VERIFYING, "Verifying firmware...");
        notify_callback(OTA_TARGET_ESP, 100, OTA_STATUS_VERIFYING, g_ota_message);

        esp_err_t err = esp_ota_end(g_ota_handle);
        if (err == ESP_OK) {
            err = esp_ota_set_boot_partition(g_ota_partition);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "OTA success, restarting...");
                set_state(OTA_STATUS_DONE, "Update successful");
                notify_callback(OTA_TARGET_ESP, 100, OTA_STATUS_DONE, g_ota_message);
                g_esp_ota_active = false;
                vTaskDelay(pdMS_TO_TICKS(1000));
                esp_restart();
            } else {
                ESP_LOGE(TAG, "Set boot partition failed: %s", esp_err_to_name(err));
                set_state(OTA_STATUS_ERROR, "Set boot partition failed");
                g_esp_ota_active = false;
                notify_callback(OTA_TARGET_ESP, 100, OTA_STATUS_ERROR, g_ota_message);
            }
        } else {
            ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
            set_state(OTA_STATUS_ERROR, "OTA verify failed");
            g_esp_ota_active = false;
            notify_callback(OTA_TARGET_ESP, 100, OTA_STATUS_ERROR, g_ota_message);
        }
    }
}

static void arm_ota_download_and_flash(const char* url) {
    ESP_LOGI(TAG, "ARM OTA streaming start: %s", url);

    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = OTA_DOWNLOAD_TIMEOUT_MS,
        .buffer_size = OTA_HTTP_BUF_SIZE,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "ARM HTTP init failed");
        set_state(OTA_STATUS_ERROR, "HTTP init failed");
        g_arm_ota_active = false;
        notify_callback(OTA_TARGET_ARM, 0, OTA_STATUS_ERROR, g_ota_message);
        return;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ARM HTTP open failed: %s", esp_err_to_name(err));
        set_state(OTA_STATUS_ERROR, "HTTP open failed");
        esp_http_client_cleanup(client);
        g_arm_ota_active = false;
        notify_callback(OTA_TARGET_ARM, 0, OTA_STATUS_ERROR, g_ota_message);
        return;
    }

    int content_len = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        ESP_LOGE(TAG, "ARM HTTP status: %d", status);
        set_state(OTA_STATUS_ERROR, "HTTP GET failed");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        g_arm_ota_active = false;
        notify_callback(OTA_TARGET_ARM, 0, OTA_STATUS_ERROR, g_ota_message);
        return;
    }

    if (content_len <= 0 || content_len > OTA_ARM_MAX_SIZE) {
        ESP_LOGE(TAG, "ARM firmware size invalid: %d", content_len);
        set_state(OTA_STATUS_ERROR, "Invalid firmware size");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        g_arm_ota_active = false;
        notify_callback(OTA_TARGET_ARM, 0, OTA_STATUS_ERROR, g_ota_message);
        return;
    }

    ESP_LOGI(TAG, "ARM firmware size: %d bytes, downloading...", content_len);

    if (!arm_ota_start_streaming(content_len)) {
        ESP_LOGE(TAG, "arm_ota_start_streaming failed");
        set_state(OTA_STATUS_ERROR, "ARM OTA start failed");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        g_arm_ota_active = false;
        notify_callback(OTA_TARGET_ARM, 0, OTA_STATUS_ERROR, g_ota_message);
        return;
    }

    uint8_t buf[OTA_HTTP_BUF_SIZE];
    int downloaded = 0;
    int64_t timeout_start = esp_timer_get_time();

    while (downloaded < content_len && g_arm_ota_active) {
        arm_ota_process();

        if (arm_ota_get_state() == ARM_OTA_ERROR ||
            arm_ota_get_state() == ARM_OTA_TIMEOUT) {
            ESP_LOGE(TAG, "ARM OTA protocol error");
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            set_state(OTA_STATUS_ERROR, arm_ota_get_message());
            g_arm_ota_active = false;
            notify_callback(OTA_TARGET_ARM, arm_ota_get_progress(), OTA_STATUS_ERROR, g_ota_message);
            return;
        }

        if (arm_ota_get_state() == ARM_OTA_DONE) {
            break;
        }

        if ((esp_timer_get_time() - timeout_start) / 1000 > OTA_DOWNLOAD_TIMEOUT_MS) {
            ESP_LOGE(TAG, "ARM download timeout");
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            set_state(OTA_STATUS_ERROR, "Download timeout");
            g_arm_ota_active = false;
            notify_callback(OTA_TARGET_ARM, 0, OTA_STATUS_ERROR, g_ota_message);
            return;
        }

        int to_read = content_len - downloaded;
        if (to_read > (int)sizeof(buf)) to_read = sizeof(buf);
        int read_len = esp_http_client_read(client, (char*)buf, to_read);
        if (read_len > 0) {
            downloaded += read_len;
            int percent = (int)((int64_t)downloaded * 100 / content_len);
            g_ota_progress = percent;
            notify_callback(OTA_TARGET_ARM, percent, OTA_STATUS_DOWNLOADING, "Downloading...");

            arm_ota_feed(buf, read_len);
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (!g_arm_ota_active) {
        ESP_LOGW(TAG, "ARM OTA cancelled");
        return;
    }

    ESP_LOGI(TAG, "Downloaded %d bytes, ending streaming...", downloaded);
    notify_callback(OTA_TARGET_ARM, 100, OTA_STATUS_WRITING, "Sending last packets...");

    arm_ota_end_streaming();

    while (g_arm_ota_active) {
        arm_ota_process();

        ArmOtaState st = arm_ota_get_state();
        if (st == ARM_OTA_DONE) {
            ESP_LOGI(TAG, "ARM OTA completed");
            g_arm_ota_active = false;
            set_state(OTA_STATUS_DONE, "ARM OTA completed");
            notify_callback(OTA_TARGET_ARM, 100, OTA_STATUS_DONE, "ARM OTA completed");
            return;
        }
        if (st == ARM_OTA_ERROR || st == ARM_OTA_TIMEOUT) {
            ESP_LOGE(TAG, "ARM OTA error: %s", arm_ota_get_message());
            g_arm_ota_active = false;
            set_state(OTA_STATUS_ERROR, arm_ota_get_message());
            notify_callback(OTA_TARGET_ARM, arm_ota_get_progress(), OTA_STATUS_ERROR, arm_ota_get_message());
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void ota_manager_init(void) {
    g_ota_status = OTA_STATUS_IDLE;
    g_ota_target = OTA_TARGET_ESP;
    g_ota_progress = 0;
    g_ota_message[0] = '\0';
    g_ota_callback = NULL;
    g_esp_ota_active = false;
    g_arm_ota_active = false;
    g_http_client = NULL;
    g_http_connected = false;

    arm_ota_init();
    arm_ota_set_callback(arm_ota_progress_cb);

    load_arm_version();

    ESP_LOGI(TAG, "Manager initialized, ESP ver=%s, ARM ver=%s",
             FIRMWARE_VERSION, g_arm_version[0] ? g_arm_version : "N/A");
}

void ota_set_progress_callback(OtaProgressCallback cb) {
    g_ota_callback = cb;
}

bool ota_start_esp(const char* url) {
    if (!url || !is_valid_url(url)) {
        ESP_LOGW(TAG, "Invalid URL");
        return false;
    }
    if (ota_is_busy()) {
        ESP_LOGW(TAG, "Already busy");
        return false;
    }

    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK) {
        ESP_LOGW(TAG, "WiFi not connected");
        return false;
    }

    g_ota_target = OTA_TARGET_ESP;
    g_ota_progress = 0;
    g_esp_ota_active = true;

    led1_set(LED_BREATH);
    led2_set(LED_BREATH);

    do_esp_ota_begin(url);
    return true;
}

bool ota_start_arm(const char* url) {
    if (!url || !is_valid_url(url)) {
        ESP_LOGW(TAG, "Invalid URL");
        return false;
    }
    if (ota_is_busy()) {
        ESP_LOGW(TAG, "Already busy");
        return false;
    }

    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK) {
        ESP_LOGW(TAG, "WiFi not connected");
        return false;
    }

    g_ota_target = OTA_TARGET_ARM;
    g_ota_progress = 0;
    g_arm_ota_active = true;

    led1_set(LED_BREATH);
    led2_set(LED_BREATH);

    arm_ota_download_and_flash(url);
    return true;
}

OtaStatus ota_get_status(void) {
    return g_ota_status;
}

OtaTarget ota_get_target(void) {
    return g_ota_target;
}

int ota_get_progress(void) {
    return g_ota_progress;
}

const char* ota_get_message(void) {
    return g_ota_message;
}

void ota_manager_process(void) {
    if (!ota_is_busy()) return;

    if (g_esp_ota_active) {
        esp_ota_process();
    }

    if (g_arm_ota_active) {
        arm_ota_process();
    }

    if (!ota_is_busy()) {
        if (g_ota_status == OTA_STATUS_DONE) {
            led1_set(LED_SOLID);
            led2_set(LED_SOLID);
        } else if (g_ota_status == OTA_STATUS_ERROR) {
            led1_set(LED_FAULT);
            led2_set(LED_FAULT);
        }
    }
}

bool ota_is_busy(void) {
    return g_esp_ota_active || g_arm_ota_active;
}

const char* ota_get_esp_version(void) {
    return FIRMWARE_VERSION;
}

const char* ota_get_arm_version(void) {
    return g_arm_version[0] ? g_arm_version : "unknown";
}
