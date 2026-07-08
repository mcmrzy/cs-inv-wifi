# 设备端 OTA 程序开发指南

**版本**: V1.0
**适用设备**: CS-I10-6k2 48V 单相离网逆变器
**更新时间**: 2026-06-04

---

## 一、系统架构概述

```
┌─────────────────┐     UART      ┌─────────────────┐    MQTT    ┌──────────────────┐
│    ARM MCU      │◄────────────►│   ESP32-C3      │◄─────────►│  后端服务器      │
│ (逆变器主控)     │  二进制帧协议   │ (WiFi网关)       │            │  (云端/APP)      │
│ GD32F303        │              │  透明转发         │            │                  │
└─────────────────┘              └─────────────────┘            └──────────────────┘
        │                                │
        │                                │
        ▼                                ▼
   固件存储区                        WiFi + MQTT
   (Flash)                          连接管理
```

**OTA 升级流程**:
1. 云端通过 MQTT 下发 OTA 命令到 ESP32
2. ESP32 通过 UART 转发给 ARM
3. ARM 从云端 HTTP 下载固件
4. ARM 写入 Flash 并校验
5. ARM 通过 ESP32 上报升级进度

---

## 二、ESP32 端程序实现

### 2.1 MQTT 连接配置

```c
// mqtt_config.h
#ifndef MQTT_CONFIG_H
#define MQTT_CONFIG_H

// MQTT 连接参数
#define MQTT_BROKER         "jiuxiaoyw.online"
#define MQTT_PORT           8883
#define MQTT_USERNAME       "CSKJ-INV-DEVICE-6K2"
#define MQTT_PASSWORD       "CSKJINVDEVICE6K2"
#define MQTT_KEEPALIVE      60
#define MQTT_CLEAN_SESSION  false

// 主题格式
#define TOPIC_PREFIX        "cs_inv/"
#define TOPIC_CMD           "/cmd"
#define TOPIC_OTA_CMD       "/ota/cmd"
#define TOPIC_STATUS        "/status"
#define TOPIC_OTA_STATUS    "/ota/status"
#define TOPIC_CMD_RESPONSE  "/cmd/response"

// 心跳间隔
#define HEARTBEAT_INTERVAL  60000  // 60秒

#endif
```

### 2.2 MQTT 主题订阅

```c
// mqtt_topics.c
#include "mqtt_client.h"
#include "mqtt_config.h"
#include "nvs_flash.h"

static char device_sn[17] = {0};

// 从 NVS 读取设备 SN
void load_device_sn(void) {
    nvs_handle_t handle;
    if (nvs_open("device", NVS_READONLY, &handle) == ESP_OK) {
        size_t len = sizeof(device_sn);
        nvs_get_str(handle, "sn", device_sn, &len);
        nvs_close(handle);
    }
}

// 订阅设备相关主题
void subscribe_device_topics(esp_mqtt_client_handle_t client) {
    char topic[64];
    
    // 订阅控制命令主题
    snprintf(topic, sizeof(topic), "%s%s%s", TOPIC_PREFIX, device_sn, TOPIC_CMD);
    esp_mqtt_client_subscribe(client, topic, 1);
    
    // 订阅 OTA 命令主题
    snprintf(topic, sizeof(topic), "%s%s%s", TOPIC_PREFIX, device_sn, TOPIC_OTA_CMD);
    esp_mqtt_client_subscribe(client, topic, 1);
    
    ESP_LOGI(TAG, "Subscribed to topics for SN: %s", device_sn);
}

// 发布消息
void publish_mqtt_message(esp_mqtt_client_handle_t client, 
                          const char* sub_topic, 
                          const char* payload) {
    char topic[64];
    snprintf(topic, sizeof(topic), "%s%s/%s", TOPIC_PREFIX, device_sn, sub_topic);
    esp_mqtt_client_publish(client, topic, payload, 0, 1, false);
}
```

### 2.3 状态上报

```c
// status_report.c
#include "mqtt_client.h"
#include "mqtt_config.h"
#include "esp_wifi.h"
#include "esp_timer.h"

// 上报在线状态（每60秒）
void report_online_status(esp_mqtt_client_handle_t client) {
    wifi_ap_record_t ap_info;
    esp_wifi_sta_get_ap_info(&ap_info);
    
    char payload[128];
    snprintf(payload, sizeof(payload),
        "{\"online\":true,\"rssi\":%d,\"ip\":\"%s\"}",
        ap_info.rssi,
        get_local_ip());
    
    publish_mqtt_message(client, TOPIC_STATUS, payload);
}

// 上报 OTA 状态
void report_ota_status(esp_mqtt_client_handle_t client, 
                       const char* target,
                       const char* state,
                       int progress,
                       const char* message) {
    char payload[256];
    snprintf(payload, sizeof(payload),
        "{\"target\":\"%s\",\"state\":\"%s\",\"progress\":%d,\"message\":\"%s\"}",
        target, state, progress, message);
    
    publish_mqtt_message(client, TOPIC_OTA_STATUS, payload);
}
```

