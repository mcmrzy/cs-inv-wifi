# ESP32-C2 逆变器WiFi模块程序架构文档

## 1. 项目概述

**项目名称**: cs_inv_wifi_c2  
**固件版本**: V2.0.0.ESP-IDF  
**ESP-IDF版本**: v5.5.4  
**目标芯片**: ESP32-C2 (单核, 120MHz)  

### 功能概述
- WiFi AP/STA双模式运行
- BLE持续广播配网
- MQTT客户端通信
- UART与ARM通信
- OTA固件升级
- 离线数据缓存

---

## 2. 系统架构图

```
┌─────────────────────────────────────────────────────────────┐
│                      ARM 主控 (UART)                         │
└───────────────────────────┬─────────────────────────────────┘
                            │ UART (GPIO0/GPIO1, 115200)
                            ▼
┌─────────────────────────────────────────────────────────────┐
│                    ESP32-C2 WiFi模块                         │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────────────┐  │
│  │ uart_protocol│  │state_machine│  │    app_main         │  │
│  │  (帧协议)    │──│  (状态机)    │──│   (初始化/主循环)   │  │
│  └─────────────┘  └─────────────┘  └─────────────────────┘  │
│         │                │                    │              │
│         ▼                ▼                    ▼              │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────────────┐  │
│  │nvs_storage  │  │mqtt_handler │  │   ble_prov          │  │
│  │ (NVS存储)   │  │ (MQTT客户端)│  │  (BLE配网)          │  │
│  └─────────────┘  └─────────────┘  └─────────────────────┘  │
│         │                │                    │              │
│         ▼                ▼                    ▼              │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────────────┐  │
│  │offline_cache│  │ ota_service │  │    ap_api_server    │  │
│  │ (离线缓存)  │  │ (OTA升级)   │  │  (HTTP配网API)      │  │
│  └─────────────┘  └─────────────┘  └─────────────────────┘  │
└─────────────────────────────────────────────────────────────┘
                            │
                            ▼
┌─────────────────────────────────────────────────────────────┐
│                     云端MQTT服务器                           │
└─────────────────────────────────────────────────────────────┘
```

---

## 3. 模块详细说明

### 3.1 核心模块

#### 3.1.1 app_main.c - 主程序入口
**职责**: 系统初始化、主循环调度

**初始化流程**:
1. NVS Flash初始化
2. WiFi事件循环创建
3. WiFi驱动初始化
4. NVS存储初始化
5. UART协议初始化
6. LED状态初始化
7. 离线缓存初始化
8. SN加载（从NVS）
9. MQTT初始化
10. OTA服务初始化
11. WiFi APSTA模式启动
12. BLE广播启动
13. HTTP API服务器启动
14. SNTP时间同步
15. 状态机初始化
16. 主循环任务创建

**主循环任务** (2ms周期):
- UART帧处理
- 按钮检测
- LED状态更新
- 状态机运行
- 离线缓存处理
- OTA看门狗喂狗

---

#### 3.1.2 state_machine.c - 状态机
**职责**: 系统状态管理、命令处理

**状态定义**:
| 状态 | 说明 |
|------|------|
| STATE_INIT | 初始化状态 |
| STATE_AP_PROVISION | AP配网模式 |
| STATE_STA_CONNECTING | STA连接中 |
| STATE_MQTT_CONNECTING | MQTT连接中 |
| STATE_RUNNING | 正常运行 |
| STATE_OTA | OTA升级中 |

**UART命令处理**:
| 命令 | 说明 |
|------|------|
| CMD_SET_BROKER (0x01) | 设置MQTT Broker |
| CMD_PUBLISH (0x02) | 发布MQTT消息 |
| CMD_COMMAND_RECV (0x03) | 接收云端命令 |
| CMD_COMMAND_SEND (0x04) | 发送命令回复 |
| CMD_FACTORY_RESET (0x05) | 恢复出厂设置 |
| CMD_HEARTBEAT (0x06) | 心跳包 |
| CMD_SET_SN (0x08) | 设置设备SN |
| CMD_SET_KEY (0x09) | 设置设备密钥 |
| CMD_SET_AP_SSID (0x0A) | 设置AP名称 |

---

#### 3.1.3 uart_protocol.c - UART帧协议
**职责**: UART数据帧的封装和解析

