/**
 * ESP32-C2 RF Diagnostic Tool (ESP-IDF)
 *
 * 诊断射频 TX 性能问题（晶振、PA、天线、匹配网络）
 * 输出: 串口 115200 波特率
 *
 * 测试项目:
 *   1. WiFi 扫描 (RX 测试)
 *   2. WiFi AP 广播 (TX 测试)
 *   3. BLE 广播 (TX 测试)
 *   4. RF 校准数据 / eFuse 读取
 *   5. TX 功率测试
 *   6. 时钟稳定性检测
 *   7. WiFi 连接测试 (综合 TX+RX)
 */

#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_chip_info.h"
#include "esp_efuse.h"
#include "esp_efuse_table.h"
#include "esp_flash.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "driver/gpio.h"

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"

static const char *TAG = "RF_DIAG";

/* ==================== 配置 ==================== */
#define TEST_AP_SSID       "ESP32C2_RF_TEST"
#define TEST_AP_CHANNEL    1
#define TEST_AP_MAX_CONN   1
#define TEST_TX_POWER_DBM  20   /* 期望 TX 功率 dBm */
#define WIFI_SCAN_SECONDS  10
#define BLE_ADV_SECONDS    30
#define AP_HOLD_SECONDS    30

/* ==================== 结果存储 ==================== */
typedef struct {
    int  scan_count;
    int  scan_max_rssi;
    int  scan_min_rssi;
    bool ap_ok;
    bool ble_ok;
    int8_t tx_power_req;
    int8_t tx_power_act;
    uint8_t mac[6];
    uint8_t efuse_mac[6];
    uint32_t flash_size;
    uint32_t free_heap;
    uint32_t min_free_heap;
} test_result_t;

static test_result_t g_res = {0};

/* ==================== 打印工具 ==================== */
static void sep(void)       { printf("--------------------------------------------------\n"); }
static void hdr(const char *t) { printf("\n"); sep(); printf("  %s\n", t); sep(); }

static void result(const char *name, bool pass, const char *detail)
{
    printf("  [%s] %s", pass ? "PASS" : "FAIL", name);
    if (detail && detail[0]) printf(" — %s", detail);
    printf("\n");
}

/* ==================== TEST 4: eFuse / 芯片信息 ==================== */
static void test_chip_info(void)
{
    hdr("TEST 4: Chip & eFuse Info");

    esp_chip_info_t ci;
    esp_chip_info(&ci);

    const char *model = "unknown";
    switch (ci.model) {
        case CHIP_ESP32:   model = "ESP32";   break;
        case CHIP_ESP32S2: model = "ESP32-S2"; break;
        case CHIP_ESP32S3: model = "ESP32-S3"; break;
        case CHIP_ESP32C3: model = "ESP32-C3"; break;
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
        case CHIP_ESP32C2: model = "ESP32-C2"; break;
#endif
        default: break;
    }
    printf("  Chip: %s, Rev: %d, Cores: %d\n", model, ci.revision, ci.cores);

    /* eFuse MAC */
    esp_efuse_mac_get_default(g_res.efuse_mac);
    printf("  eFuse MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
           g_res.efuse_mac[0], g_res.efuse_mac[1], g_res.efuse_mac[2],
           g_res.efuse_mac[3], g_res.efuse_mac[4], g_res.efuse_mac[5]);

    /* Flash size */
    esp_flash_get_size(NULL, &g_res.flash_size);
    printf("  Flash: %lu MB\n", g_res.flash_size / (1024 * 1024));

    /* Heap */
    g_res.free_heap     = esp_get_free_heap_size();
    g_res.min_free_heap = esp_get_minimum_free_heap_size();
    printf("  Free heap:     %lu bytes\n", g_res.free_heap);
    printf("  Min free heap: %lu bytes\n", g_res.min_free_heap);
}