### 2.4 OTA 命令处理

```c
// ota_handler.c
#include "mqtt_client.h"
#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"

typedef struct {
    char target[8];      // "esp" 或 "arm"
    char url[256];       // 固件下载 URL
    int firmware_size;
    char firmware_md5[33];
    char firmware_sha256[65];
    char task_id[64];
} ota_command_t;

// 解析 OTA 命令
bool parse_ota_command(const char* json_str, ota_command_t* cmd) {
    cJSON* root = cJSON_Parse(json_str);
    if (!root) return false;
    
    cJSON* topic = cJSON_GetObjectItem(root, "topic");
    cJSON* payload = cJSON_GetObjectItem(root, "payload");
    
    if (!topic || !payload) {
        cJSON_Delete(root);
        return false;
    }
    
    // 解析 payload（注意：payload 是 JSON 字符串）
    cJSON* payload_json = cJSON_Parse(payload->valuestring);
    if (!payload_json) {
        cJSON_Delete(root);
        return false;
    }
    
    // 读取字段
    cJSON* target = cJSON_GetObjectItem(payload_json, "target");
    cJSON* url = cJSON_GetObjectItem(payload_json, "url");
    cJSON* version = cJSON_GetObjectItem(payload_json, "version");
    cJSON* file_md5 = cJSON_GetObjectItem(payload_json, "file_md5");
    cJSON* file_sha256 = cJSON_GetObjectItem(payload_json, "file_sha256");
    cJSON* task_id = cJSON_GetObjectItem(payload_json, "task_id");
    
    if (target) strncpy(cmd->target, target->valuestring, sizeof(cmd->target)-1);
    if (url) strncpy(cmd->url, url->valuestring, sizeof(cmd->url)-1);
    if (file_md5) strncpy(cmd->firmware_md5, file_md5->valuestring, sizeof(cmd->firmware_md5)-1);
    if (file_sha256) strncpy(cmd->firmware_sha256, file_sha256->valuestring, sizeof(cmd->firmware_sha256)-1);
    if (task_id) strncpy(cmd->task_id, task_id->valuestring, sizeof(cmd->task_id)-1);
    
    cJSON_Delete(payload_json);
    cJSON_Delete(root);
    return true;
}

// ESP32 自身 OTA 升级
void perform_esp_ota(esp_mqtt_client_handle_t client, const ota_command_t* cmd) {
    report_ota_status(client, "esp", "starting", 0, "开始升级");
    
    // 1. 配置 HTTP 客户端
    esp_http_client_config_t http_config = {
        .url = cmd->url,
        .timeout_ms = 30000,
    };
    
    // 2. 配置 OTA
    esp_ota_config_t ota_config = {
        .max_http_request_size = 4096,
    };
    
    // 3. 执行 OTA
    esp_err_t ret = esp_https_ota(&http_config);
    
    if (ret == ESP_OK) {
        report_ota_status(client, "esp", "done", 100, "升级完成，准备重启");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    } else {
        report_ota_status(client, "esp", "error", 0, "升级失败");
    }
}

// ARM OTA 升级（通过 UART 转发）
void perform_arm_ota(esp_mqtt_client_handle_t client, const ota_command_t* cmd) {
    report_ota_status(client, "arm", "starting", 0, "开始ARM升级");
    
    // 1. 下载固件到缓冲区
    uint8_t* firmware_buffer = NULL;
    int firmware_size = 0;
    
    if (download_firmware(cmd->url, &firmware_buffer, &firmware_size) != ESP_OK) {
        report_ota_status(client, "arm", "error", 0, "固件下载失败");
        return;
    }
    
    report_ota_status(client, "arm", "downloading", 10, "固件下载完成");
    
    // 2. 通过 UART 发送 OTA 开始命令
    if (!send_arm_ota_start(firmware_size, cmd->firmware_md5, cmd->task_id)) {
        report_ota_status(client, "arm", "error", 0, "ARM OTA 启动失败");
        free(firmware_buffer);
        return;
    }
    
    // 3. 分包发送固件数据
    int packet_size = 480;
    int total_packets = (firmware_size + packet_size - 1) / packet_size;
    
    for (int i = 0; i < total_packets; i++) {
        int offset = i * packet_size;
        int len = (firmware_size - offset) > packet_size ? packet_size : (firmware_size - offset);
        
        if (!send_arm_ota_data(i, firmware_buffer + offset, len)) {
            report_ota_status(client, "arm", "error", 0, "数据传输失败");
            free(firmware_buffer);
            return;
        }
        
        int progress = (i * 100) / total_packets;
        report_ota_status(client, "arm", "uploading", progress, "传输中");
    }
    
    // 4. 发送 OTA 结束命令
    if (!send_arm_ota_end(total_packets, firmware_size, cmd->firmware_md5)) {
        report_ota_status(client, "arm", "error", 0, "ARM OTA 结束命令失败");
        free(firmware_buffer);
        return;
    }
    
    report_ota_status(client, "arm", "done", 100, "ARM 升级完成");
    free(firmware_buffer);
}

// MQTT 消息处理回调
void handle_mqtt_message(esp_mqtt_client_handle_t client, 
                         const char* topic, 
                         const char* data) {
    // 检查是否是 OTA 命令
    if (strstr(topic, TOPIC_OTA_CMD) != NULL) {
        ota_command_t cmd;
        if (parse_ota_command(data, &cmd)) {
            if (strcmp(cmd.target, "esp") == 0) {
                perform_esp_ota(client, &cmd);
            } else if (strcmp(cmd.target, "arm") == 0) {
                perform_arm_ota(client, &cmd);
            }
        }
    }
    // 检查是否是普通控制命令
    else if (strstr(topic, TOPIC_CMD) != NULL) {
        // 转发给 ARM 处理
        forward_command_to_arm(data);
    }
}
```

