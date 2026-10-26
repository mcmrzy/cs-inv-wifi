/*
 * app_main.c — ESP32-C3 direct DSP/BMS controller prototype
 *
 * 初始化顺序:
 *   NVS → SN/Key → direct DSP/BMS polling → MQTT+ESP OTA → offline cache
 *   → reserve main loop → LCD first frame → WiFi STA → SNTP → boot health → MQTT/BLE 共存
 *
 * 主循环 (100ms):
 *   arm_poll_tick / telemetry_tick / sntp_sync_tick / offline_cache_process
 *   / mqtt 重连 / 离线补传 / ota 周期处理
 */
#include <string.h>
#include <stdatomic.h>
#include <stdio.h>
#include "config.h"
#include "app_network.h"
#include "nvs_storage.h"
#include "factory_identity.h"
#include "factory_service.h"
#include "wifi_sta.h"
#include "esp_wifi.h"
#include "arm_poll.h"
#include "direct_panel.h"
#include "direct_board_pins.h"
#include "telemetry.h"
#include "cmd_handler.h"
#include "mqtt_handler.h"
#include "cloud_binding.h"
#include "offline_cache.h"
#include "sntp_sync.h"
#include "ble_prov.h"
#include "ble_ct.h"
#include "ble_ct_cmd.h"
#include "ota_service.h"
#include "ble_ota.h"
#include "arm_ota.h"
#include "debug_log.h"
#include "ota_mqtt_adapter.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_task_wdt.h"
#include "esp_system.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "app";

typedef enum {
    BOOT_BUZZ, BOOT_NVS, BOOT_PINS, BOOT_DSP_BMS,
    BOOT_LCD, BOOT_NETWORK, BOOT_READY, BOOT_FAILED
} direct_boot_stage_t;
static atomic_int s_boot_stage;
#define LCD_FIRST_UI_WAIT_MS 5000U