/* ==================== TEST 6: 时钟稳定性 ==================== */
static void test_clock(void)
{
    hdr("TEST 6: Clock Stability (Crystal Oscillator)");

    #define N_SAMPLES 20
    uint32_t samples[N_SAMPLES];
    const uint32_t expected_us = 100000; /* 100 ms */

    printf("  Taking %d samples of 100 ms intervals...\n", N_SAMPLES);

    for (int i = 0; i < N_SAMPLES; i++) {
        int64_t t0 = esp_timer_get_time();
        vTaskDelay(pdMS_TO_TICKS(100));
        int64_t t1 = esp_timer_get_time();
        samples[i] = (uint32_t)(t1 - t0);
    }

    uint32_t sum = 0, mn = samples[0], mx = samples[0];
    for (int i = 0; i < N_SAMPLES; i++) {
        sum += samples[i];
        if (samples[i] < mn) mn = samples[i];
        if (samples[i] > mx) mx = samples[i];
    }
    uint32_t avg = sum / N_SAMPLES;
    int32_t drift_ppm = ((int32_t)(avg - expected_us) * 1000000L) / (int32_t)expected_us;
    uint32_t jitter = mx - mn;

    printf("  Expected: %lu us\n", expected_us);
    printf("  Average:  %lu us\n", avg);
    printf("  Min: %lu  Max: %lu\n", mn, mx);
    printf("  Jitter: %lu us\n", jitter);
    printf("  Drift:  %ld ppm\n", (long)drift_ppm);

    printf("\n  Interpretation:\n");
    printf("    |Drift| < 10 ppm  → Crystal OK\n");
    printf("    |Drift| 10-50 ppm → Marginal, may affect RF\n");
    printf("    |Drift| > 50 ppm  → BAD — likely cause of TX failure\n");
    printf("    Jitter > 1000 us  → Clock unstable\n");

    bool ok = (abs(drift_ppm) < 50 && jitter < 1500);
    result("Clock stability", ok,
           ok ? "Crystal appears OK" : "Crystal may be faulty!");

    /* delay 精度 */
    int64_t dt0 = esp_timer_get_time();
    vTaskDelay(pdMS_TO_TICKS(1000));
    int64_t dt1 = esp_timer_get_time();
    uint32_t actual = (uint32_t)(dt1 - dt0);
    int32_t dppm = ((int32_t)(actual - 1000000) * 1000000L) / 1000000L;
    printf("  1s delay actual: %lu us (drift %ld ppm)\n", (unsigned long)actual, (long)dppm);
}

/* ==================== TEST 5: TX 功率 ==================== */
static void test_tx_power(void)
{
    hdr("TEST 5: TX Power Configuration");

    int8_t powers[] = {8, 13, 15, 17, 19, 20};
    int np = sizeof(powers) / sizeof(powers[0]);

    printf("  %-10s %-10s %s\n", "Requested", "Actual", "Status");
    printf("  %-10s %-10s %s\n", "---------", "------", "------");

    for (int i = 0; i < np; i++) {
        int8_t req = powers[i];
        esp_wifi_set_max_tx_power(req * 4);   /* 单位 0.25 dBm */
        int8_t act = 0;
        esp_wifi_get_max_tx_power(&act);
        int act_dbm = act / 4;
        bool match = (abs(act_dbm - req) <= 2);
        printf("  %-10d %-10d %s\n", req, act_dbm, match ? "OK" : "CLAMPED");
    }

    /* 恢复 */
    esp_wifi_set_max_tx_power(TEST_TX_POWER_DBM * 4);
    int8_t final_act;
    esp_wifi_get_max_tx_power(&final_act);
    g_res.tx_power_req = TEST_TX_POWER_DBM;
    g_res.tx_power_act = final_act / 4;
}

/* ==================== TEST 1: WiFi 扫描 (RX) ==================== */
static void test_wifi_scan(void)
{
    hdr("TEST 1: WiFi Scan (RX Test)");
    printf("  Scanning for %d seconds...\n", WIFI_SCAN_SECONDS);

    wifi_scan_config_t sc = {
        .ssid = NULL, .bssid = NULL, .channel = 0,
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time = { .active = { .min = 100, .max = 300 } },
    };

    esp_err_t err = esp_wifi_scan_start(&sc, true);
    if (err != ESP_OK) {
        printf("  [ERROR] scan failed: %s\n", esp_err_to_name(err));
        result("WiFi RX scan", false, "scan start failed");
        return;
    }

    uint16_t ap_num = 0;
    esp_wifi_scan_get_ap_num(&ap_num);
    g_res.scan_count = ap_num;
    g_res.scan_max_rssi = -999;
    g_res.scan_min_rssi = 0;

    if (ap_num == 0) {
        printf("  [WARN] No networks found!\n");
        printf("  → Possible: RX path damaged / no WiFi in range\n");
    } else {
        if (ap_num > 20) ap_num = 20;
        wifi_ap_record_t *ap = malloc(sizeof(wifi_ap_record_t) * ap_num);
        if (ap) {
            uint16_t real_num = ap_num;
            esp_wifi_scan_get_ap_records(&real_num, ap);
            printf("  Found %d networks:\n", real_num);
            for (int i = 0; i < real_num; i++) {
                printf("    %2d. %-32s CH:%2d RSSI:%4d %s\n",
                       i + 1, (char *)ap[i].ssid, ap[i].primary,
                       ap[i].rssi,
                       ap[i].authmode == WIFI_AUTH_OPEN ? "[OPEN]" : "[SECURED]");
                if (ap[i].rssi > g_res.scan_max_rssi) g_res.scan_max_rssi = ap[i].rssi;
                if (ap[i].rssi < g_res.scan_min_rssi) g_res.scan_min_rssi = ap[i].rssi;
            }
            printf("  RSSI range: %d ~ %d dBm\n", g_res.scan_min_rssi, g_res.scan_max_rssi);
            free(ap);
        }
    }
    result("WiFi RX scan", ap_num > 0,
           ap_num > 0 ? "RX path working" : "RX may have issues");
}