---

## 三、ARM 端程序实现

### 3.1 UART 帧协议实现

```c
// uart_protocol.h
#ifndef UART_PROTOCOL_H
#define UART_PROTOCOL_H

#include <stdint.h>

// 帧头和转义字符
#define FRAME_HEADER     0xAA
#define FRAME_ESCAPE     0x55
#define FRAME_ESC_HEADER 0x01
#define FRAME_ESC_ESCAPE 0x00
#define FRAME_DATA_MAX   512

// 命令码定义
#define CMD_SET_BROKER      0x01
#define CMD_PUBLISH         0x02
#define CMD_COMMAND_RECV    0x03
#define CMD_COMMAND_SEND    0x04
#define CMD_FACTORY_RESET   0x05
#define CMD_HEARTBEAT       0x06
#define CMD_SET_SN          0x08
#define CMD_SET_KEY         0x09
#define CMD_SET_AP_SSID     0x0A
#define CMD_OTA_START       0x10
#define CMD_OTA_DATA        0x11
#define CMD_OTA_END         0x12
#define CMD_OTA_ACK         0x13
#define CMD_OTA_NACK        0x14
#define CMD_OTA_INFO        0x15
#define CMD_ACK             0xFE
#define CMD_NACK            0xFF

// 帧结构
typedef struct {
    uint8_t cmd;
    uint16_t len;
    uint8_t data[FRAME_DATA_MAX];
} uart_frame_t;

// 接收状态机
typedef enum {
    RX_WAIT_HEADER,
    RX_WAIT_CMD,
    RX_WAIT_LEN_H,
    RX_WAIT_LEN_L,
    RX_WAIT_DATA,
    RX_WAIT_XOR
} rx_state_t;

// 接收上下文
typedef struct {
    rx_state_t state;
    uint8_t cmd;
    uint16_t len;
    uint16_t recv_len;
    uint8_t data[FRAME_DATA_MAX];
    uint8_t xor;
    uint8_t esc_next;
} rx_context_t;

#endif
```

### 3.2 UART 发送接收实现

