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
#include "panel_wifi.h"
#include "panel_weather.h"
#include "panel_ble_aux.h"
#include "nvs_storage.h"
#include "factory_identity.h"
#include "factory_service.h"
#include "wifi_sta.h"
#include "esp_wifi.h"
#include "arm_poll.h"
#include "direct_panel.h"
#include "full_board_pins.h"
#include "panel_indicators_hw.h"
#include "full_alarm_service.h"
#include "telemetry.h"
#include "cmd_handler.h"
#include "mqtt_handler.h"
#include "cloud_binding.h"
#include "offline_cache.h"
#include "sntp_sync.h"
#include "ble_prov.h"
#include "ble_ct.h"
#include "ble_ct_cmd.h"
#include "panel_ble.h"
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

/* Set the buzzer's idle latch before enabling the output. */
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
        {CONFIG_PANEL_LED_GPIO, DIRECT_PIN_LED_DATA},
        {CONFIG_DIRECT_BUZZ_PIN, DIRECT_PIN_OTHER},
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
        {CONFIG_DIRECT_KEY_DATA_PIN, DIRECT_PIN_KEY_DATA},
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
static atomic_bool s_ble_retry_requested;
static uint32_t s_ble_retry_after_ms;
static atomic_bool s_prov_busy;
static bool s_local_ble_mode;
static uint32_t s_local_ble_idle_deadline_ms;

#define MAIN_LOOP_STACK_SIZE 6144U
#define STARTUP_STACK_SETTLE_MS 100U
#define BLE_STARTUP_WAIT_MS 10000U
#define BLE_START_RETRY_MS 5000U
#define CLOUD_RETRY_WINDOW_MS 30000U
#define LOCAL_BLE_IDLE_MS 15000U
#define BLE_MEMORY_RETRY_MS 300000U
static uint32_t s_next_coexist_log_ms;
static void startup_deferred_tick(void);

static void indicators_tick(void)
{
    static bool cloud_seen;
    const bool configured=storage_has_wifi_config() || WIFI_DEFAULT_SSID[0];
    const bool wifi=wifi_sta_is_connected(),cloud=mqtt_is_connected();
    if(cloud)cloud_seen=true;
    const bool available=ble_prov_is_ready() && !ble_prov_is_suspended();
    const bool provisioning=available && (atomic_load(&s_prov_busy) ||
        atomic_load(&s_local_ble_requested) || s_local_ble_mode || !configured);
    const panel_led_network_t network={
        .enabled=configured || wifi || cloud || available,
        .local_warning=full_alarm_needs_attention(),
        .wifi=wifi,.cloud=cloud,.ble=ble_prov_is_connected(),.provisioning=provisioning,
        .reconnecting=configured && (!wifi || (cloud_seen && !cloud && !s_local_ble_mode))};
    full_indicators_tick(&network);
}

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* Persisted factory identity survives maintenance images. A blank Device build
 * keeps the development fallback; Factory/Production never invent an SN. */