/* ==================== TEST 2: WiFi AP (TX) ==================== */
static void test_wifi_ap(void)
{
    hdr("TEST 2: WiFi AP Broadcast (TX Test)");

    /* 配置 AP */
    wifi_config_t ap_cfg = {
        .ap = {
            .ssid = TEST_AP_SSID,
            .ssid_len = strlen(TEST_AP_SSID),
            .channel = TEST_AP_CHANNEL,
            .authmode = WIFI_AUTH_OPEN,
            .max_connection = TEST_AP_MAX_CONN,
        },
    };

    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    esp_wifi_set_max_tx_power(TEST_TX_POWER_DBM * 4);

    esp_err_t err = esp_wifi_start();
    if (err != ESP_OK) {
        printf("  [ERROR] WiFi start failed: %s\n", esp_err_to_name(err));
        result("WiFi AP broadcast", false, "WiFi start failed");
        return;
    }

    /* 读实际功率 */
    int8_t act_pw;
    esp_wifi_get_max_tx_power(&act_pw);
    printf("  TX Power: %d dBm (actual %d dBm)\n", TEST_TX_POWER_DBM, act_pw / 4);

    /* 读 AP MAC */
    uint8_t ap_mac[6];
    esp_wifi_get_mac(WIFI_IF_AP, ap_mac);
    printf("  AP MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
           ap_mac[0], ap_mac[1], ap_mac[2], ap_mac[3], ap_mac[4], ap_mac[5]);

    printf("\n  >>> CHECK YOUR PHONE: Can you see '%s' ? <<<\n", TEST_AP_SSID);
    printf("  >>> If NOT visible → TX path broken (PA/balun/antenna) <<<\n\n");

    g_res.ap_ok = true;
    printf("  Keeping AP alive for %d seconds...\n", AP_HOLD_SECONDS);
    for (int i = AP_HOLD_SECONDS; i > 0; i--) {
        if (i % 5 == 0) printf("  %d s remaining...\n", i);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    result("WiFi AP broadcast", true, "AP started — did phone see it?");
}

/* ==================== TEST 3: BLE 广播 (TX) ==================== */
static void gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    (void)param;
    if (event == ESP_GAP_BLE_ADV_START_COMPLETE_EVT) {
        g_res.ble_ok = (param->adv_start_cmpl.status == ESP_BT_STATUS_SUCCESS);
    }
}