```c
// uart_protocol.c
#include "uart_protocol.h"
#include "uart.h"

// 计算 XOR 校验和
uint8_t calc_xor(uint8_t cmd, uint16_t len, const uint8_t* data) {
    uint8_t xor = FRAME_HEADER;
    xor ^= cmd;
    xor ^= (uint8_t)((len >> 8) & 0xFF);
    xor ^= (uint8_t)(len & 0xFF);
    for (uint16_t i = 0; i < len; i++) {
        xor ^= data[i];
    }
    return xor;
}

// 发送转义字节
static void send_escaped_byte(uint8_t b) {
    if (b == FRAME_HEADER) {
        uart_send_byte(FRAME_ESCAPE);
        uart_send_byte(FRAME_ESC_HEADER);
    } else if (b == FRAME_ESCAPE) {
        uart_send_byte(FRAME_ESCAPE);
        uart_send_byte(FRAME_ESC_ESCAPE);
    } else {
        uart_send_byte(b);
    }
}

// 发送完整帧
void send_frame(uint8_t cmd, const uint8_t* data, uint16_t len) {
    uint8_t xor = calc_xor(cmd, len, data);
    
    // 发送帧头（不转义）
    uart_send_byte(FRAME_HEADER);
    
    // 发送 CMD（需要转义）
    send_escaped_byte(cmd);
    
    // 发送长度（需要转义）
    send_escaped_byte((uint8_t)((len >> 8) & 0xFF));
    send_escaped_byte((uint8_t)(len & 0xFF));
    
    // 发送数据（需要转义）
    for (uint16_t i = 0; i < len; i++) {
        send_escaped_byte(data[i]);
    }
    
    // 发送校验和（需要转义）
    send_escaped_byte(xor);
}

// 接收状态机初始化
void rx_init(rx_context_t* ctx) {
    memset(ctx, 0, sizeof(rx_context_t));
    ctx->state = RX_WAIT_HEADER;
}

// 处理接收到的字节
int rx_process_byte(rx_context_t* ctx, uint8_t b, 
                    void (*on_frame)(uint8_t cmd, const uint8_t* data, uint16_t len)) {
    // 处理转义
    if (ctx->esc_next) {
        ctx->esc_next = 0;
        if (b == 0x01) b = FRAME_HEADER;
        else if (b == 0x00) b = FRAME_ESCAPE;
    } else if (b == FRAME_ESCAPE) {
        ctx->esc_next = 1;
        return 0;
    }
    
    switch (ctx->state) {
        case RX_WAIT_HEADER:
            if (b == FRAME_HEADER) {
                ctx->state = RX_WAIT_CMD;
                ctx->xor = FRAME_HEADER;
            }
            break;
            
        case RX_WAIT_CMD:
            ctx->cmd = b;
            ctx->xor ^= b;
            ctx->state = RX_WAIT_LEN_H;
            break;
            
        case RX_WAIT_LEN_H:
            ctx->len = ((uint16_t)b) << 8;
            ctx->xor ^= b;
            ctx->state = RX_WAIT_LEN_L;
            break;
            
        case RX_WAIT_LEN_L:
            ctx->len |= b;
            ctx->xor ^= b;
            ctx->recv_len = 0;
            if (ctx->len > FRAME_DATA_MAX) {
                ctx->state = RX_WAIT_HEADER;  // 长度错误，重置
            } else if (ctx->len == 0) {
                ctx->state = RX_WAIT_XOR;
            } else {
                ctx->state = RX_WAIT_DATA;
            }
            break;
            
        case RX_WAIT_DATA:
            ctx->data[ctx->recv_len++] = b;
            ctx->xor ^= b;
            if (ctx->recv_len >= ctx->len) {
                ctx->state = RX_WAIT_XOR;
            }
            break;
            
        case RX_WAIT_XOR:
            if (b == ctx->xor) {
                // 校验通过，处理命令
                on_frame(ctx->cmd, ctx->data, ctx->len);
            }
            ctx->state = RX_WAIT_HEADER;
            break;
    }
    
    return 0;
}
```

### 3.3 OTA 接收处理

