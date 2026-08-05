/*
 * app_main.c — CS-L10-6K2 采集器主程序 (ESP32-C3)
 *
 * 初始化顺序:
 *   NVS → SN/Key → ARM 串口轮询+OTA → MQTT+OTA → 离线缓存
 *   → WiFi STA → BLE 配网(常驻) → SNTP → 主循环状态机
 *
 * 主循环 (100ms):
 *   arm_poll_tick / telemetry_tick / sntp_sync_tick / offline_cache_process
 *   / mqtt 重连 / 离线补传 / ota 周期处理
 */
#include <string.h>
#include "config.h"
#include "nvs_storage.h"
#include "wifi_sta.h"
#include "esp_wifi.h"
#include "arm_poll.h"
#include "telemetry.h"
#include "cmd_handler.h"
#include "mqtt_handler.h"
#include "offline_cache.h"
#include "sntp_sync.h"
#include "ble_prov.h"
#include "ota_service.h"
#include "arm_ota.h"
#include "ota_mqtt_adapter.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "app";

/* ---- 主循环状态机 ---- */
typedef enum {
    STATE_INIT,
    STATE_STA_CONNECTING,
    STATE_MQTT_CONNECTING,
    STATE_RUNNING,
} sys_state_t;

static sys_state_t s_state = STATE_INIT;
static uint32_t s_state_enter_ms;
static uint32_t s_last_flush_ms;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* ---- SN: NVS 已有则加载, 否则由 MAC 派生 ---- */
static void ensure_sn(void)
{
    if (storage_has_sn()) {
        storage_get_sn(g_device_sn, sizeof(g_device_sn));
        ESP_LOGI(TAG, "SN loaded: %s", g_device_sn);
        return;
    }
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(g_device_sn, sizeof(g_device_sn), "%s%02X%02X%02X%02X%02X%02X",
             SN_DEV_PREFIX, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    storage_set_sn(g_device_sn);
    ESP_LOGI(TAG, "SN generated from MAC: %s", g_device_sn);
}

/* ---- Key: NVS 已有则加载, 否则用默认 MQTT 密码 ---- */
static void ensure_key(void)
{
    if (storage_get_key(g_device_key, sizeof(g_device_key))) {
        ESP_LOGI(TAG, "Key loaded");
        return;
    }
    snprintf(g_device_key, sizeof(g_device_key), "%s", MQTT_PASSWORD);
    storage_set_key(g_device_key);
    ESP_LOGI(TAG, "Key set to default (MQTT password)");
}

static void enter_state(sys_state_t st)
{
    if (s_state == st) {
        return;
    }
    s_state = st;
    s_state_enter_ms = now_ms();
    ESP_LOGI(TAG, "state -> %d", (int)st);
}

/* 配网结果反馈 (BLE_Provisioning_Protocol.md V1.2): 配网仅尝试 1 次, 最多约 15 秒 */
#define PROV_WIFI_TIMEOUT_MS   15000
static bool s_prov_busy = false;
static char s_prov_ssid[33];
static char s_prov_pass[65];

/* 配网连接任务: 连接 WiFi -> 按结果通知 App (connected/failed/not_found), 成功才保存 NVS */
static void prov_connect_task(void *arg)
{
    (void)arg;
    wifi_sta_connect(s_prov_ssid, s_prov_pass);
    if (wifi_sta_wait_connected(PROV_WIFI_TIMEOUT_MS)) {
        /* 成功: 保存凭据并反馈; 不重启, 主状态机检测到 IP 后自动走 MQTT */
        storage_set_wifi(s_prov_ssid, s_prov_pass);
        ble_prov_notify_status("connected");
        ESP_LOGI(TAG, "Provision OK: SSID=%s saved to NVS", s_prov_ssid);
    } else {
        /* 失败: 停止自动重连, 按断开原因反馈; 不保存 NVS, BLE 继续广播等重试 */
        wifi_sta_force_disconnect();
        uint8_t reason = wifi_sta_get_last_disconnect_reason();
        const char *status = (reason == WIFI_REASON_NO_AP_FOUND) ? "not_found" : "failed";
        ble_prov_notify_status(status);
        ESP_LOGW(TAG, "Provision failed (reason=%u) -> %s", reason, status);
    }
    s_prov_busy = false;
    vTaskDelete(NULL);
}

/* 配网完成回调 (BLE): 起独立任务连接 WiFi, 避免阻塞 NimBLE host task */
static void on_provision_done(const char *ssid, const char *password)
{
    if (!ssid || !ssid[0]) {
        ESP_LOGE(TAG, "Provision callback with empty SSID, ignored");
        return;
    }
    if (s_prov_busy) {
        ESP_LOGW(TAG, "Provision in progress, ignore new credentials");
        return;
    }
    s_prov_busy = true;
    strlcpy(s_prov_ssid, ssid, sizeof(s_prov_ssid));
    strlcpy(s_prov_pass, password ? password : "", sizeof(s_prov_pass));
    if (xTaskCreate(prov_connect_task, "prov_wifi", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "prov_wifi task create failed");
        s_prov_busy = false;
    }
}

static void setup_wifi(void)
{
    char ssid[33] = {0};
    char pass[65] = {0};

    if (storage_has_wifi_config()) {
        storage_get_wifi(ssid, sizeof(ssid), pass, sizeof(pass));
        ESP_LOGI(TAG, "WiFi from NVS: %s", ssid);
    } else if (WIFI_DEFAULT_SSID[0]) {
        snprintf(ssid, sizeof(ssid), "%s", WIFI_DEFAULT_SSID);
        snprintf(pass, sizeof(pass), "%s", WIFI_DEFAULT_PASS);
        ESP_LOGI(TAG, "WiFi from config: %s", ssid);
    } else {
        ESP_LOGW(TAG, "No WiFi config (NVS/Kconfig); waiting for config");
        return;
    }
    wifi_sta_connect(ssid, pass);
}

static void main_loop_task(void *arg)
{
    (void)arg;
    esp_task_wdt_add(xTaskGetCurrentTaskHandle());

    for (;;) {
        switch (s_state) {
        case STATE_INIT:
            if (wifi_sta_is_connected()) {
                enter_state(STATE_MQTT_CONNECTING);
            } else {
                enter_state(STATE_STA_CONNECTING);
            }
            break;

        case STATE_STA_CONNECTING:
            if (wifi_sta_is_connected()) {
                enter_state(STATE_MQTT_CONNECTING);
            } else if (now_ms() - s_state_enter_ms > WIFI_CONNECT_TIMEOUT_MS) {
                /* 超时: 有配置则重试连接; 无配置则静默等待 BLE 配网 */
                if (storage_has_wifi_config() || WIFI_DEFAULT_SSID[0]) {
                    ESP_LOGW(TAG, "WiFi connect timeout, retrying");
                    setup_wifi();
                }
                s_state_enter_ms = now_ms();   /* 重置计时器, 避免刷屏 */
            }
            break;

        case STATE_MQTT_CONNECTING:
            if (!wifi_sta_is_connected()) {
                enter_state(STATE_STA_CONNECTING);
                break;
            }
            mqtt_connect();
            if (mqtt_is_connected()) {
                enter_state(STATE_RUNNING);
            }
            break;

        case STATE_RUNNING:
            if (!wifi_sta_is_connected()) {
                enter_state(STATE_STA_CONNECTING);
                break;
            }
            if (!mqtt_is_connected()) {
                enter_state(STATE_MQTT_CONNECTING);
                break;
            }
            break;

        default:
            break;
        }

        /* 周期任务 (ARM 轮询始终运行, 与 MQTT 状态无关) */
        arm_poll_tick();
        telemetry_tick();
        sntp_sync_tick();
        offline_cache_process();

        /* OTA 周期处理 + 主循环存活标记 (boot 健康检查/回滚用) */
        mqtt_ota_process();
        ota_service_note_main_loop_alive();

        /* MQTT 重连 + 定期触发补传 */
        if (s_state == STATE_MQTT_CONNECTING || s_state == STATE_RUNNING) {
            mqtt_connect();
        }
        if (s_state == STATE_RUNNING &&
            now_ms() - s_last_flush_ms > 10000) {
            s_last_flush_ms = now_ms();
            mqtt_flush_offline_cache();
        }

        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_INFO);
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, " CS-L10-6K2 Collector (ESP-IDF %s)", FIRMWARE_VERSION);
    ESP_LOGI(TAG, " ESP32-C3 + CS Protocol(UART1) + MQTT");
    ESP_LOGI(TAG, "========================================");

    if (!storage_init()) {
        ESP_LOGE(TAG, "NVS init failed; abort");
        return;
    }

    ensure_sn();
    ensure_key();

    /* ARM 串口轮询 / OTA / 遥测 / 命令 / MQTT (不依赖 WiFi 状态, 先就绪) */
    arm_poll_init();
    arm_ota_init();
    telemetry_init();
    cmd_handler_init();
    if (!mqtt_init(cmd_handler_on_mqtt_message)) {
        ESP_LOGE(TAG, "MQTT init failed (SN/Key missing)");
    }
    if (!ota_service_init()) {
        ESP_LOGE(TAG, "OTA service init failed; OTA disabled");
    }
    mqtt_ota_init();

    offline_cache_init();
    wifi_sta_init();

    /* 配网: BLE 常驻 (无论有无配置) */
    ble_prov_set_callback(on_provision_done);
    if (ble_prov_start(g_device_sn, FIRMWARE_VERSION)) {
        ESP_LOGI(TAG, "BLE provisioning advertising: %s",
                 ble_prov_get_service_name());
    } else {
        ESP_LOGE(TAG, "BLE provisioning start failed");
    }

    setup_wifi();
    sntp_sync_init();

    enter_state(STATE_INIT);

    if (xTaskCreate(main_loop_task, "main_loop", 6144, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "main loop task creation failed");
        return;
    }
    if (!ota_service_start_boot_health_check()) {
        ESP_LOGE(TAG, "OTA boot health check task creation failed");
    }
    ESP_LOGI(TAG, "Setup done");
}