static void test_ble(void)
{
    hdr("TEST 3: BLE Advertising (TX Test)");

    /* 释放经典 BT 内存给 BLE */
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_bt_controller_init(&bt_cfg);
    if (err != ESP_OK) {
        printf("  [ERROR] BT init failed: %s\n", esp_err_to_name(err));
        result("BLE advertising", false, "BT init failed");
        return;
    }
    err = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (err != ESP_OK) {
        printf("  [ERROR] BT enable failed: %s\n", esp_err_to_name(err));
        result("BLE advertising", false, "BT enable failed");
        return;
    }

    esp_bluedroid_config_t bluedroid_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    err = esp_bluedroid_init_with_cfg(&bluedroid_cfg);
    if (err != ESP_OK) {
        printf("  [ERROR] Bluedroid init failed: %s\n", esp_err_to_name(err));
        result("BLE advertising", false, "Bluedroid init failed");
        goto ble_cleanup;
    }
    err = esp_bluedroid_enable();
    if (err != ESP_OK) {
        printf("  [ERROR] Bluedroid enable failed: %s\n", esp_err_to_name(err));
        result("BLE advertising", false, "Bluedroid enable failed");
        goto ble_cleanup;
    }

    esp_ble_gap_register_callback(gap_cb);

    /* 设备名称 */
    esp_ble_gap_set_device_name("ESP32C2_BLE");

    /* 广播数据 */
    esp_ble_adv_data_t adv_data = {
        .set_scan_rsp = false,
        .include_name = true,
        .include_txpower = false,
        .min_interval = 0x0006,
        .max_interval = 0x0010,
        .appearance = 0x00,
        .manufacturer_len = 0,
        .p_manufacturer_data = NULL,
        .service_data_len = 0,
        .p_service_data = NULL,
        .service_uuid_len = 0,
        .p_service_uuid_data = NULL,
        .flag = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT),
    };
    esp_ble_gap_config_adv_data(&adv_data);

    esp_ble_adv_params_t adv_params = {
        .adv_int_min       = 0x20,
        .adv_int_max       = 0x40,
        .adv_type          = ADV_TYPE_IND,
        .own_addr_type     = BLE_ADDR_TYPE_PUBLIC,
        .peer_addr         = {0},
        .peer_addr_type    = BLE_ADDR_TYPE_PUBLIC,
        .channel_map       = ADV_CHNL_ALL,
        .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
    };
    esp_ble_gap_start_advertising(&adv_params);

    printf("\n  >>> CHECK YOUR PHONE: BLE scanner (nRF Connect) <<<\n");
    printf("  >>> Device name: ESP32C2_BLE <<<\n");
    printf("  >>> If NOT visible → TX path has issues <<<\n\n");

    printf("  Keeping BLE alive for %d seconds...\n", BLE_ADV_SECONDS);
    for (int i = BLE_ADV_SECONDS; i > 0; i--) {
        if (i % 5 == 0) printf("  %d s remaining...\n", i);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    esp_ble_gap_stop_advertising();

ble_cleanup:
    esp_bluedroid_disable();
    esp_bluedroid_deinit();
    esp_bt_controller_disable();
    esp_bt_controller_deinit();

    result("BLE advertising", g_res.ble_ok,
           g_res.ble_ok ? "Advertising started — did phone see it?" : "Failed to start");
}

/* ==================== TEST 7: WiFi 连接 (TX+RX) ==================== */
static EventGroupHandle_t s_wifi_event;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupSetBits(s_wifi_event, WIFI_FAIL_BIT);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        printf("  Got IP: " IPSTR "\n", IP2STR(&ev->ip_info.ip));
        xEventGroupSetBits(s_wifi_event, WIFI_CONNECTED_BIT);
    }
}

static void test_wifi_connect(void)
{
    hdr("TEST 7: WiFi Connection Test (TX+RX Combined)");
    printf("  Scanning for open networks to test connection...\n");

    /* 先扫描 */
    wifi_scan_config_t sc = {
        .ssid = NULL, .bssid = NULL, .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time = { .active = { .min = 100, .max = 300 } },
    };
    esp_wifi_scan_start(&sc, true);

    uint16_t ap_num = 0;
    esp_wifi_scan_get_ap_num(&ap_num);
    if (ap_num == 0) {
        printf("  No networks found, skipping.\n");
        return;
    }
    if (ap_num > 20) ap_num = 20;

    wifi_ap_record_t *ap = malloc(sizeof(wifi_ap_record_t) * ap_num);
    if (!ap) return;
    uint16_t real_num = ap_num;
    esp_wifi_scan_get_ap_records(&real_num, ap);

    /* 找最强开放网络 */
    int best = -1;
    int best_rssi = -999;
    for (int i = 0; i < real_num; i++) {
        if (ap[i].authmode == WIFI_AUTH_OPEN && ap[i].rssi > best_rssi) {
            best_rssi = ap[i].rssi;
            best = i;
        }
    }

    if (best < 0) {
        printf("  No open networks found. Skipping connection test.\n");
        free(ap);
        return;
    }

    printf("  Found open: %s (RSSI %d)\n", (char *)ap[best].ssid, ap[best].rssi);
    printf("  Connecting...\n");

    s_wifi_event = xEventGroupCreate();

    esp_wifi_set_mode(WIFI_MODE_STA);
    wifi_config_t sta_cfg = {0};
    strncpy((char *)sta_cfg.sta.ssid, (char *)ap[best].ssid, sizeof(sta_cfg.sta.ssid) - 1);
    sta_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL);

    esp_wifi_connect();

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdTRUE, pdFALSE, pdMS_TO_TICKS(15000));

    bool connected = (bits & WIFI_CONNECTED_BIT);
    if (connected) {
        printf("  CONNECTED! TX+RX both working!\n");
        wifi_ap_record_t info;
        if (esp_wifi_sta_get_ap_info(&info) == ESP_OK) {
            printf("  RSSI: %d dBm\n", info.rssi);
        }
        esp_wifi_disconnect();
    } else {
        printf("  Connection FAILED (timeout)\n");
        printf("  → TX may be too weak for AP to receive\n");
    }

    result("WiFi connection", connected,
           connected ? "TX+RX functional" : "TX path likely broken");

    esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler);
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler);
    vEventGroupDelete(s_wifi_event);
    free(ap);
}