```c
// arm_ota.c
#include "uart_protocol.h"
#include "flash.h"
#include "md5.h"
#include <string.h>

// OTA 状态
typedef enum {
    OTA_IDLE,
    OTA_STARTED,
    OTA_RECEIVING,
    OTA_VERIFYING,
    OTA_DONE,
    OTA_ERROR
} ota_state_t;

// OTA 上下文
typedef struct {
    ota_state_t state;
    uint32_t total_size;
    uint32_t received_size;
    uint16_t total_packets;
    uint16_t received_packets;
    uint8_t expected_md5[16];
    char version[32];
    uint32_t flash_addr;
    md5_context_t md5_ctx;
} ota_context_t;

static ota_context_t ota_ctx;

// 发送 OTA 应答
void send_ota_ack(uint8_t cmd) {
    if (cmd == CMD_ACK) {
        send_frame(CMD_OTA_ACK, NULL, 0);
    } else {
        send_frame(CMD_OTA_NACK, NULL, 0);
    }
}

// 处理 OTA 开始命令
void handle_ota_start(const uint8_t* data, uint16_t len) {
    if (len < 21) {  // 4 + 16 + 1 + version
        send_ota_ack(CMD_NACK);
        return;
    }
    
    // 解析数据
    // [total_size:4B][md5:16B][version_len:1B][version_str:nB]
    ota_ctx.total_size = (data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3];
    memcpy(ota_ctx.expected_md5, data + 4, 16);
    
    uint8_t version_len = data[20];
    if (version_len > 0 && version_len < sizeof(ota_ctx.version)) {
        memcpy(ota_ctx.version, data + 21, version_len);
        ota_ctx.version[version_len] = '\0';
    }
    
    // 初始化 OTA
    ota_ctx.state = OTA_STARTED;
    ota_ctx.received_size = 0;
    ota_ctx.received_packets = 0;
    ota_ctx.flash_addr = OTA_FLASH_START_ADDR;
    
    // 初始化 MD5
    md5_init(&ota_ctx.md5_ctx);
    
    // 擦除 Flash
    flash_erase(ota_ctx.flash_addr, ota_ctx.total_size);
    
    send_ota_ack(CMD_ACK);
}

// 处理 OTA 数据包
void handle_ota_data(const uint8_t* data, uint16_t len) {
    if (ota_ctx.state != OTA_STARTED && ota_ctx.state != OTA_RECEIVING) {
        send_ota_ack(CMD_NACK);
        return;
    }
    
    if (len < 2) {  // packet_index + data
        send_ota_ack(CMD_NACK);
        return;
    }
    
    // 解析包序号
    uint16_t packet_index = (data[0] << 8) | data[1];
    
    // 验证包序号
    if (packet_index != ota_ctx.received_packets) {
        send_ota_ack(CMD_NACK);
        return;
    }
    
    // 写入 Flash
    uint16_t data_len = len - 2;
    flash_write(ota_ctx.flash_addr + ota_ctx.received_size, data + 2, data_len);
    
    // 更新 MD5
    md5_update(&ota_ctx.md5_ctx, data + 2, data_len);
    
    // 更新计数
    ota_ctx.received_size += data_len;
    ota_ctx.received_packets++;
    ota_ctx.state = OTA_RECEIVING;
    
    send_ota_ack(CMD_ACK);
}

// 处理 OTA 结束命令
void handle_ota_end(const uint8_t* data, uint16_t len) {
    if (ota_ctx.state != OTA_RECEIVING) {
        send_ota_ack(CMD_NACK);
        return;
    }
    
    if (len < 22) {  // total_packets + total_size + md5
        send_ota_ack(CMD_NACK);
        return;
    }
    
    // 解析结束信息
    uint16_t total_packets = (data[0] << 8) | data[1];
    uint32_t total_size = (data[2] << 24) | (data[3] << 16) | (data[4] << 8) | data[5];
    uint8_t md5[16];
    memcpy(md5, data + 6, 16);
    
    // 验证
    if (total_packets != ota_ctx.received_packets || 
        total_size != ota_ctx.received_size) {
        send_ota_ack(CMD_NACK);
        ota_ctx.state = OTA_ERROR;
        return;
    }
    
    // 计算并验证 MD5
    uint8_t computed_md5[16];
    md5_final(&ota_ctx.md5_ctx, computed_md5);
    
    if (memcmp(computed_md5, md5, 16) != 0) {
        send_ota_ack(CMD_NACK);
        ota_ctx.state = OTA_ERROR;
        return;
    }
    
    // 校验通过
    ota_ctx.state = OTA_DONE;
    send_ota_ack(CMD_ACK);
    
    // 设置新固件标志，下次重启时跳转
    set_pending_boot_flag(ota_ctx.flash_addr, ota_ctx.total_size);
}

// 处理 OTA 信息查询
void handle_ota_info(void) {
    char info[64];
    snprintf(info, sizeof(info), 
             "{\"version\":\"%s\",\"md5\":\"%s\"}",
             get_current_firmware_version(),
             get_current_firmware_md5());
    
    send_frame(CMD_OTA_INFO, (uint8_t*)info, strlen(info));
}

// UART 帧处理回调
void on_uart_frame(uint8_t cmd, const uint8_t* data, uint16_t len) {
    switch (cmd) {
        case CMD_OTA_START:
            handle_ota_start(data, len);
            break;
            
        case CMD_OTA_DATA:
            handle_ota_data(data, len);
            break;
            
        case CMD_OTA_END:
            handle_ota_end(data, len);
            break;
            
        case CMD_OTA_INFO:
            handle_ota_info();
            break;
            
        case CMD_COMMAND_RECV:
            handle_cloud_command(data, len);
            break;
            
        case CMD_HEARTBEAT:
            // ESP32 心跳，无需处理
            break;
            
        case CMD_ACK:
            // 命令应答成功
            break;
            
        case CMD_NACK:
            // 命令应答失败
            break;
    }
}
```

### 3.4 数据上报实现