**帧格式**:
```
┌──────┬──────┬──────────┬──────┐
│ 0xAA │ 长度 │   数据   │ 校验 │
└──────┴──────┴──────────┴──────┘
```

**转义字符**:
- 0xAA → 0x55 0x01
- 0x55 → 0x55 0x00

**配置**:
- UART1 (GPIO0=TX, GPIO1=RX)
- 波特率: 115200
- 最大数据长度: 1024字节

---

### 3.2 通信模块

#### 3.2.1 mqtt_handler.c - MQTT客户端
**职责**: MQTT连接、消息收发

**功能**:
- TLS加密连接
- 自动重连机制
- QoS消息确认
- 离线消息缓存
- LWT遗嘱消息

**Topic结构**:
```
设备Topic前缀: {sn}/
├── /status      - 设备状态
├── /cmd         - 云端命令
├── /cmd/reply   - 命令回复
├── /telemetry   - 遥测数据
└── /ota/cmd     - OTA命令
```

---

#### 3.2.2 ap_api_server.c - HTTP API服务器
**职责**: AP模式下的配网API

**API端点**:
| 端点 | 方法 | 说明 |
|------|------|------|
| /scan | GET | 扫描WiFi列表 |
| /provision | POST | 提交WiFi配置 |
| /info | GET | 获取设备信息 |

---

#### 3.2.3 ble_prov.c (组件) - BLE配网
**职责**: BLE广播和配网

**功能**:
- 持续BLE广播（启动时自动开始）
- WiFi Provisioning协议
- 服务名: CS_INV + SN后6位

**配网流程**:
1. ESP32启动后自动开始BLE广播
2. 手机APP连接BLE
3. APP发送WiFi凭据
4. ESP32保存凭据并重启
5. 重启后使用新WiFi配置连接

---

### 3.3 存储模块

#### 3.3.1 nvs_storage.c - NVS存储
**职责**: 持久化配置存储

**存储键值**:
| 键 | 说明 |
|------|------|
| ws | WiFi SSID |
| wp | WiFi密码 |
| m_host | MQTT主机 |
| m_port | MQTT端口 |
| dev_sn | 设备SN |
| dev_key | 设备密钥 |
| ap_ssid | AP名称 |
| ota_sec_ver | OTA安全版本 |

---

#### 3.3.2 offline_cache.c - 离线缓存
**职责**: 网络断开时缓存数据

**功能**:
- SPIFFS文件系统存储
- 批量文件管理
- TTL过期机制
- 网络恢复后自动上传

**配置**:
- 最大缓存条目: CONFIG_CS_INV_CACHE_MAX_ITEMS
- 缓存天数: CONFIG_CS_INV_CACHE_TTL_DAYS

---

### 3.4 升级模块

#### 3.4.1 ota_service.c - OTA服务
**职责**: ESP32固件OTA升级

**功能**:
- 双分区OTA
- 固件签名验证 (Ed25519)
- 防降级保护
- 回滚机制

---

#### 3.4.2 arm_ota.c - ARM OTA
**职责**: ARM固件透传升级

**功能**:
- UART透传ARM固件数据
- 分块传输
- CRC校验

---

### 3.5 辅助模块

#### 3.5.1 led_status.c - LED状态指示
**职责**: LED状态控制

**LED状态**:
| 状态 | LED1 | LED2 |
|------|------|------|
| AP配网 | 慢闪 | 灭 |
| STA连接 | 快闪 | 灭 |
| MQTT连接 | 快闪 | 慢闪 |
| 正常运行 | 常亮 | 常亮 |
| OTA升级 | 3闪 | 3闪 |

---

#### 3.5.2 ota_signature.c - OTA签名
**职责**: 固件签名验证

**算法**: Ed25519

---

## 4. 启动流程

```
ESP32-C2 上电
    │
    ▼
NVS Flash初始化
    │
    ▼
WiFi驱动初始化
    │
    ▼
外设初始化 (UART, LED, GPIO)
    │
    ▼
加载NVS配置 (SN, WiFi, MQTT)
    │
    ▼
WiFi APSTA模式启动
    │
    ├── 有WiFi配置 → STA连接 → MQTT连接 → 正常运行
    │
    └── 无WiFi配置 → AP配网模式
    │
    ▼
BLE广播启动 (持续)
    │
    ▼
HTTP API服务器启动
    │
    ▼
主循环开始 (2ms周期)
```

