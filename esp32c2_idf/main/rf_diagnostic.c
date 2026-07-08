/**
 * ESP32-C2 RF 诊断 — 最小版
 * 只测晶振和 WiFi 扫描，不用 BLE，不用事件循环。
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_mac.h"

static void sep(void) { printf("--------------------------------------------------\n"); }

void app_main(void)
{
    printf("\n========================================\n");
    printf("  ESP32-C2 RF Diagnostic v2.0\n");
    printf("========================================\n\n");

    /* NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    /* ---- TEST 1: 芯片信息 ---- */
    sep();
    printf("  TEST 1: Chip Info\n");
    sep();
    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);
    printf("  MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    printf("  Heap: %lu free\n", (unsigned long)esp_get_free_heap_size());

    /* ---- TEST 2: 晶振 ---- */
    sep();
    printf("  TEST 2: Crystal Clock\n");
    sep();
    /* warmup run to prime cache */
    { int64_t tw0 = esp_timer_get_time(); vTaskDelay(pdMS_TO_TICKS(200)); (void)tw0; }

    printf("  Measuring 10 x 500ms...\n");
    int32_t drifts[10];
    for (int i = 0; i < 10; i++) {
        int64_t t0 = esp_timer_get_time();
        vTaskDelay(pdMS_TO_TICKS(500));
        int64_t t1 = esp_timer_get_time();
        int32_t us = (int32_t)(t1 - t0);
        drifts[i] = us - 500000;
        printf("    [%d] %ld us (drift %ld)\n", i, (long)us, (long)drifts[i]);
    }
    /* check consistency: max - min jitter */
    int32_t d_min = drifts[0], d_max = drifts[0];
    int64_t d_sum = 0;
    for (int i = 0; i < 10; i++) {
        if (drifts[i] < d_min) d_min = drifts[i];
        if (drifts[i] > d_max) d_max = drifts[i];
        d_sum += drifts[i];
    }
    int32_t jitter = d_max - d_min;
    int32_t avg_drift = (int32_t)(d_sum / 10);
    printf("  Avg drift: %ld us, Jitter: %ld us\n", (long)avg_drift, (long)jitter);
    if (jitter > 1000)
        printf("  [FAIL] Timing jitter too large — clock unstable\n");
    else
        printf("  [PASS] Crystal stable (RTOS tick offset is normal)\n");

    /* ---- TEST 3: WiFi 扫描 ---- */
    sep();
    printf("  TEST 3: WiFi Scan (RX)\n");
    sep();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    vTaskDelay(pdMS_TO_TICKS(500));

    printf("  Scanning...\n");
    wifi_scan_config_t sc = {
        .ssid = NULL, .bssid = NULL, .channel = 0,
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time = { .active = { .min = 100, .max = 300 } },
    };
    esp_err_t scan_err = esp_wifi_scan_start(&sc, true);
    if (scan_err != ESP_OK) {
        printf("  [FAIL] Scan error: %s\n", esp_err_to_name(scan_err));
    } else {
        uint16_t n = 0;
        esp_wifi_scan_get_ap_num(&n);
        if (n == 0) {
            printf("  [FAIL] No networks — RX broken?\n");
        } else {
            uint16_t show = (n > 10) ? 10 : n;
            wifi_ap_record_t *ap = malloc(sizeof(wifi_ap_record_t) * show);
            uint16_t real = show;
            esp_wifi_scan_get_ap_records(&real, ap);
            printf("  Found %d networks:\n", n);
            for (int i = 0; i < real; i++) {
                printf("    %2d. %-28s CH:%2d RSSI:%4d\n",
                       i + 1, (char *)ap[i].ssid, ap[i].primary, ap[i].rssi);
            }
            free(ap);
            printf("  [PASS] RX working\n");
        }
    }

    /* ---- TEST 4: WiFi AP (TX) ---- */
    sep();
    printf("  TEST 4: WiFi AP (TX)\n");
    sep();

    esp_wifi_stop();
    vTaskDelay(pdMS_TO_TICKS(200));

    esp_wifi_set_mode(WIFI_MODE_AP);
    wifi_config_t ap_cfg = {
        .ap = {
            .ssid = "ESP32C2_DIAG",
            .ssid_len = 11,
            .channel = 1,
            .authmode = WIFI_AUTH_OPEN,
            .max_connection = 1,
        },
    };
    esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    esp_wifi_set_max_tx_power(20 * 4);
    esp_wifi_start();

    uint8_t ap_mac[6];
    esp_wifi_get_mac(WIFI_IF_AP, ap_mac);
    printf("  AP MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
           ap_mac[0], ap_mac[1], ap_mac[2], ap_mac[3], ap_mac[4], ap_mac[5]);
    int8_t pw;
    esp_wifi_get_max_tx_power(&pw);
    printf("  TX Power: %d dBm\n", pw / 4);
    printf("  SSID: ESP32C2_DIAG\n");
    printf("  *** CHECK YOUR PHONE NOW ***\n");
    printf("  Keeping AP for 30s...\n");
    for (int i = 30; i > 0; i--) {
        if (i % 10 == 0) printf("  %d s...\n", i);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    /* ---- 完成 ---- */
    sep();
    printf("  Diagnostic complete.\n");
    sep();

    while (1) vTaskDelay(pdMS_TO_TICKS(10000));
}