```c
// data_report.c
#include "uart_protocol.h"
#include "cJSON.h"

// 发布数据到云端
void publish_data(const char* topic, cJSON* payload) {
    char* payload_str = cJSON_PrintUnformatted(payload);
    
    // 构造完整 JSON
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "topic", topic);
    cJSON_AddStringToObject(root, "payload", payload_str);
    cJSON_AddStringToObject(root, "sn", get_device_sn());
    
    char* json_str = cJSON_PrintUnformatted(root);
    
    // 发送帧
    send_frame(CMD_PUBLISH, (uint8_t*)json_str, strlen(json_str));
    
    // 清理
    free(payload_str);
    free(json_str);
    cJSON_Delete(root);
}

// 上报 AC 数据
void report_ac_data(float voltage, float current, float power, 
                    float frequency, float load_percent) {
    cJSON* payload = cJSON_CreateObject();
    cJSON_AddNumberToObject(payload, "voltage", voltage);
    cJSON_AddNumberToObject(payload, "current", current);
    cJSON_AddNumberToObject(payload, "power", power);
    cJSON_AddNumberToObject(payload, "frequency", frequency);
    cJSON_AddNumberToObject(payload, "load_percent", load_percent);
    
    publish_data("data/ac", payload);
    cJSON_Delete(payload);
}

// 上报电池数据
void report_battery_data(float soc, float soh, float voltage, 
                         float current, const char* charge_state) {
    cJSON* payload = cJSON_CreateObject();
    cJSON_AddNumberToObject(payload, "soc", soc);
    cJSON_AddNumberToObject(payload, "soh", soh);
    cJSON_AddNumberToObject(payload, "voltage", voltage);
    cJSON_AddNumberToObject(payload, "current", current);
    cJSON_AddStringToObject(payload, "charge_state", charge_state);
    
    publish_data("data/battery", payload);
    cJSON_Delete(payload);
}

// 上报设备信息
void report_device_info(void) {
    cJSON* payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "sn", get_device_sn());
    cJSON_AddStringToObject(payload, "model", "CS-I10-6k2");
    cJSON_AddStringToObject(payload, "manufacturer", "辰烁科技");
    cJSON_AddStringToObject(payload, "firmware_arm", get_current_firmware_version());
    cJSON_AddStringToObject(payload, "firmware_esp", get_esp_firmware_version());
    cJSON_AddStringToObject(payload, "type", "离网逆变器");
    cJSON_AddNumberToObject(payload, "rated_power", 6200);
    cJSON_AddNumberToObject(payload, "rated_voltage", 220);
    cJSON_AddNumberToObject(payload, "rated_freq", 50.0);
    cJSON_AddNumberToObject(payload, "battery_voltage", 51.2);
    cJSON_AddStringToObject(payload, "battery_type", "LiFePO4");
    cJSON_AddNumberToObject(payload, "cell_count", 16);
    
    publish_data("info", payload);
    cJSON_Delete(payload);
}

// 发送命令执行结果
void send_command_result(const char* cmd, const char* result, 
                         const char* message) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "result", result);
    cJSON_AddStringToObject(root, "cmd", cmd);
    cJSON_AddStringToObject(root, "message", message);
    cJSON_AddNumberToObject(root, "timestamp", get_unix_timestamp());
    
    char* json_str = cJSON_PrintUnformatted(root);
    send_frame(CMD_COMMAND_SEND, (uint8_t*)json_str, strlen(json_str));
    
    free(json_str);
    cJSON_Delete(root);
}
```

---

## 四、完整主循环示例

### 4.1 ESP32 主程序

```c
// main_esp32.c
#include "mqtt_client.h"
#include "uart.h"
#include "wifi.h"
#include "nvs_flash.h"

static esp_mqtt_client_handle_t mqtt_client = NULL;
static rx_context_t uart_rx_ctx;

// UART 接收任务
void uart_rx_task(void* pvParameters) {
    rx_init(&uart_rx_ctx);
    
    uint8_t byte;
    while (1) {
        if (uart_receive_byte(&byte, pdMS_TO_TICKS(100)) == ESP_OK) {
            rx_process_byte(&uart_rx_ctx, byte, on_uart_frame);
        }
    }
}

// 心跳任务
void heartbeat_task(void* pvParameters) {
    while (1) {
        if (mqtt_client && is_mqtt_connected()) {
            report_online_status(mqtt_client);
            
            // 发送心跳到 ARM
            send_frame(CMD_HEARTBEAT, NULL, 0);
        }
        vTaskDelay(pdMS_TO_TICKS(HEARTBEAT_INTERVAL));
    }
}

// MQTT 事件处理
static void mqtt_event_handler(void* handler_args, esp_event_base_t base, 
                               int32_t event_id, void* event_data) {
    esp_mqtt_event_handle_t event = event_data;
    
    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT Connected");
            subscribe_device_topics(mqtt_client);
            report_online_status(mqtt_client);
            break;
            
        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG, "MQTT Data: %.*s", event->topic_len, event->topic);
            handle_mqtt_message(mqtt_client, event->topic, event->data);
            break;
            
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "MQTT Disconnected");
            break;
            
        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT Error");
            break;
    }
}

void app_main(void) {
    // 初始化 NVS
    nvs_flash_init();
    
    // 加载设备 SN
    load_device_sn();
    
    // 初始化 WiFi
    wifi_init();
    
    // 初始化 UART
    uart_init(115200);
    
    // 初始化 MQTT
    esp_mqtt_client_config_t mqtt_config = {
        .uri = "mqtts://jiuxiaoyw.online:8883",
        .username = MQTT_USERNAME,
        .password = MQTT_PASSWORD,
        .client_id = device_sn,
        .keepalive = MQTT_KEEPALIVE,
        .clean_session = MQTT_CLEAN_SESSION,
        .lwt_topic = TOPIC_PREFIX device_sn TOPIC_STATUS,
        .lwt_msg = "{\"online\":false}",
        .lwt_retain = true,
    };
    
    mqtt_client = esp_mqtt_client_init(&mqtt_config);
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, 
                                   mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);
    
    // 创建任务
    xTaskCreate(uart_rx_task, "uart_rx", 4096, NULL, 5, NULL);
    xTaskCreate(heartbeat_task, "heartbeat", 2048, NULL, 3, NULL);
}
```

