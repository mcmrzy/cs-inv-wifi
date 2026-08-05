/*
 * wifi_sta.h — WiFi STA + 自动重连
 *
 * 事件组 WIFI_CONNECTED_BIT 表示已获取 IP (wifi_sta_wait_connected)
 */
#ifndef WIFI_STA_H
#define WIFI_STA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#ifdef __cplusplus
extern "C" {
#endif

extern EventGroupHandle_t s_wifi_event_group;
extern const int WIFI_CONNECTED_BIT;

/* 初始化 (netif/事件循环/wifi 驱动), 由 app_main 调用 */
void wifi_sta_init(void);

/* 配置 STA 并连接; 断开后 esp_wifi 自动重连 */
bool wifi_sta_connect(const char *ssid, const char *pass);

/* 是否已获取 IP */
bool wifi_sta_is_connected(void);

/* 主动断开 (停止自动重连); 配网失败后用于停止持续重试 */
void wifi_sta_force_disconnect(void);

/* 最近一次断开原因 (esp_wifi 的 WIFI_REASON_*), 未断开过返回 0 */
uint8_t wifi_sta_get_last_disconnect_reason(void);

/* 阻塞等待连接 (超时 ms, 0=永久), 返回是否成功 */
bool wifi_sta_wait_connected(uint32_t timeout_ms);

/* 当前 RSSI (dBm), 未连接返回 0 */
int wifi_sta_get_rssi(void);

/* 当前 IP 字符串, 未连接返回 false */
bool wifi_sta_get_ip(char *ip, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_STA_H */
