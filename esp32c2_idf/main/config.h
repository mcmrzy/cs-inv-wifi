#ifndef CONFIG_H
#define CONFIG_H

#include "sdkconfig.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define PIN_UART_TX      CONFIG_CS_INV_UART_TX_PIN
#define PIN_UART_RX      CONFIG_CS_INV_UART_RX_PIN
#define PIN_LED1         CONFIG_CS_INV_LED1_PIN
#define PIN_LED2         CONFIG_CS_INV_LED2_PIN
#define PIN_BTN          CONFIG_CS_INV_BTN_PIN
#define UART_BAUDRATE    CONFIG_CS_INV_UART_BAUD
#define UART_PORT_NUM    UART_NUM_1

#define FRAME_HEADER     0xAA
#define FRAME_ESCAPE     0x55
#define FRAME_ESC_HEADER 0x01
#define FRAME_ESC_ESCAPE 0x00
#define FRAME_DATA_MAX   1024
#define FRAME_TOTAL_MAX  (FRAME_DATA_MAX + 4)

#define CMD_SET_BROKER      0x01
#define CMD_PUBLISH         0x02
#define CMD_COMMAND_RECV    0x03
#define CMD_COMMAND_SEND    0x04
#define CMD_FACTORY_RESET   0x05
#define CMD_HEARTBEAT       0x06
#define CMD_SET_SN          0x08
#define CMD_SET_KEY         0x09
#define CMD_SET_AP_SSID     0x0A
#define CMD_ACK             0xFE
#define CMD_NACK            0xFF

#define CMD_ARM_OTA_START    0x10
#define CMD_ARM_OTA_DATA     0x11
#define CMD_ARM_OTA_END      0x12
#define CMD_ARM_OTA_ACK      0x13
#define CMD_ARM_OTA_NACK     0x14
#define CMD_ARM_OTA_INFO     0x15

enum SystemState {
    STATE_INIT,
    STATE_AP_PROVISION,
    STATE_STA_CONNECTING,
    STATE_MQTT_CONNECTING,
    STATE_RUNNING,
    STATE_OTA
};

#define MQTT_DEFAULT_HOST   CONFIG_CS_INV_MQTT_HOST
#define MQTT_DEFAULT_PORT   CONFIG_CS_INV_MQTT_PORT
#define MQTT_KEEPALIVE      60
#define MQTT_RECONNECT_MIN  1000
#define MQTT_RECONNECT_MAX  60000
#define MQTT_RECONNECT_MAX_ATTEMPTS 10
#define MQTT_USERNAME       CONFIG_CS_INV_MQTT_USERNAME
#define MQTT_PASSWORD       CONFIG_CS_INV_MQTT_PASSWORD

#define NVS_NAMESPACE       "inv_cfg"
#define KEY_WIFI_SSID       "ws"
#define KEY_WIFI_PASS       "wp"
#define KEY_MQTT_HOST       "m_host"
#define KEY_MQTT_PORT       "m_port"
#define KEY_DEVICE_SN       "dev_sn"
#define KEY_DEVICE_KEY      "dev_key"
#define KEY_AP_SSID         "ap_ssid"

#define AP_SSID_MAX_LEN     32
#define AP_SSID_PREFIX      "CS_INV_"

#define BUTTON_LONG_PRESS   5000
#define HEARTBEAT_INTERVAL  60000
#define PROVISION_TCP_PORT  80

#define OFFLINE_CACHE_MAX_ITEMS  CONFIG_CS_INV_CACHE_MAX_ITEMS
#define OFFLINE_CACHE_TTL_DAYS   CONFIG_CS_INV_CACHE_TTL_DAYS
#define OFFLINE_CACHE_ENABLED    1

#define FIRMWARE_VERSION "V2.0.0.ESP-IDF"

#define SN_MAX_LEN     32
#define SN_STD_LEN     16
#define SN_DEV_PREFIX  "MAC"

#define MIN(a, b) ((a) < (b) ? (a) : (b))

#endif
