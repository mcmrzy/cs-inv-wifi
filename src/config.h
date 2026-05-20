#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>

// ==================== 芯片选择 ====================
// 注释掉下面这行即为 ESP8266，取消注释为 ESP32-C3
#define CHIP_ESP32C3

// ==================== 引脚定义 ====================
#ifdef CHIP_ESP32C3
  // ----- ESP32-C3 -----
  // GPIO0 = UART1_TX, GPIO1 = UART1_RX → 连接 ARM MCU
  #define PIN_UART_TX     0    // UART TX → ARM MCU RX
  #define PIN_UART_RX     1    // UART RX → ARM MCU TX
  
  // 辅助 LED（可选，不影响运行）
  #define PIN_LED1        12   // LED1（状态指示，高电平有效）
  #define PIN_LED2        13   // LED2（状态指示，高电平有效）
  #define PIN_BTN         9    // BOOT 按键（配网/恢复出厂，可选）
  #define WIFI_LED_CHANNEL 0   // LEDC 通道（用于呼吸灯）
  #define UART_NUM        1    // HardwareSerial 编号（仅 USE_HW_UART 模式使用）
  // ESP32-C3: 使用 Preferences（NVS）
  #define USE_PREFERENCES
#else
  // ----- ESP8266-07S -----
  #define PIN_UART_TX     1    // UART TX（默认Serial口）
  #define PIN_UART_RX     3    // UART RX
  #define PIN_LED1        4    // LED
  #define PIN_LED2        5    // 另一个 LED（可选）
  #define PIN_BTN         0    // 按键
  #define UART_NUM        0    // ESP8266 只有一个硬件串口（Serial）
  // ESP8266: 使用 EEPROM（模拟 NVS）
  #define USE_EEPROM
#endif

// ==================== 帧协议常量 ====================
#define FRAME_HEADER     0xAA
#define FRAME_ESCAPE     0x55
#define FRAME_ESC_HEADER 0x01
#define FRAME_ESC_ESCAPE 0x00
#define FRAME_DATA_MAX   512
#define FRAME_TOTAL_MAX  (FRAME_DATA_MAX + 4)  // HEADER + CMD + LEN_H + LEN_L + DATA... + XOR

// ==================== 命令码 ====================
#define CMD_SET_BROKER      0x01  // 设置 MQTT 代理
#define CMD_PUBLISH         0x02  // 发布 MQTT 消息
#define CMD_COMMAND_RECV    0x03  // 收到 MQTT 命令（ESP→ARM）
#define CMD_COMMAND_SEND    0x04  // 发送控制命令结果（ARM→ESP）
#define CMD_FACTORY_RESET   0x05  // 恢复出厂
#define CMD_HEARTBEAT       0x06  // 心跳
#define CMD_SET_SN          0x08  // 设置序列号
#define CMD_SET_KEY         0x09  // 设置设备密钥
#define CMD_SET_AP_SSID     0x0A  // 设置热点名称（App通过MQTT下发）
#define CMD_ACK             0xFE  // ACK
#define CMD_NACK            0xFF  // NACK

// ==================== 状态机 ====================
enum SystemState {
  STATE_INIT,             // 初始化
  STATE_AP_PROVISION,     // AP 配网模式
  STATE_STA_CONNECTING,   // 正在连接 WiFi
  STATE_MQTT_CONNECTING,  // 正在连接 MQTT
  STATE_RUNNING,          // 正常运行
  STATE_OTA              // OTA 升级
};

// LED 状态枚举已移至 led_status.h（LedPattern），避免重复定义
// ==================== MQTT 配置 ====================
#define MQTT_DEFAULT_HOST   "jiuxiaoyw.online"
#define MQTT_DEFAULT_PORT   8883   // TLS 加密端口 (mqtts://)
#define MQTT_KEEPALIVE      60
#define MQTT_RECONNECT_MIN  1000
#define MQTT_RECONNECT_MAX  60000
#define MQTT_RECONNECT_MAX_ATTEMPTS 10

// MQTT 认证凭据 (固定用户名/密码)
#define MQTT_USERNAME       "CSKJ-INV-DEVICE-6K2"
#define MQTT_PASSWORD       "CSKJINVDEVICE6K2"

// ==================== NVS / EEPROM 命名空间 ====================
#define NVS_NAMESPACE "inv_cfg"
#define KEY_WIFI_SSID     "ws"      // WiFi SSID（明文）
#define KEY_WIFI_PASS     "wp"      // WiFi 密码（明文）
#define KEY_MQTT_HOST     "m_host"
#define KEY_MQTT_PORT     "m_port"
#define KEY_DEVICE_SN     "dev_sn"
#define KEY_DEVICE_KEY    "dev_key"
#define KEY_AP_SSID       "ap_ssid"   // 自定义热点名称
#define KEY_IPGEO_HOST    "igeo_h"    // IP 定位 API 主机
#define KEY_IPGEO_PORT    "igeo_p"    // IP 定位 API 端口
#define KEY_IPGEO_PATH    "igeo_u"    // IP 定位 API 路径

// ==================== AP 配置常量 ====================
#define AP_SSID_MAX_LEN   32
#define AP_SSID_PREFIX    "CS_INV_"

// ==================== IP 定位 API 默认配置 ====================
#define IPGEO_DEFAULT_HOST  "ipapi.co"
#define IPGEO_DEFAULT_PORT  443
#define IPGEO_DEFAULT_PATH  "/json/"
#define IPGEO_HOST_MAX_LEN  64
#define IPGEO_PATH_MAX_LEN  64

// ==================== 硬件配置 ====================
#define UART_BAUDRATE       115200
#define BUTTON_LONG_PRESS   5000
#define HEARTBEAT_INTERVAL  60000
#define PROVISION_AP_SSID_PREFIX "CS_INV_"
#define PROVISION_TCP_PORT  8888

// ==================== 宏工具 ====================
#define SN_MAX_LEN 32      // 兼容旧代码的最大缓冲区 (标准 SN 为 16 字符)

// SN 编码规则常量 (遵循 CS-SN-STD-001 V1.1)
#define SN_STD_LEN       16   // 标准 SN 长度: 16 字符
#define SN_DEV_PREFIX    "MAC" // 开发模式 SN 前缀 (无 ARM 时的 MAC 占位)

#endif // CONFIG_H
