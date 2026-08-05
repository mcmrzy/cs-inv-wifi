/*
 * config.h — CS-L10-6K2 采集器配置 (ESP32-C3)
 *
 * 与 esp32c2_idf 相比: 无 BLE/AP 配网/OTA, 纯 WiFi STA + MQTT + 串口轮询
 */
#ifndef CONFIG_H
#define CONFIG_H

#include "sdkconfig.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ---- 串口 (UART0 接 ARM 采集器接口, console 日志走 USB-Serial/JTAG) ---- */
#define PIN_UART_TX      CONFIG_CS_INV_UART_TX_PIN
#define PIN_UART_RX      CONFIG_CS_INV_UART_RX_PIN
#define UART_BAUDRATE    CONFIG_CS_INV_UART_BAUD
#define UART_PORT_NUM    UART_NUM_0

/* 软件反相补偿: 1=UART 硬件极性反转 (空闲/起始/停止位一并反转) */
/* 用户决定: TX 与 RX 均反相 */
#define UART_INVERT_TX   1
#define UART_INVERT_RX   1

/* ---- ARM OTA 帧协议 (与 esp32c3_idf/Arduino 版一致, 兼容 ARM 端 IAP) ---- */
#define FRAME_HEADER     0xAA
#define FRAME_ESCAPE     0x55
#define FRAME_ESC_HEADER 0x01
#define FRAME_ESC_ESCAPE 0x00
#define FRAME_DATA_MAX   1024

#define CMD_ACK          0xFE
#define CMD_NACK         0xFF

#define CMD_ARM_OTA_START    0x10
#define CMD_ARM_OTA_DATA     0x11
#define CMD_ARM_OTA_END      0x12
#define CMD_ARM_OTA_ACK      0x13
#define CMD_ARM_OTA_NACK     0x14
#define CMD_ARM_OTA_INFO     0x15

/* ---- WiFi ---- */
#define WIFI_DEFAULT_SSID   CONFIG_CS_INV_WIFI_SSID
#define WIFI_DEFAULT_PASS   CONFIG_CS_INV_WIFI_PASS
#define WIFI_RECONNECT_RETRY_MAX  10
#define WIFI_CONNECT_TIMEOUT_MS   30000

/* ---- MQTT ---- */
#define MQTT_DEFAULT_HOST   CONFIG_CS_INV_MQTT_HOST
#define MQTT_DEFAULT_PORT   CONFIG_CS_INV_MQTT_PORT
#define MQTT_KEEPALIVE      60
#define MQTT_RECONNECT_MIN  1000
#define MQTT_RECONNECT_MAX  60000
#define MQTT_RECONNECT_MAX_ATTEMPTS 10
#define MQTT_USERNAME       CONFIG_CS_INV_MQTT_USERNAME
#define MQTT_PASSWORD       CONFIG_CS_INV_MQTT_PASSWORD

/* ---- NVS ---- */
#define NVS_NAMESPACE       "inv_cfg"
#define KEY_WIFI_SSID       "ws"
#define KEY_WIFI_PASS       "wp"
#define KEY_MQTT_HOST       "m_host"
#define KEY_MQTT_PORT       "m_port"
#define KEY_DEVICE_SN       "dev_sn"
#define KEY_DEVICE_KEY      "dev_key"
#define KEY_OTA_SEC_VER     "ota_sec_ver"

/* ---- 设备 SN ---- */
#define SN_MAX_LEN     32
#define SN_DEV_PREFIX  "L10"   /* MAC 派生 SN 前缀, 如 L10A1B2C3D4E5 */

/* ---- 离线缓存 ---- */
#define OFFLINE_CACHE_MAX_ITEMS  CONFIG_CS_INV_CACHE_MAX_ITEMS
#define OFFLINE_CACHE_TTL_DAYS   CONFIG_CS_INV_CACHE_TTL_DAYS
#define OFFLINE_CACHE_ENABLED    1

/* ---- 版本 ---- */
#define FIRMWARE_VERSION "V1.2.0.L10"

/* ---- 上报周期 (ms) ---- */
#define TELEMETRY_PERIOD_MS   180000  /* heartbeat 周期上报 (3 分钟, V2.0 协议) */

#define MIN(a, b) ((a) < (b) ? (a) : (b))

#endif