---

## 5. 数据流

### 5.1 ARM → ESP32 → MQTT → 云端

```
ARM (UART) 
    │
    ▼
uart_protocol (帧解析)
    │
    ▼
state_machine (命令分发)
    │
    ▼
mqtt_handler (MQTT发布)
    │
    ▼
云端MQTT服务器
```

### 5.2 云端 → MQTT → ESP32 → ARM

```
云端MQTT服务器
    │
    ▼
mqtt_handler (消息接收)
    │
    ▼
state_machine (命令处理)
    │
    ▼
uart_protocol (帧封装)
    │
    ▼
ARM (UART)
```

---

## 6. 配置说明

### 6.1 sdkconfig关键配置

**WiFi配置**:
```
CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=3
CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM=6
CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM=4
CONFIG_ESP_WIFI_RX_BA_WIN=3
```

**BLE配置**:
```
CONFIG_BT_ENABLED=y
CONFIG_BT_NIMBLE_ENABLED=y
CONFIG_BT_NIMBLE_ROLE_PERIPHERAL=y
CONFIG_BT_NIMBLE_ROLE_BROADCASTER=y
CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1
CONFIG_BT_NIMBLE_MSYS_1_BLOCK_COUNT=8
CONFIG_BT_NIMBLE_MSYS_2_BLOCK_COUNT=4
```

**内存优化**:
- 禁用不必要的BLE服务
- 减少WiFi/BLE缓冲区
- 启用LOW_SPEED_MODE

---

## 7. 编译和烧录

### 7.1 编译
```bash
idf.py build
```

### 7.2 烧录
```bash
idf.py -p COMx flash
```

### 7.3 监控
```bash
idf.py -p COMx monitor
```

---

## 8. 日志级别

**默认配置**:
- 启动阶段: ESP_LOG_INFO
- 运行阶段: ESP_LOG_WARN

**查看详细日志**:
修改 app_main.c 中的日志级别:
```c
esp_log_level_set("*", ESP_LOG_INFO);  // 改为 INFO
```

---

## 9. 文件清单

### 9.1 main目录 (30个文件)

| 文件 | 说明 |
|------|------|
| app_main.c/h | 主程序入口 |
| state_machine.c/h | 状态机 |
| uart_protocol.c/h | UART帧协议 |
| mqtt_handler.c/h | MQTT客户端 |
| mqtt_ota_adapter.c | MQTT OTA适配器 |
| mqtt_ota_handler.h | MQTT OTA处理 |
| ota_service.c/h | OTA服务 |
| ota_signature.c/h | OTA签名 |
| arm_ota.c/h | ARM OTA |
| offline_cache.c/h | 离线缓存 |
| nvs_storage.c/h | NVS存储 |
| led_status.c/h | LED状态 |
| ap_api_server.c/h | HTTP API服务器 |
| config.h | 配置定义 |
| cs_protocol_defs.h | 协议定义 |
| mqtt_ca_cert.h | MQTT CA证书 |
| ota_release_pubkey.h | OTA公钥 |

### 9.2 components目录 (3个组件)

| 组件 | 说明 |
|------|------|
| ble_prov/ | BLE配网 |
| monocypher/ | 加密库 (Ed25519) |
| smartconfig/ | SmartConfig (未使用) |

---

## 10. 内存使用

**总RAM**: ~108 KiB

**启动后可用**: ~32 KiB

**内存分配**:
- WiFi驱动: ~5 KiB
- BLE驱动: ~8 KiB
- MQTT客户端: ~4 KiB
- HTTP服务器: ~3 KiB
- 系统任务: ~12 KiB

---

## 11. 注意事项

1. **BLE和WiFi共存**: 已优化内存配置，但高负载时可能有性能影响
2. **SN处理**: ARM发什么SN就存什么，不做校验
3. **日志级别**: 默认WARN级别，需要调试时改为INFO
4. **OTA升级**: 支持双分区，失败可回滚
5. **离线缓存**: SPIFFS空间有限，定期清理过期数据

---

## 12. 版本历史

| 版本 | 日期 | 说明 |
|------|------|------|
| V2.0.0 | 2026-07-21 | BLE持续广播、内存优化 |
| V1.0.0 | 2026-07-20 | 初始版本 |
