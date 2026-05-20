#ifndef LED_STATUS_H
#define LED_STATUS_H

#include "config.h"

// ==================== LED 分工 ====================
// LED1 (GPIO12) — 系统 / WiFi 状态
// LED2 (GPIO13) — MQTT / 数据 状态
//
// 用户一眼就能判断问题出在 WiFi 还是 MQTT：
//   LED1 常亮 + LED2 闪烁  → WiFi 已连，MQTT 正在连（等一会就好）
//   LED1 快闪 + LED2 灭    → WiFi 正在连（可能密码错或信号差）
//   LED1 慢闪 + LED2 灭    → AP 配网模式（需要手机配网）
//   都常亮                  → 一切正常
//   LED1 常亮 + LED2 快闪  → WiFi 正常但 MQTT 连不上（服务器问题）

typedef enum {
  LED_OFF = 0,
  LED_SLOW_BLINK,    // 1Hz（AP 配网 / MQTT 连接中）
  LED_FAST_BLINK,    // 3Hz（WiFi 连接中）
  LED_SOLID,         // 常亮（正常）
  LED_FAULT,         // 5Hz 快闪（故障）
  LED_BREATH,        // 呼吸灯（OTA）
  LED_3FLASH,        // 3 次快闪（恢复出厂确认）
} LedPattern;

void led_init();

// 每个 LED 独立设置
void led1_set(LedPattern state);
void led2_set(LedPattern state);

// 快捷函数：触发数据活动指示（LED2 短暂闪一下）
void led_signal_data();

void led_tick();

#endif