static void ensure_sn(void)
{
    if(factory_identity_admin_sn(g_device_sn,sizeof(g_device_sn)))return;
    factory_record_t identity;
    if(factory_identity_read(&identity)==FACTORY_OK){snprintf(g_device_sn,sizeof(g_device_sn),"%s",identity.sn);return;}
#if CONFIG_DIRECT_IDENTITY_PRODUCTION
    g_device_sn[0]=0;
    ESP_LOGW(TAG,"FACTORY_REQUIRED: provision identity before network enrollment");
#else
    if(storage_get_sn(g_device_sn,sizeof(g_device_sn)) && strlen(g_device_sn)==16)return;
    uint8_t mac[6];
    if(esp_read_mac(mac,ESP_MAC_WIFI_STA)!=ESP_OK){g_device_sn[0]=0;return;}
    snprintf(g_device_sn,sizeof(g_device_sn),"MB00%02X%02X%02X%02X%02X%02X",mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    if(!storage_set_sn(g_device_sn))ESP_LOGE(TAG,"Development identity persistence failed");
#endif
}

static void ensure_key(void)
{
    if (storage_get_key(g_device_key, sizeof(g_device_key))) {
        ESP_LOGD(TAG, "Key loaded");
        return;
    }
    snprintf(g_device_key, sizeof(g_device_key), "%s", MQTT_PASSWORD);
    storage_set_key(g_device_key);
    ESP_LOGD(TAG, "Key set to default (MQTT password)");
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

#define PROV_WIFI_TIMEOUT_MS 30000U
static char s_prov_ssid[33],s_prov_pass[65];
static unsigned s_wifi_profile;
static atomic_uint s_wifi_attempt_ms;
static bool s_prov_panel;
static atomic_uint s_panel_wifi_status;
static panel_wifi_ap_t s_panel_wifi_aps[PANEL_WIFI_MAX_AP];
static unsigned s_panel_wifi_count;
static void setup_wifi(void);

static void provision_report(panel_wifi_status_t result)
{
    if(s_prov_panel)atomic_store(&s_panel_wifi_status,result);
    else ble_prov_notify_status(result==PANEL_WIFI_CONNECTED?"connected":
        result==PANEL_WIFI_NOT_FOUND?"not_found":"failed");
}

static void prov_connect_task(void *arg)
{
    (void)arg;
    panel_wifi_status_t result=PANEL_WIFI_FAILED;
    if(wifi_sta_connect(s_prov_ssid,s_prov_pass) && wifi_sta_wait_connected(PROV_WIFI_TIMEOUT_MS)){
        result=storage_set_wifi(s_prov_ssid,s_prov_pass)?PANEL_WIFI_CONNECTED:PANEL_WIFI_SAVE_FAILED;
        if(result==PANEL_WIFI_CONNECTED)s_wifi_profile=0;
    }else if(wifi_sta_get_last_disconnect_reason()==WIFI_REASON_NO_AP_FOUND)result=PANEL_WIFI_NOT_FOUND;
    if(result!=PANEL_WIFI_CONNECTED && (s_prov_panel || result!=PANEL_WIFI_SAVE_FAILED)){
        wifi_sta_force_disconnect();
        if(s_prov_panel)setup_wifi();
    }
    memset(s_prov_pass,0,sizeof(s_prov_pass));
    provision_report(result);
    atomic_store(&s_prov_busy,false);
    vTaskDelete(NULL);
}

static bool provision_start(const char *ssid,const char *password,bool panel)
{
    bool busy=false;
    if(!ssid || !*ssid || !atomic_compare_exchange_strong(&s_prov_busy,&busy,true))return false;
    s_prov_panel=panel;
    strlcpy(s_prov_ssid,ssid,sizeof(s_prov_ssid));
    strlcpy(s_prov_pass,password?password:"",sizeof(s_prov_pass));
    if(panel)atomic_store(&s_panel_wifi_status,PANEL_WIFI_CONNECTING);
    if(xTaskCreate(prov_connect_task,"prov_wifi",6144,NULL,5,NULL)!=pdPASS){
        memset(s_prov_pass,0,sizeof(s_prov_pass));
        provision_report(PANEL_WIFI_FAILED);
        atomic_store(&s_prov_busy,false);
        return false;
    }
    return true;
}

static void on_provision_done(const char *ssid,const char *password)
{
    provision_start(ssid,password,false);
}

panel_wifi_status_t panel_wifi_status(void)
{
    return (panel_wifi_status_t)atomic_load(&s_panel_wifi_status);
}

const panel_wifi_ap_t *panel_wifi_ap(unsigned index)
{
    return index<s_panel_wifi_count?&s_panel_wifi_aps[index]:NULL;
}

bool panel_wifi_connect(const char *ssid,const char *pass)
{
    return !ota_service_is_busy() && provision_start(ssid,pass,true);
}

static void panel_scan_task(void *arg)
{
    (void)arg;
    wifi_scan_config_t config={0};
    esp_err_t error=esp_wifi_scan_start(&config,true);
    bool restore=error==ESP_ERR_WIFI_STATE && !wifi_sta_is_connected();
    if(restore){
        wifi_sta_force_disconnect();
        error=esp_wifi_scan_start(&config,true);
    }
    s_panel_wifi_count=0;
    if(error==ESP_OK){
        wifi_ap_record_t ap;
        for(unsigned n=0;n<32 && s_panel_wifi_count<PANEL_WIFI_MAX_AP &&
            esp_wifi_scan_get_ap_record(&ap)==ESP_OK;n++){
            bool secured=ap.authmode!=WIFI_AUTH_OPEN;
            if(!ap.ssid[0] || (secured && ap.authmode!=WIFI_AUTH_WPA2_PSK &&
                ap.authmode!=WIFI_AUTH_WPA_WPA2_PSK && ap.authmode!=WIFI_AUTH_WPA3_PSK &&
                ap.authmode!=WIFI_AUTH_WPA2_WPA3_PSK))continue;
            unsigned i=0;
            while(i<s_panel_wifi_count && strcmp(s_panel_wifi_aps[i].ssid,(char *)ap.ssid))i++;
            if(i==s_panel_wifi_count){
                strlcpy(s_panel_wifi_aps[i].ssid,(char *)ap.ssid,sizeof(s_panel_wifi_aps[i].ssid));
                s_panel_wifi_aps[i].secured=secured;
                s_panel_wifi_count++;
            }
        }
        esp_wifi_clear_ap_list();
    }
    if(restore)setup_wifi();
    atomic_store(&s_panel_wifi_status,error!=ESP_OK?PANEL_WIFI_SCAN_FAILED:
        s_panel_wifi_count?PANEL_WIFI_LIST:PANEL_WIFI_EMPTY);
    atomic_store(&s_prov_busy,false);
    vTaskDelete(NULL);
}

bool panel_wifi_scan(void)
{
    bool busy=false;
    if(ota_service_is_busy() || !atomic_compare_exchange_strong(&s_prov_busy,&busy,true))return false;
    atomic_store(&s_panel_wifi_status,PANEL_WIFI_SCANNING);
    if(xTaskCreate(panel_scan_task,"scan_wifi",4096,NULL,5,NULL)==pdPASS)return true;
    atomic_store(&s_panel_wifi_status,PANEL_WIFI_SCAN_FAILED);
    atomic_store(&s_prov_busy,false);
    return false;
}

static void setup_wifi(void)
{
    atomic_store(&s_wifi_attempt_ms,now_ms());
    char ssid[33] = {0};
    char pass[65] = {0};
    if(!storage_get_wifi_at(s_wifi_profile,ssid,sizeof(ssid),pass,sizeof(pass))){
        s_wifi_profile=0;
        if(!storage_get_wifi(ssid,sizeof(ssid),pass,sizeof(pass))){
            if(!WIFI_DEFAULT_SSID[0])return;
            strlcpy(ssid,WIFI_DEFAULT_SSID,sizeof(ssid));strlcpy(pass,WIFI_DEFAULT_PASS,sizeof(pass));
        }
    }
    ESP_LOGI(TAG,"WiFi attempt profile=%u",s_wifi_profile+1);
    if(!wifi_sta_connect(ssid,pass))ESP_LOGW(TAG,"WiFi connection request rejected");
    memset(pass,0,sizeof(pass));
}

static bool start_saved_wifi(bool advance)
{
    bool idle=false;
    if(!atomic_compare_exchange_strong(&s_prov_busy,&idle,true))return false;
    if(advance)s_wifi_profile++;
    setup_wifi();
    atomic_store(&s_prov_busy,false);
    return true;
}

static void retry_saved_wifi(void)
{
    uint32_t now=now_ms();
    if(now-s_state_enter_ms>=WIFI_CONNECT_TIMEOUT_MS &&
       now-atomic_load(&s_wifi_attempt_ms)>=WIFI_CONNECT_TIMEOUT_MS && start_saved_wifi(true))
        s_state_enter_ms=now;
}

static void main_loop_task(void *arg)
{
    (void)arg;
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    /* The startup owner may wait for the LCD before enabling radio tasks. */
    esp_task_wdt_add(xTaskGetCurrentTaskHandle());

    for (;;) {
        panel_weather_process(atomic_load(&s_prov_busy) || atomic_load(&s_local_ble_requested));
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
            } else retry_saved_wifi();
            break;

        case STATE_MQTT_CONNECTING:
            if (!wifi_sta_is_connected()) {
                enter_state(STATE_STA_CONNECTING);
                break;
            }
            if(panel_weather_network_busy())break;
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
        full_alarm_tick();
        indicators_tick();
        cmd_handler_tick();
        panel_ble_tick();
        ble_ct_cmd_tick();
        panel_ble_aux_tick();
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
    ble_ct_cmd_set_battery_supported(false);
    panel_ble_init();
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
    bool previous=atomic_exchange(&s_local_ble_requested, requested);
    if(requested && !previous)atomic_store(&s_ble_retry_requested,true);
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
    const bool ble_start_required = !s_ble_services_started ||
        ble_prov_is_suspended() || ble_prov_start_failed();
    const bool ble_memory_available = heap_caps_get_free_size(caps) >= 65536U &&
        heap_caps_get_largest_free_block(caps) >= 32768U;
    if(atomic_exchange(&s_ble_retry_requested,false))s_ble_retry_after_ms=0;
    if(!s_local_ble_mode && ble_start_required && !ble_memory_available &&
       s_ble_retry_after_ms && (int32_t)(now-s_ble_retry_after_ms)<0){
        /* Failed local admission must not keep taking the cloud offline. */
        mqtt_set_connect_allowed(true);
        return;
    }
    const bool local_fallback = requested &&
        ble_start_required && (ble_prov_start_failed() || !ble_memory_available);
    if (!s_local_ble_mode && mqtt_is_connected() && !local_fallback) {
        s_cloud_retry_deadline_ms = now + CLOUD_RETRY_WINDOW_MS;
        mqtt_set_connect_allowed(true);
        if (ble_prov_is_ready() && (int32_t)(now - s_next_coexist_log_ms) >= 0) {
            s_next_coexist_log_ms = now + 30000U;
            ESP_LOGW(TAG, "MQTT+BLE resident: free=%u largest=%u",
                     (unsigned)heap_caps_get_free_size(caps),
                     (unsigned)heap_caps_get_largest_free_block(caps));
        }
        /* Keep the working cloud link when no phone has requested memory.
         * A failed, unsuspended host may be cleaned once below, but never
         * retry start/restore below the BLE service's existing admission floor.
         * OTA-suspended hosts retain their owner until a restore is admitted. */
        if (ble_start_required && !ble_memory_available &&
            (!ble_prov_start_failed() || ble_prov_is_suspended())) return;
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
    if(ble_start_required && !ble_memory_available &&
       !atomic_load(&s_prov_busy) && !ble_prov_is_connected()){
        if(!ota_service_admin_begin())return;
        bool cleaned=true;
        if(ble_prov_start_failed() && !ble_prov_is_suspended()){
            cleaned=ble_prov_recover_start_failure();
            if(cleaned)s_ble_services_started=false;
        }
        const bool resumed=cleaned && mqtt_resume_after_local();
        ota_service_admin_end();
        if(!resumed){s_next_ble_start_ms=now+BLE_START_RETRY_MS;return;}
        s_local_ble_mode=false;
        s_ble_retry_after_ms=now+BLE_MEMORY_RETRY_MS;
        s_cloud_retry_deadline_ms=s_ble_retry_after_ms;
        s_next_ble_start_ms=now;
        mqtt_set_connect_allowed(true);
        ESP_LOGW(TAG,"BLE memory low; cloud restored; retry=%u ms",BLE_MEMORY_RETRY_MS);
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
        ESP_LOGW(TAG, "MQTT resume; BLE retry in %u ms",
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
        ESP_LOGW(TAG, "BLE start failed; MQTT paused");
        s_next_ble_start_ms = now + BLE_START_RETRY_MS;
    }
}

/* Only the main loop owns lifecycle changes; LCD/GAP callbacks publish intent.
 * SDK auto-reconnect stays disabled so no TLS attempt bypasses local mode. */
static void startup_deferred_tick(void)
{
    const uint32_t now = now_ms();
    bool weather_busy=panel_weather_network_busy();
    if(!weather_busy)mqtt_poll_connection();
    else mqtt_set_connect_allowed(false);
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
    if(weather_busy)return;
    if (s_startup_owner ||
        (int32_t)(now - s_next_ble_start_ms) < 0) {
        if(s_startup_owner)mqtt_set_connect_allowed(false);
        return; /* Retain the last decision while BLE scheduling is deferred. */
    }
    mqtt_set_connect_allowed(false);
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
    ESP_ERROR_CHECK(direct_board_pins_valid() ? ESP_OK : ESP_ERR_INVALID_ARG);
    direct_buzzer_safe_init();
    direct_can_idle_init();
    direct_panel_prepare_boot();
    esp_log_level_set("*", ESP_LOG_INFO);
    ESP_LOGW(TAG, "LCD_ISOLATION %s reset=%d WiFi=OFF BLE=OFF MQTT=OFF SPI=20000000",
             FIRMWARE_VERSION, (int)esp_reset_reason());
    direct_panel_init();
    return;
#endif
    atomic_store(&s_boot_stage,BOOT_BUZZ);
    if (direct_board_pins_valid()) {
        direct_buzzer_safe_init();
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
    /* Link diagnostics must remain visible in normal (non-debug) firmware. */
    esp_log_level_set("panel_bus", ESP_LOG_INFO);
    esp_log_level_set("modbus_direct", ESP_LOG_INFO);
    esp_log_level_set("panel_led", ESP_LOG_INFO);
    esp_log_level_set("mb_alarm", ESP_LOG_INFO);
    atomic_store(&s_boot_stage, BOOT_BUZZ);
    ESP_LOGI(TAG, "Buzzer GPIO%d; USB wait=0ms", CONFIG_DIRECT_BUZZ_PIN);
    /* BLE 配对诊断: 放开 NimBLE 协议栈日志 (需 CONFIG_BT_NIMBLE_LOG_LEVEL=DEBUG 编译) */
    esp_log_level_set("NimBLE", ESP_LOG_WARN);
    debug_log_init();   /* 尽早装钩子, 让后续启动日志也进调试缓冲 */
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, " CS-MB-PANEL full controller (ESP-IDF %s)", FIRMWARE_VERSION);
    ESP_LOGI(TAG, " ESP32-C3 + Modbus inverter / direct BMS + MQTT");
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
    /* GPIO9 is sampled by the ROM before the LED driver takes ownership. */
    (void)full_indicators_init(CONFIG_PANEL_LED_GPIO);
    arm_poll_init();
    (void)full_alarm_init();
    atomic_store(&s_boot_stage, BOOT_LCD);
    if(!cloud_binding_start())ESP_LOGW(TAG,"Cloud binding state unavailable: configure server public key; local operation continues");
    panel_weather_init();
    direct_panel_init();
    atomic_store(&s_boot_stage, BOOT_NETWORK);
    if(!factory_service_start())ESP_LOGE(TAG,"Factory USB service failed");
    printf("[DEVICE] CS-MB-PANEL firmware=%s sn=%s reset=%d\n",FIRMWARE_VERSION,g_device_sn,(int)esp_reset_reason());
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
    start_saved_wifi(false);
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