/* ==================== 最终报告 ==================== */
static void print_report(void)
{
    hdr("DIAGNOSTIC SUMMARY");

    printf("  WiFi Scan (RX):   %s (%d networks)\n",
           g_res.scan_count > 0 ? "PASS" : "FAIL", g_res.scan_count);
    printf("  WiFi AP (TX):     %s\n", g_res.ap_ok ? "AP started" : "FAIL");
    printf("  BLE (TX):         %s\n", g_res.ble_ok ? "Advertising" : "FAIL");
    printf("  TX Power:         %d dBm (actual)\n", g_res.tx_power_act);
    printf("  Free Heap:        %lu bytes\n", g_res.free_heap);

    printf("\n  ROOT CAUSE ANALYSIS:\n");
    sep();

    if (g_res.scan_count == 0) {
        printf("  [CRITICAL] RX broken — cannot scan ANY networks\n");
        printf("  → Check: LNA, balun, antenna connection\n");
    } else if (g_res.scan_count > 0 && !g_res.ap_ok) {
        printf("  [CRITICAL] RX works but TX AP failed\n");
        printf("  → Check: PA, TX balun, antenna matching\n");
    } else if (g_res.ap_ok) {
        printf("  [INFO] AP started OK\n");
        printf("  → If phone CANNOT see AP: TX power too low or antenna issue\n");
        printf("  → If phone CAN see AP but can't connect: TX/RX mismatch\n");
    }

    printf("\n  HARDWARE CHECKLIST:\n");
    sep();
    printf("  [ ] Crystal: 40MHz, ±10ppm, load cap match\n");
    printf("  [ ] Crystal traces: short, no vias, ground guard\n");
    printf("  [ ] No high-freq signals near crystal (SDIO, UART)\n");
    printf("  [ ] PA/VDD3P3 power: stable, low ripple\n");
    printf("  [ ] Antenna: connected, matching network populated\n");
    printf("  [ ] Balun: properly connected to chip and antenna\n");
    printf("  [ ] No inductive components near antenna\n");
    printf("  [ ] PCB ground plane: continuous under RF section\n");

    printf("\n");
    sep();
    printf("  Diagnostic complete.\n");
    sep();
}

/* ==================== WiFi 初始化 ==================== */
static void wifi_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
}

/* ==================== app_main ==================== */
void app_main(void)
{
    printf("\n");
    printf("╔══════════════════════════════════════════════════╗\n");
    printf("║       ESP32-C2 RF Diagnostic Tool v1.0          ║\n");
    printf("║       Crystal Oscillator & TX Performance        ║\n");
    printf("╚══════════════════════════════════════════════════╝\n\n");

    /* NVS — WiFi 驱动需要 */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    wifi_init();

    /* 运行测试 */
    test_chip_info();     /* TEST 4: 芯片/eFuse */
    test_clock();         /* TEST 6: 晶振 */
    test_tx_power();      /* TEST 5: TX 功率 */

    /* TEST 1: WiFi 扫描 */
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();
    vTaskDelay(pdMS_TO_TICKS(500));
    test_wifi_scan();

    /* TEST 2: WiFi AP */
    test_wifi_ap();

    /* TEST 3: BLE */
    esp_wifi_stop();
    vTaskDelay(pdMS_TO_TICKS(200));
    test_ble();

    /* TEST 7: WiFi 连接 */
    esp_wifi_start();
    vTaskDelay(pdMS_TO_TICKS(500));
    test_wifi_connect();

    esp_wifi_stop();

    print_report();
}