#if CONFIG_DIRECT_BOOT_DIAGNOSTICS
static void boot_diagnostic_task(void *arg)
{
    (void)arg;
    static const char *const names[] = {
        "GPIO8_INIT", "NVS", "PIN_CHECK",
        "DSP_BMS", "LCD", "NETWORK", "READY", "FAILED"
    };
    for (;;) {
        const int stage = atomic_load(&s_boot_stage);
        /* Bypass the debug-log hook to also detect a logging-hook fault. */
        printf("[BOOT_DIAG] uptime_ms=%llu reset=%d stage=%s heap=%lu\n",
               (unsigned long long)(esp_timer_get_time() / 1000),
               (int)esp_reset_reason(), names[stage],
               (unsigned long)esp_get_free_heap_size());
        fflush(stdout);
        /* Keep stalled-startup evidence, but return this task's stack to the
         * idle task for reclamation once startup reaches a terminal stage. */
        if (stage == BOOT_READY || stage == BOOT_FAILED) {
            vTaskDelete(NULL);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
#endif

/* GPIO8 is both the board BUZZ net and an ESP32-C3 strapping pin.  The
 * original ARM implementation drives the buzzer high, so low is the safe
 * idle level.  Set the output latch before enabling output to avoid a visible
 * active glitch when the application starts. */
static void direct_buzzer_safe_init(void)
{
#if CONFIG_DIRECT_BUZZ_PIN >= 0
    const int pin = CONFIG_DIRECT_BUZZ_PIN;
    const int off_level = CONFIG_DIRECT_BUZZ_ACTIVE_LEVEL ? 0 : 1;
    gpio_set_level(pin, off_level);
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
    gpio_set_level(pin, off_level);
#endif
}

/* U30 remains powered and connected to BMS even though CAN is unused.
 * D high releases the CAN bus; LCD CS is now strapped low independently. */
static void direct_can_idle_init(void)
{
#if CONFIG_DIRECT_CAN_IDLE_TX_PIN >= 0
    const int pin = CONFIG_DIRECT_CAN_IDLE_TX_PIN;
    ESP_ERROR_CHECK(gpio_set_level(pin, 1));
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
    ESP_ERROR_CHECK(gpio_set_level(pin, 1));
    ESP_LOGI(TAG, "CAN_IDLE_HIGH: GPIO%d; LCD CS external low", pin);
#endif
}

static bool direct_board_pins_valid(void)
{
    const struct { int gpio; direct_pin_role_t role; } pins[] = {
        {CONFIG_DIRECT_DSP_TX_PIN, DIRECT_PIN_OTHER},
        {CONFIG_DIRECT_DSP_RX_PIN, DIRECT_PIN_OTHER},
        {CONFIG_DIRECT_BMS_TX_PIN, DIRECT_PIN_OTHER},
        {CONFIG_DIRECT_BMS_RX_PIN, DIRECT_PIN_OTHER},
        {CONFIG_DIRECT_BMS_RS485_DE_PIN, DIRECT_PIN_OTHER},
        {CONFIG_DIRECT_CAN_IDLE_TX_PIN, DIRECT_PIN_OTHER},
        {CONFIG_DIRECT_LCD_SCLK_PIN, DIRECT_PIN_OTHER},
        {CONFIG_DIRECT_LCD_MOSI_PIN, DIRECT_PIN_OTHER},
        {CONFIG_DIRECT_LCD_CS_PIN, DIRECT_PIN_OTHER},
#if CONFIG_DIRECT_LCD_4WIRE
        {CONFIG_DIRECT_LCD_DC_PIN, DIRECT_PIN_LCD_DC},
#else
        {CONFIG_DIRECT_LCD_DC_PIN, DIRECT_PIN_OTHER},
#endif
        {CONFIG_DIRECT_LCD_RST_PIN, DIRECT_PIN_OTHER},
        {CONFIG_DIRECT_LCD_BL_PIN, DIRECT_PIN_OTHER},
        {CONFIG_DIRECT_KEY_ROW_UP_PIN, DIRECT_PIN_OTHER},
        {CONFIG_DIRECT_KEY_ROW_DOWN_PIN, DIRECT_PIN_OTHER},
        {CONFIG_DIRECT_KEY_ESC_PIN, DIRECT_PIN_OTHER},
        {CONFIG_DIRECT_KEY_ENTER_PIN, DIRECT_PIN_OTHER},
#if CONFIG_DIRECT_KEY_BS818A
        {CONFIG_DIRECT_KEY_CLOCK_PIN, DIRECT_PIN_KEY_CLOCK},
#else
        {CONFIG_DIRECT_KEY_CLOCK_PIN, DIRECT_PIN_OTHER},
#endif
        {CONFIG_DIRECT_KEY_DATA_PIN, DIRECT_PIN_OTHER},
    };
    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); ++i) {
        int pin = pins[i].gpio;
        if (pin < 0) continue;
        if (!direct_board_gpio_allowed(pin, pins[i].role)) {
            ESP_LOGE(TAG, "unsafe board GPIO %d; direct peripherals disabled", pin);
            return false;
        }
        for (size_t j = 0; j < i; ++j) {
            if (pin == pins[j].gpio) {
                ESP_LOGE(TAG, "duplicate board GPIO %d; direct peripherals disabled", pin);
                return false;
            }
        }
    }
    return true;
}

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
static TaskHandle_t s_main_loop_handle;
static TaskHandle_t s_startup_owner;
static atomic_bool s_startup_health_done;
static bool s_ble_services_started;
static uint32_t s_startup_release_ms;
static uint32_t s_ble_start_deadline_ms;
static uint32_t s_cloud_retry_deadline_ms;
static uint32_t s_next_ble_start_ms;
static bool s_ble_suspended_for_mqtt;
static atomic_bool s_local_ble_requested;
static bool s_local_ble_mode;
static uint32_t s_local_ble_idle_deadline_ms;

#define MAIN_LOOP_STACK_SIZE 6144U
#define STARTUP_STACK_SETTLE_MS 100U
#define BLE_STARTUP_WAIT_MS 10000U
#define BLE_START_RETRY_MS 5000U
#define CLOUD_RETRY_WINDOW_MS 30000U
#define LOCAL_BLE_IDLE_MS 15000U
static uint32_t s_next_coexist_log_ms;
static void startup_deferred_tick(void);

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* Persisted factory identity survives maintenance images. A blank Device build
 * keeps the development fallback; Factory/Production never invent an SN. */
static void ensure_sn(void)
{
#if CONFIG_DIRECT_IDENTITY_PRODUCTION
    factory_record_t identity;
    if(factory_identity_read(&identity)==FACTORY_OK){snprintf(g_device_sn,sizeof(g_device_sn),"%s",identity.sn);}
    else {g_device_sn[0]=0;ESP_LOGW(TAG,"FACTORY_REQUIRED: no valid per-device identity, network disabled");}
#else
    _Static_assert(sizeof(DEVICE_FIXED_SN) <= SN_MAX_LEN, "Fixed SN too long");
    if(factory_identity_admin_sn(g_device_sn,sizeof(g_device_sn))){
        ESP_LOGI(TAG,"SN loaded from authenticated local override (ARM sync disabled)");return;
    }
    factory_record_t identity;
    if(factory_identity_read(&identity)==FACTORY_OK){
        snprintf(g_device_sn,sizeof(g_device_sn),"%s",identity.sn);
        ESP_LOGI(TAG,"SN loaded from preserved factory identity");return;
    }
    char stored[SN_MAX_LEN] = {0};
    snprintf(g_device_sn, sizeof(g_device_sn), "%s", DEVICE_FIXED_SN);
    if (!storage_get_sn(stored, sizeof(stored)) || strcmp(stored, g_device_sn) != 0) {
        if (!storage_set_sn(g_device_sn)) {
            ESP_LOGE(TAG, "Fixed SN persistence failed; runtime identity retained");
        }
    }
    ESP_LOGI(TAG, "SN fixed locally: %s (ARM sync disabled)", g_device_sn);
#endif
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
#define PROV_WIFI_TIMEOUT_MS   30000
static atomic_bool s_prov_busy;
static char s_prov_ssid[33];
static char s_prov_pass[65];

/* 配网连接任务: 连接 WiFi -> 按结果通知 App (connected/failed/not_found), 成功才保存 NVS */
static void prov_connect_task(void *arg)
{
    (void)arg;
    if (!wifi_sta_connect(s_prov_ssid, s_prov_pass)) {
        /* 立即失败 (配置无效/链路忙): 直接反馈, 不空等超时 */
        uint8_t reason = wifi_sta_get_last_disconnect_reason();
        const char *status =
            (reason == WIFI_REASON_NO_AP_FOUND) ? "not_found" : "failed";
        ble_prov_notify_status(status);
        ESP_LOGW(TAG, "Provision connect failed (reason=%u) -> %s",
                 reason, status);
        s_prov_busy = false;
        vTaskDelete(NULL);
        return;
    }
    if (wifi_sta_wait_connected(PROV_WIFI_TIMEOUT_MS)) {
        /* 成功: 保存凭据并反馈; 不重启, 主状态机检测到 IP 后自动走 MQTT.
           断开自动重连是 esp_wifi 默认行为, 无需显式开启 */
        /* 返回值必须检查: 写失败(典型是 NVS 分区满/碎片)时若仍回 connected,
         * App 会以为配网成功, 而设备重启后就"忘"了凭据 —— 现场表现为
         * "每次重启配网都丢"。失败时回 failed (App 认识的既有状态), 并打日志。 */
        if (!storage_set_wifi(s_prov_ssid, s_prov_pass)) {
            ESP_LOGE(TAG, "Provision connect OK but NVS save FAILED (SSID=%s);"
                     " credentials will be lost on reboot", s_prov_ssid);
            ble_prov_notify_status("failed");
        } else {
            ble_prov_notify_status("connected");
            ESP_LOGI(TAG, "Provision OK: SSID=%s saved to NVS (pass_len=%d)",
                     s_prov_ssid, (int)strlen(s_prov_pass));
        }
    } else {
        /* 失败: 主动断开 (esp_wifi_disconnect 为 ASSOC_LEAVE, 内部不自动重连),
           按原因反馈; 不保存 NVS, BLE 继续广播等重试 */
        wifi_sta_force_disconnect();
        uint8_t reason = wifi_sta_get_last_disconnect_reason();
        const char *status =
            (reason == WIFI_REASON_NO_AP_FOUND) ? "not_found" : "failed";
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
    if (xTaskCreate(prov_connect_task, "prov_wifi", 6144, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "prov_wifi task create failed");
        s_prov_busy = false;
        ble_prov_notify_status("failed");   /* 让 App 立即收到失败, 避免干等超时 */
    }
}

static void setup_wifi(void)
{
    char ssid[33] = {0};
    char pass[65] = {0};

    if (storage_has_wifi_config()) {
        if(!storage_get_wifi(ssid, sizeof(ssid), pass, sizeof(pass))){
            ESP_LOGW(TAG,"WiFi credentials could not be read from NVS; waiting for config");
            return;
        }
        ESP_LOGW(TAG, "WiFi from NVS: %s (pass_len=%d)", ssid, (int)strlen(pass));
    } else if (WIFI_DEFAULT_SSID[0]) {
        snprintf(ssid, sizeof(ssid), "%s", WIFI_DEFAULT_SSID);
        snprintf(pass, sizeof(pass), "%s", WIFI_DEFAULT_PASS);
        ESP_LOGW(TAG, "WiFi from config: %s", ssid);
    } else {
        ESP_LOGW(TAG, "No WiFi config (NVS/Kconfig); waiting for config");
        return;
    }
    if(!wifi_sta_connect(ssid, pass))ESP_LOGW(TAG,"WiFi connect request rejected; credentials retained");
}

static void main_loop_task(void *arg)
{
    (void)arg;
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    /* The startup owner may wait for the LCD before enabling radio tasks. */
    esp_task_wdt_add(xTaskGetCurrentTaskHandle());

    for (;;) {
        startup_deferred_tick();
        direct_panel_diagnostic_tick();
        /* Device identity is fixed locally in this build. */
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
            } else if (!s_prov_busy &&
                       now_ms() - s_state_enter_ms > WIFI_CONNECT_TIMEOUT_MS) {
                /* 超时: 有配置则重试连接; 无配置则静默等待 BLE 配网;
                   配网进行中 (s_prov_busy) 暂停重连, 避免旧配置覆盖新凭据 */
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
            if (mqtt_is_stuck(MQTT_STUCK_TIMEOUT_MS)) {
                mqtt_force_restart();
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
                if (mqtt_is_stuck(MQTT_STUCK_TIMEOUT_MS)) {
                    mqtt_force_restart();
                }
                enter_state(STATE_MQTT_CONNECTING);
                break;
            }
            break;

        default:
            break;
        }

        /* 周期任务 (ARM 轮询始终运行, 与 MQTT 状态无关) */
        arm_poll_tick();
        cmd_handler_tick();
        ble_ct_cmd_tick();
        telemetry_tick();
        if (!ota_service_is_busy()) debug_log_tick(); /* Defer bulk logs during OTA. */
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

/* ---- BLE COMMAND 执行器（main 层实现, 注入 ble_ct_cmd） ----
 * 复用 cmd_handler 的 s_cmd_map（参数名/地址/缩放单一来源），
 * 使 BLE 控制通道与云端 MQTT 命令行为一致。
 * 返回值与 ble_ct_cmd.h 的 BLE_CMD_* 保持同序。 */
static int ble_exec_write_param(const char *param_id, double value)
{
    return (int)cmd_handler_write_param(param_id, value);
}

static int ble_exec_read_param(const char *param_id, double *out)
{
    return cmd_handler_get_param(param_id, out) ? BLE_CMD_OK : BLE_CMD_NOT_READABLE;
}

static int ble_exec_write_battery_config(const struct cJSON *config, bool validate_only)
{
    return cmd_handler_write_battery_config(config, validate_only);
}

static struct cJSON *ble_exec_read_battery_config(void)
{
    return cmd_handler_get_battery_config();
}

static bool start_ble_services(void)
{
    ESP_LOGW(TAG, "BLE start memory: free=%u largest=%u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    if (!ble_ct_start(g_device_sn, FIRMWARE_VERSION)) {
        ESP_LOGE(TAG, "BLE CT service init failed");
        return false;
    }
    ble_ct_cmd_set_executors(ble_exec_write_param, ble_exec_read_param);
    ble_ct_cmd_set_battery_executors(ble_exec_write_battery_config, ble_exec_read_battery_config);
    if (!ble_ota_init()) {
        ESP_LOGE(TAG, "BLE OTA adapter init failed");
        return false;
    }
    ble_prov_set_callback(on_provision_done);
    ble_prov_set_gap_event_forward(ble_ct_on_gap_event);
    ble_prov_set_pre_host_start_cb(ble_ct_register_services);
    if (!ble_prov_start(g_device_sn, FIRMWARE_VERSION)) {
        ESP_LOGE(TAG, "BLE provisioning start failed");
        return false;
    }
    ESP_LOGI(TAG, "BLE host startup accepted (actual readiness pending): %s",
             ble_prov_get_service_name());
    return true;
}

void app_request_local_ble(bool requested)
{
    atomic_store(&s_local_ble_requested, requested);
}

/* Normal operation keeps both transports resident. An idle host is reclaimed
 * for reconnect handshakes; active phones are never forcibly disconnected.
 * Exclusive local mode also provides bounded offline and low-memory recovery. */
static void local_ble_mode_tick(uint32_t now)
{
    const bool requested = atomic_load(&s_local_ble_requested) ||
        (!storage_has_wifi_config() && !WIFI_DEFAULT_SSID[0]);
    const bool recovery_window = !mqtt_is_connected() &&
        (int32_t)(now - s_cloud_retry_deadline_ms) >= 0;
    const unsigned caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const bool local_fallback = requested &&
        (!s_ble_services_started || ble_prov_start_failed()) &&
        (ble_prov_start_failed() || heap_caps_get_free_size(caps) < 65536U ||
         heap_caps_get_largest_free_block(caps) < 32768U);
    if (!s_local_ble_mode && mqtt_is_connected() && !local_fallback) {
        s_cloud_retry_deadline_ms = now + CLOUD_RETRY_WINDOW_MS;
        mqtt_set_connect_allowed(true);
        if (ble_prov_is_ready() && (int32_t)(now - s_next_coexist_log_ms) >= 0) {
            s_next_coexist_log_ms = now + 30000U;
            ESP_LOGW(TAG, "MQTT+BLE resident: free=%u largest=%u",
                     (unsigned)heap_caps_get_free_size(caps),
                     (unsigned)heap_caps_get_largest_free_block(caps));
        }
        if (!ota_service_admin_begin()) return;
        if (ble_prov_is_suspended() && !s_ble_suspended_for_mqtt) {
            /* A failed OTA restore retains its owner. No OTA is active under
             * this admin guard; retry that owner's restore, not generic start. */
            if (!ble_prov_restore_after_ota())
                s_next_ble_start_ms = now + BLE_START_RETRY_MS;
        } else if (ble_prov_start_failed() && !s_ble_suspended_for_mqtt) {
            if (ble_prov_recover_start_failure()) s_ble_services_started = false;
            s_next_ble_start_ms = now + BLE_START_RETRY_MS;
        } else {
            bool accepted = true;
            if (s_ble_suspended_for_mqtt) {
                accepted = ble_prov_restore_after_mqtt();
                if (accepted) s_ble_suspended_for_mqtt = false;
            } else if (!s_ble_services_started) {
                accepted = start_ble_services();
                if (accepted) s_ble_services_started = true;
            }
            if (!accepted) s_next_ble_start_ms = now + BLE_START_RETRY_MS;
        }
        ota_service_admin_end();
        return;
    }
    if (!s_local_ble_mode && !requested && !recovery_window) {
        /* The menu can close while an old PUBACK worker is draining. Cancel
         * that pending pause without leaving the cloud permanently gated. */
        if (mqtt_is_paused_for_local()) {
            if (!ota_service_admin_begin()) return;
            const bool resumed = mqtt_resume_after_local();
            ota_service_admin_end();
            if (!resumed) return;
        }
        if (s_ble_services_started && !s_ble_suspended_for_mqtt) {
            if (ble_prov_is_connected() || atomic_load(&s_prov_busy)) {
                const unsigned caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
                mqtt_set_connect_allowed(heap_caps_get_free_size(caps) >= 32768U &&
                    heap_caps_get_largest_free_block(caps) >= 16384U);
                return;
            }
            if (!ota_service_admin_begin()) return;
            if (ble_prov_suspend_for_mqtt()) {
                s_ble_suspended_for_mqtt = true;
                s_next_ble_start_ms = now + STARTUP_STACK_SETTLE_MS;
            } else {
                ble_prov_restore_after_mqtt();
                s_next_ble_start_ms = now + BLE_START_RETRY_MS;
            }
            ota_service_admin_end();
            return;
        }
        mqtt_set_connect_allowed(true);
        return;
    }
    if (!s_local_ble_mode) {
        if (!atomic_load(&s_local_ble_requested) &&
            (int32_t)(now - s_ble_start_deadline_ms) < 0) return;
        if (!ota_service_admin_begin()) return;
        const bool paused = mqtt_pause_for_local();
        ota_service_admin_end();
        if (!paused) return; /* A pending PUBACK worker still owns the client. */
        s_local_ble_mode = true;
        s_local_ble_idle_deadline_ms = now + LOCAL_BLE_IDLE_MS;
        s_next_ble_start_ms = now + STARTUP_STACK_SETTLE_MS;
        ESP_LOGW(TAG, "Local BLE mode: MQTT released; free=%u largest=%u",
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        return;
    }
    const bool occupied = requested || atomic_load(&s_prov_busy) ||
                          ble_prov_is_connected();
    if (occupied) s_local_ble_idle_deadline_ms = now + LOCAL_BLE_IDLE_MS;
    if (ble_prov_is_suspended() && !s_ble_suspended_for_mqtt) {
        if (!ota_service_admin_begin()) return;
        const bool restored = ble_prov_restore_after_ota();
        ota_service_admin_end();
        s_next_ble_start_ms = now + (restored ? STARTUP_STACK_SETTLE_MS : BLE_START_RETRY_MS);
        return;
    }
    if (ble_prov_start_failed() && !s_ble_suspended_for_mqtt) {
        if (!ota_service_admin_begin()) return;
        if (ble_prov_recover_start_failure()) s_ble_services_started = false;
        ota_service_admin_end();
        s_next_ble_start_ms = now + BLE_START_RETRY_MS;
        return;
    }
    if (!occupied && (int32_t)(now - s_local_ble_idle_deadline_ms) >= 0) {
        if (!ota_service_admin_begin()) return;
        if (!s_ble_suspended_for_mqtt) {
            if (!ble_prov_suspend_for_mqtt()) {
                /* A phone may have connected after our idle check. The BLE
                 * helper rejects that race; recover only our own failed pause. */
                ble_prov_restore_after_mqtt();
                s_next_ble_start_ms = now + BLE_START_RETRY_MS;
                ota_service_admin_end();
                return;
            }
            s_ble_suspended_for_mqtt = true;
            s_next_ble_start_ms = now + STARTUP_STACK_SETTLE_MS;
            ota_service_admin_end();
            return;
        }
        const bool resumed = mqtt_resume_after_local();
        ota_service_admin_end();
        if (!resumed) {
            s_next_ble_start_ms = now + BLE_START_RETRY_MS;
            return;
        }
        s_local_ble_mode = false;
        s_cloud_retry_deadline_ms = now + CLOUD_RETRY_WINDOW_MS;
        mqtt_set_connect_allowed(true);
        ESP_LOGW(TAG, "MQTT reconnect enabled; offline BLE recovery in %u ms",
                 CLOUD_RETRY_WINDOW_MS);
        return;
    }
    if (!ota_service_admin_begin()) return;
    bool accepted = true;
    if (s_ble_suspended_for_mqtt) {
        accepted = ble_prov_restore_after_mqtt();
        if (accepted) s_ble_suspended_for_mqtt = false;
    } else if (!s_ble_services_started) {
        accepted = start_ble_services();
        if (accepted) s_ble_services_started = true;
    }
    ota_service_admin_end();
    if (!accepted || ble_prov_start_failed()) {
        ESP_LOGW(TAG, "Local BLE unavailable; retrying with MQTT still released");
        s_next_ble_start_ms = now + BLE_START_RETRY_MS;
    }
}

/* Only the main loop owns lifecycle changes; LCD/GAP callbacks publish intent.
 * SDK auto-reconnect stays disabled so no TLS attempt bypasses local mode. */
static void startup_deferred_tick(void)
{
    const uint32_t now = now_ms();
    mqtt_poll_connection();
    mqtt_set_connect_allowed(false);
    if (s_startup_owner && atomic_load(&s_startup_health_done) &&
        (int32_t)(now - s_startup_release_ms) >= 0) {
        TaskHandle_t owner = s_startup_owner;
        s_startup_owner = NULL;
        vTaskDelete(owner);
        s_next_ble_start_ms = now + STARTUP_STACK_SETTLE_MS;
        ESP_LOGW(TAG, "Startup stack released; free=%u largest=%u",
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    }
    if (s_startup_owner ||
        (int32_t)(now - s_next_ble_start_ms) < 0) {
        return;
    }
    if (ota_service_is_busy()) {
        /* Freeze mode/lifecycle changes, not an existing cloud control link.
         * Remote OTA may still need progress/cancel traffic after a drop. */
        const unsigned caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
        if (!s_local_ble_mode && !mqtt_is_paused_for_local() &&
            (!s_ble_services_started || ble_prov_is_suspended()) &&
            heap_caps_get_free_size(caps) >= 32768U &&
            heap_caps_get_largest_free_block(caps) >= 16384U)
            mqtt_set_connect_allowed(true);
        return;
    }
    /* Finish the admitted handshake before stopping/destroying its client. */
    if (mqtt_is_connecting()) return;
    local_ble_mode_tick(now);
}

static bool wait_for_first_ui_frame(void)
{
    const uint32_t started=now_ms();
    ESP_LOGW(TAG,"LCD_NETWORK_GATE waiting max_ms=%u; WiFi/BLE not started",LCD_FIRST_UI_WAIT_MS);
    for(;;){
        if(direct_panel_software_healthy()){
            ESP_LOGW(TAG,"LCD_NETWORK_GATE ready wait_ms=%lu; starting network",
                     (unsigned long)(now_ms()-started));
            return true;
        }
        if((uint32_t)(now_ms()-started)>=LCD_FIRST_UI_WAIT_MS){
            ESP_LOGW(TAG,"LCD_NETWORK_GATE timeout wait_ms=%lu; starting network for recovery access",
                     (unsigned long)(now_ms()-started));
            direct_panel_diagnostic_tick();
            return false;
        }
        direct_panel_diagnostic_tick();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void app_main(void)
{
#if CONFIG_DIRECT_LCD_ISOLATION_TEST
    /* Do not initialize NVS, Wi-Fi, BLE, MQTT, DSP/BMS, keys or normal UI. */
    direct_buzzer_safe_init();
    ESP_ERROR_CHECK(direct_board_pins_valid() ? ESP_OK : ESP_ERR_INVALID_ARG);
    direct_can_idle_init();
    direct_panel_prepare_boot();
    esp_log_level_set("*", ESP_LOG_INFO);
    ESP_LOGW(TAG, "LCD_ISOLATION %s reset=%d WiFi=OFF BLE=OFF MQTT=OFF SPI=20000000",
             FIRMWARE_VERSION, (int)esp_reset_reason());
    direct_panel_init();
    return;
#endif
    direct_buzzer_safe_init();
    atomic_store(&s_boot_stage,BOOT_BUZZ);
    if (direct_board_pins_valid()) {
        direct_can_idle_init();
#if CONFIG_DIRECT_LCD_NOCS_BOOT
        direct_panel_prepare_boot();
#endif
    }
#if CONFIG_DIRECT_BOOT_DIAGNOSTICS
    setvbuf(stdout, NULL, _IONBF, 0);
    if (xTaskCreate(boot_diagnostic_task, "boot_diag", 3072, NULL, 2, NULL) != pdPASS)
        ESP_LOGE(TAG, "boot diagnostic task creation failed");
#endif
#if CONFIG_DIRECT_BOOT_DIAGNOSTICS
    esp_log_level_set("*", ESP_LOG_INFO);
#else
    esp_log_level_set("*", ESP_LOG_WARN);
#endif
    atomic_store(&s_boot_stage, BOOT_BUZZ);
    ESP_LOGI(TAG, "GPIO8 buzzer disabled at application entry; USB wait=0ms");
    /* BLE 配对诊断: 放开 NimBLE 协议栈日志 (需 CONFIG_BT_NIMBLE_LOG_LEVEL=DEBUG 编译) */
    esp_log_level_set("NimBLE", ESP_LOG_WARN);
    debug_log_init();   /* 尽早装钩子, 让后续启动日志也进调试缓冲 */
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, " CS-L10-6K2 direct controller (ESP-IDF %s)", FIRMWARE_VERSION);
    ESP_LOGI(TAG, " ESP32-C3 + DSP/BMS direct UART + MQTT");
    ESP_LOGI(TAG, "========================================");

    atomic_store(&s_boot_stage, BOOT_NVS);
    if (!storage_init()) {
        atomic_store(&s_boot_stage, BOOT_FAILED);
        ESP_LOGE(TAG, "NVS init failed; abort");
        return;
    }

    factory_result_t factory_init=factory_identity_init();
    ESP_LOGI(TAG,"Factory partition init: %s",factory_result_name(factory_init));
    ensure_sn();
    ensure_key();

    /* Direct DSP/BMS snapshots retain the L10 telemetry API for migration. */
    atomic_store(&s_boot_stage, BOOT_PINS);
    if (!direct_board_pins_valid()) {
        atomic_store(&s_boot_stage, BOOT_FAILED);
        ESP_LOGE(TAG, "board pin configuration invalid; abort startup");
        return;
    }
    atomic_store(&s_boot_stage, BOOT_DSP_BMS);
    arm_poll_init();
    atomic_store(&s_boot_stage, BOOT_LCD);
    if(!cloud_binding_start())ESP_LOGW(TAG,"Cloud binding state unavailable: configure server public key; local operation continues");
    direct_panel_init();
    atomic_store(&s_boot_stage, BOOT_NETWORK);
    if(!factory_service_start())ESP_LOGE(TAG,"Factory USB service failed");
    printf("[DEVICE] CS-L10-6K2 firmware=%s sn=%s reset=%d\n",FIRMWARE_VERSION,g_device_sn,(int)esp_reset_reason());
    if(esp_reset_reason()==ESP_RST_BROWNOUT)ESP_LOGW(TAG,"Previous reset: BROWNOUT; check controller 3.3V supply");
#if CONFIG_DIRECT_IDENTITY_PRODUCTION
    if(!g_device_sn[0]){atomic_store(&s_boot_stage,BOOT_READY);ESP_LOGW(TAG,"Waiting for USB provision, then reset device");return;}
#endif
    telemetry_init();
    cmd_handler_init();
    if (!mqtt_init(cmd_handler_on_mqtt_message)) {
        ESP_LOGE(TAG, "MQTT init failed (SN/Key missing)");
    }
    if (!ota_service_init()) {
        ESP_LOGE(TAG, "OTA service init failed; OTA disabled");
    }
    mqtt_ota_init();

    /* Reserve the permanent main-loop stack before Wi-Fi, MQTT and BLE each
     * take their own long-lived allocations. The task waits below until all
     * modules are initialized, so this changes allocation order only. */
    s_startup_owner = xTaskGetCurrentTaskHandle();
    atomic_store(&s_startup_health_done, false);
    s_startup_release_ms = 0;
    if (xTaskCreate(main_loop_task, "main_loop", MAIN_LOOP_STACK_SIZE,
                    NULL, 5, &s_main_loop_handle) != pdPASS) {
        atomic_store(&s_boot_stage, BOOT_FAILED);
        ESP_LOGE(TAG, "main loop task creation failed: free=%u largest=%u",
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        return;
    }

    offline_cache_init();
    /* UI snapshots need MQTT/OTA/storage ready, but no radio is required.
     * The main loop remains parked so it cannot start BLE during the gate. */
    (void)wait_for_first_ui_frame();
    s_ble_start_deadline_ms = now_ms() + BLE_STARTUP_WAIT_MS;
    s_cloud_retry_deadline_ms = now_ms() + CLOUD_RETRY_WINDOW_MS;
    s_next_ble_start_ms = now_ms();
    wifi_sta_init();
    setup_wifi();
    /* mqtt_init reserved stack headroom. Transport starts only after DHCP
     * and after startup_deferred_tick admits its exclusive memory window. */
    mqtt_connect();

    sntp_sync_init();

    enter_state(STATE_INIT);

    atomic_store(&s_boot_stage, BOOT_READY);
    ESP_LOGI(TAG, "Setup done");
    xTaskNotifyGive(s_main_loop_handle);
    /* Reuse the startup task's reserved stack for OTA confirmation, then hold
     * it briefly so the idle task can reclaim it before BLE allocates. */
    ota_service_start_boot_health_check();
    s_startup_release_ms = now_ms() + STARTUP_STACK_SETTLE_MS;
    atomic_store(&s_startup_health_done, true);
    vTaskSuspend(NULL);
}
