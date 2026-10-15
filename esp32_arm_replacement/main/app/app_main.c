/*
 * app_main.c — ESP32-C3 direct DSP/BMS controller prototype
 *
 * 初始化顺序:
 *   NVS → SN/Key → direct DSP/BMS polling → MQTT+ESP OTA → offline cache
 *   → WiFi STA → BLE 配网(常驻) → SNTP → 主循环状态机
 *
 * 主循环 (100ms):
 *   arm_poll_tick / telemetry_tick / sntp_sync_tick / offline_cache_process
 *   / mqtt 重连 / 离线补传 / ota 周期处理
 */
#include <string.h>
#include <stdatomic.h>
#include <stdio.h>
#include "config.h"
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

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* This device-specific build owns its SN; previous NVS/ARM identity cannot
 * replace it. Only commit when necessary to avoid writes on every boot. */
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
static bool s_prov_busy = false;
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
    esp_task_wdt_add(xTaskGetCurrentTaskHandle());

    for (;;) {
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
        telemetry_tick();
        debug_log_tick();      /* 调试模式生效时把串口日志批量上行 */
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

void app_main(void)
{
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

    offline_cache_init();
    wifi_sta_init();
    setup_wifi();
    /* Allocate MQTT's full 8KB task stack before BLE fragments the heap.
     * esp-mqtt owns transport reconnects while WiFi obtains its address. */
    mqtt_connect();

    /* CSIV-CT 本地模式服务（GATT 服务通过 pre-host-start 回调注册） */
    if (ble_ct_start(g_device_sn, FIRMWARE_VERSION)) {
        ESP_LOGI(TAG, "BLE CT service initialized");
    } else {
        ESP_LOGE(TAG, "BLE CT service init failed");
    }
    /* 注入 BLE COMMAND 执行器：复用 cmd_handler 命令映射（设备控制 + 参数调整） */
    ble_ct_cmd_set_executors(ble_exec_write_param, ble_exec_read_param);
    if (!ble_ota_init()) {
        ESP_LOGE(TAG, "BLE OTA adapter init failed");
    }

    /* 配网: BLE 常驻 (无论有无配置) */
    ble_prov_set_callback(on_provision_done);
    ble_prov_set_gap_event_forward(ble_ct_on_gap_event);
    ble_prov_set_pre_host_start_cb(ble_ct_register_services);
    if (ble_prov_start(g_device_sn, FIRMWARE_VERSION)) {
        ESP_LOGI(TAG, "BLE provisioning advertising: %s",
                 ble_prov_get_service_name());
    } else {
        ESP_LOGE(TAG, "BLE provisioning start failed");
    }

    sntp_sync_init();

    enter_state(STATE_INIT);

    if (xTaskCreate(main_loop_task, "main_loop", 6144, NULL, 5, NULL) != pdPASS) {
        atomic_store(&s_boot_stage, BOOT_FAILED);
        ESP_LOGE(TAG, "main loop task creation failed");
        return;
    }
    if (!ota_service_start_boot_health_check()) {
        ESP_LOGE(TAG, "OTA boot health check task creation failed");
    }
    atomic_store(&s_boot_stage, BOOT_READY);
    ESP_LOGI(TAG, "Setup done");
}