### 4.2 ARM 主程序

```c
// main_arm.c
#include "uart_protocol.h"
#include "arm_ota.h"
#include "data_report.h"
#include "rtos.h"

static rx_context_t uart_rx_ctx;

// 数据采集任务
void data_collection_task(void* pvParameters) {
    while (1) {
        // 采集 AC 数据
        ac_data_t ac = read_ac_data();
        report_ac_data(ac.voltage, ac.current, ac.power, 
                       ac.frequency, ac.load_percent);
        
        // 采集电池数据
        battery_data_t bat = read_battery_data();
        report_battery_data(bat.soc, bat.soh, bat.voltage, 
                           bat.current, bat.charge_state);
        
        vTaskDelay(pdMS_TO_TICKS(5000));  // 5秒间隔
    }
}

// UART 接收任务
void uart_rx_task(void* pvParameters) {
    rx_init(&uart_rx_ctx);
    
    uint8_t byte;
    while (1) {
        if (uart_receive_byte(&byte, pdMS_TO_TICKS(100)) == OS_OK) {
            rx_process_byte(&uart_rx_ctx, byte, on_uart_frame);
        }
    }
}

// 命令处理任务
void command_process_task(void* pvParameters) {
    while (1) {
        // 检查是否有待处理的命令
        command_t cmd;
        if (dequeue_command(&cmd)) {
            process_command(&cmd);
        }
        
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

int main(void) {
    // 初始化系统
    system_init();
    
    // 初始化 UART
    uart_init(115200);
    
    // 初始化 Flash
    flash_init();
    
    // 发送设备信息
    report_device_info();
    
    // 创建任务
    xTaskCreate(uart_rx_task, "uart_rx", 1024, NULL, 5, NULL);
    xTaskCreate(data_collection_task, "data_col", 2048, NULL, 3, NULL);
    xTaskCreate(command_process_task, "cmd_proc", 1024, NULL, 4, NULL);
    
    // 启动调度器
    vTaskStartScheduler();
    
    return 0;
}
```

---

## 五、Flash 存储布局

```
┌─────────────────────────────────────────────────────────────┐
│ 0x08000000 │ Bootloader (16KB)                              │
├─────────────────────────────────────────────────────────────┤
│ 0x08004000 │ 固件区 A (256KB) - 当前运行固件                  │
├─────────────────────────────────────────────────────────────┤
│ 0x08044000 │ 固件区 B (256KB) - OTA 下载区                   │
├─────────────────────────────────────────────────────────────┤
│ 0x08084000 │ 参数区 (16KB) - 设备配置、OTA 状态               │
├─────────────────────────────────────────────────────────────┤
│ 0x08088000 │ 数据区 (剩余空间) - 历史数据存储                 │
└─────────────────────────────────────────────────────────────┘
```

### Bootloader 判断逻辑

```c
// bootloader.c
typedef struct {
    uint32_t magic;           // 0xOTA_MAGIC
    uint32_t firmware_addr;   // 固件起始地址
    uint32_t firmware_size;   // 固件大小
    uint8_t  firmware_md5[16]; // MD5 校验
    uint8_t  valid;           // 0=无效, 1=有效
} ota_info_t;

void bootloader_main(void) {
    ota_info_t ota_info;
    
    // 读取 OTA 信息
    flash_read(OTA_INFO_ADDR, &ota_info, sizeof(ota_info));
    
    if (ota_info.magic == OTA_MAGIC && ota_info.valid == 1) {
        // 验证固件 MD5
        uint8_t md5[16];
        calculate_md5(ota_info.firmware_addr, ota_info.firmware_size, md5);
        
        if (memcmp(md5, ota_info.firmware_md5, 16) == 0) {
            // MD5 校验通过，跳转到新固件
            jump_to_firmware(ota_info.firmware_addr);
        } else {
            // MD5 校验失败，标记 OTA 失败
            ota_info.valid = 0;
            flash_write(OTA_INFO_ADDR, &ota_info, sizeof(ota_info));
        }
    }
    
    // 跳转到默认固件
    jump_to_firmware(DEFAULT_FIRMWARE_ADDR);
}
```

