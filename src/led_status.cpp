/**
 * LED 双色状态指示驱动 — 辰烁科技 CS_INV
 * ==========================================
 * LED1 (GPIO12, 绿色) = 系统 / WiFi 状态
 * LED2 (GPIO13, 蓝色) = MQTT / 数据 状态
 *
 * 独立控制，一目了然哪个环节出了问题
 */

#include "led_status.h"

// ------ 每个 LED 独立的状态机 ------
struct LedSlot {
  int       pin;
  LedPattern pattern;
  unsigned long last_toggle;
  bool      cur_on;
  // 呼吸灯
  int       breath_step;
  int       breath_dir;
  // 3-flash
  int       flash_count;
  int       flash_total;
};

static LedSlot g_led1;
static LedSlot g_led2;

// 数据闪烁（不影响 LED2 主要模式）
static bool      g_data_flash_active = false;
static unsigned long g_data_flash_start = 0;
static unsigned long g_data_flash_prev  = 0;  // LED2 恢复前状态（仅模式切换用）

#define DATA_FLASH_MS  80   // LED2 数据闪烁持续时间

// ------ 常量 ------
#define SLOW_MS   500   // 1Hz
#define FAST_MS   166   // 3Hz
#define FAULT_MS  100   // 5Hz
#define BREATH_T  1500
#define BREATH_S  30
#define BREATH_MS (BREATH_T / BREATH_S)
#define FLASH_ON  150
#define FLASH_OFF 250

// ==================== 底层写 GPIO ====================
static void led_write(LedSlot* s, bool on) {
  digitalWrite(s->pin, on ? HIGH : LOW);
  s->cur_on = on;
}

static void led_on(LedSlot* s)  { led_write(s, true); }
static void led_off(LedSlot* s) { led_write(s, false); }

// ==================== 初始化 ====================
void led_init() {
  g_led1.pin = PIN_LED1;  g_led1.pattern = LED_OFF;  g_led1.last_toggle = 0;
  g_led1.cur_on = false;   g_led1.breath_step = 0;    g_led1.breath_dir = 1;
  g_led1.flash_count = 0;  g_led1.flash_total = 0;

  g_led2.pin = PIN_LED2;  g_led2.pattern = LED_OFF;  g_led2.last_toggle = 0;
  g_led2.cur_on = false;   g_led2.breath_step = 0;    g_led2.breath_dir = 1;
  g_led2.flash_count = 0;  g_led2.flash_total = 0;

  pinMode(PIN_LED1, OUTPUT);
  pinMode(PIN_LED2, OUTPUT);
  digitalWrite(PIN_LED1, LOW);
  digitalWrite(PIN_LED2, LOW);
}

// ==================== 设置模式 ====================
static void led_set_pattern(LedSlot* s, LedPattern p) {
  if (s->pattern == p) return;

  // 先关掉
  led_off(s);

  s->pattern    = p;
  s->last_toggle = millis();
  s->breath_step = 0;
  s->breath_dir  = 1;
  s->flash_count = 0;
  s->flash_total = 0;

  switch (p) {
    case LED_OFF:
      break;
    case LED_SLOW_BLINK:
    case LED_FAST_BLINK:
    case LED_FAULT:
      led_on(s);   // 从亮开始
      break;
    case LED_SOLID:
      led_on(s);
      break;
    case LED_BREATH:
      break;  // tick() 里用 analogWrite
    case LED_3FLASH:
      s->flash_total = 3;
      s->flash_count = 1;
      led_on(s);
      s->last_toggle = millis();
      break;
  }
}

void led1_set(LedPattern state) { led_set_pattern(&g_led1, state); }
void led2_set(LedPattern state) {
  g_data_flash_prev = (unsigned long)state;  // 记住切换前的模式（仅模式切换时使用）
  led_set_pattern(&g_led2, state);
}

// ==================== 数据活动指示 ====================
void led_signal_data() {
  g_data_flash_active = true;
  g_data_flash_start  = millis();
}

// ==================== tick ====================
static void led_tick_slot(LedSlot* s, int pin) {
  unsigned long now = millis();

  switch (s->pattern) {
    case LED_OFF:
    case LED_SOLID:
      break;

    case LED_SLOW_BLINK:
      if (now - s->last_toggle >= SLOW_MS) {
        s->last_toggle = now;
        led_write(s, !s->cur_on);
      }
      break;

    case LED_FAST_BLINK:
      if (now - s->last_toggle >= FAST_MS) {
        s->last_toggle = now;
        led_write(s, !s->cur_on);
      }
      break;

    case LED_FAULT:
      if (now - s->last_toggle >= FAULT_MS) {
        s->last_toggle = now;
        led_write(s, !s->cur_on);
      }
      break;

    case LED_BREATH:
      if (now - s->last_toggle >= BREATH_MS) {
        s->last_toggle = now;
        s->breath_step += s->breath_dir;
        int duty = (s->breath_step * 255) / BREATH_S;
#ifdef CHIP_ESP32C3
        ledcWrite(WIFI_LED_CHANNEL, duty);
#else
        analogWrite(pin, duty);
#endif
        if (s->breath_step >= BREATH_S)      { s->breath_dir = -1; }
        if (s->breath_step <= 0)             { s->breath_dir = 1; s->breath_step = 0; }
      }
      break;

    case LED_3FLASH:
      if (s->flash_count < s->flash_total * 2) {
        unsigned long interval = (s->flash_count % 2 == 0) ? FLASH_OFF : FLASH_ON;
        if (now - s->last_toggle >= interval) {
          led_write(s, (s->flash_count % 2 == 0));
          s->last_toggle = now;
          s->flash_count++;
          if (s->flash_count >= s->flash_total * 2) {
            s->pattern = LED_OFF;
            led_off(s);
          }
        }
      }
      break;
  }
}

void led_tick() {
  led_tick_slot(&g_led1, PIN_LED1);

  // LED2：数据闪烁优先于主模式
  if (g_data_flash_active) {
    if (millis() - g_data_flash_start < DATA_FLASH_MS) {
      digitalWrite(PIN_LED2, HIGH);  // 短暂亮一下
    } else {
      g_data_flash_active = false;
      // 恢复 LED2 主模式显示
      led_tick_slot(&g_led2, PIN_LED2);
    }
  } else {
    led_tick_slot(&g_led2, PIN_LED2);
  }
}