---

## 六、调试与测试

### 6.1 串口调试命令

```c
// debug_commands.c
void process_debug_command(const char* cmd) {
    if (strcmp(cmd, "status") == 0) {
        printf("OTA State: %d\n", ota_ctx.state);
        printf("Received: %d/%d packets\n", 
               ota_ctx.received_packets, ota_ctx.total_packets);
        printf("Size: %d/%d bytes\n", 
               ota_ctx.received_size, ota_ctx.total_size);
    }
    else if (strcmp(cmd, "reset") == 0) {
        ota_ctx.state = OTA_IDLE;
        printf("OTA reset\n");
    }
    else if (strcmp(cmd, "info") == 0) {
        printf("Firmware: %s\n", get_current_firmware_version());
        printf("MD5: %s\n", get_current_firmware_md5());
    }
}
```

### 6.2 测试流程

1. **单元测试**:
   - UART 帧发送/接收
   - XOR 校验计算
   - 转义处理
   - JSON 解析

2. **集成测试**:
   - MQTT 连接
   - 主题订阅/发布
   - OTA 命令接收
   - 固件下载

3. **系统测试**:
   - 完整 OTA 升级流程
   - 断网恢复
   - 异常处理

---

## 七、常见问题

### 7.1 MQTT 连接失败

**检查项**:
- Broker 地址和端口是否正确
- 用户名密码是否正确
- 设备 SN 是否已设置
- 网络是否正常

### 7.2 OTA 升级失败

**检查项**:
- 固件 URL 是否可访问
- 固件大小是否超过 Flash 容量
- MD5 校验是否正确
- UART 通信是否正常

### 7.3 数据上报异常

**检查项**:
- JSON 格式是否正确
- 字段名是否与协议一致
- 数值范围是否合理
- 上报频率是否过高

---

## 八、参考资源

- `MQTT协议文档.md` - 完整 MQTT 协议定义
- `ARM_ESP32_UART_Protocol.md` - UART 帧协议详细说明
- `系统参数规范_48V离网逆变器.md` - 数据字段定义
- ESP-IDF 官方文档: https://docs.espressif.com/projects/esp-idf/
- GD32 固件库: https://www.gigadevice.com/

---

## 九、本地 HTTP 接口响应格式规范

设备在 AP 模式下通过 HTTP 提供本地接口，供 App 直接访问查询 OTA 状态和设备信息。

### /ota/progress 响应格式

设备在处理 OTA 升级期间和完成后，通过此接口返回升级状态。

**升级中响应**：
```json
{
  "status": "uploading",
  "progress": 45,
  "message": "固件推送中"
}
```

**升级完成响应**（status=done）：
```json
{
  "status": "done",
  "progress": 100,
  "message": "Upgrade successful",
  "version": "V3.0.2.20260701",
  "target_chip": "esp",
  "firmware_esp": "V3.0.2.20260701",
  "firmware_arm": "V1.2.3.20240510",
  "firmware_dsp": "V1.1.0.20240508",
  "firmware_bms": "V2.0.1.20240415",
  "main_version": "V3.0.2.20260701"
}
```

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| status | string | 是 | 状态：uploading/verifying/done/error |
| progress | int | 是 | 进度百分比 0-100 |
| message | string | 否 | 状态描述信息 |
| version | string | 建议 | 当前升级芯片的新版本号 |
| target_chip | string | 建议 | 本次升级的目标芯片 |
| firmware_esp | string | 建议 | ESP32 固件版本号 |
| firmware_arm | string | 建议 | ARM 固件版本号 |
| firmware_dsp | string | 可选 | DSP 固件版本号 |
| firmware_bms | string | 可选 | BMS 固件版本号 |
| main_version | string | 建议 | 设备主版本号（升级包版本） |

### /ota/info 响应格式

设备信息查询接口，返回设备基本信息和各芯片固件版本。

```json
{
  "model": "CS-I10-6k2",
  "sn": "CS1234567890123456",
  "manufacturer": "CSKJ",
  "firmware_esp": "V3.0.2.20260701",
  "firmware_arm": "V1.2.3.20240510",
  "firmware_dsp": "V1.1.0.20240508",
  "firmware_bms": "V2.0.1.20240415",
  "main_version": "V3.0.2.20260701",
  "hardware_version": "V1.0",
  "protocol_version": 2
}
```

**重要说明**：
- `main_version` 字段表示设备当前所属升级包的版本号（格式：`Va.b.c.YYYYMMDD`）
- 每次升级完成后，ESP32 应将新的 `main_version` 持久化到 NVS
- `firmware_dsp` 和 `firmware_bms` 字段如设备不支持可省略或返回空字符串
