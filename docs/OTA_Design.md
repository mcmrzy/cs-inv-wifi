# OTA 升级系统设计文档

> **文档版本**: v1.0
> **适用产品**: CS-I10-6k2 48V 单相离网逆变器
> **适用对象**: ESP32 端开发人员、ARM 端开发人员、测试人员
> **最后更新**: 2026-05-25
> **相关文档**: `ARM_ESP32_UART_Protocol.md`、`MQTT协议文档.md`、`系统参数规范_48V离网逆变器.md`

---

## 目录

1. [概述](#1-概述)
2. [系统架构](#2-系统架构)
3. [ESP 自身 OTA（HTTP 服务器 OTA）](#3-esp-自身-otahttp-服务器-ota)
4. [ARM (GD32F303) 固件串口 OTA](#4-arm-gd32f303-固件串口-ota)
5. [UART 协议扩展（OTA 命令码）](#5-uart-协议扩展ota-命令码)
6. [HTTP API 设计](#6-http-api-设计)
7. [Web 页面设计](#7-web-页面设计)
8. [OTA 状态机设计](#8-ota-状态机设计)
9. [安全考虑](#9-安全考虑)
10. [错误处理与超时机制](#10-错误处理与超时机制)
11. [注意事项](#11-注意事项)

---

## 1. 概述

### 1.1 文档目的

本文档定义逆变器 WiFi 通信模块（ESP32-C3）的 OTA（Over-The-Air）升级系统设计方案，涵盖以下两部分：

| OTA 类型 | 目标芯片 | 升级通道 | 触发方式 |
|----------|----------|----------|----------|
| ESP 自身 OTA | ESP32-C3 / ESP8266 | HTTP 下载 | Web 页面 / 手机 APP |
| ARM 固件 OTA | GD32F303 | ESP32 下载后经 UART 转发 | Web 页面 / 手机 APP |

### 1.2 设计原则

- **可靠性优先**：升级失败必须安全回滚，不影响设备正常运行
- **兼容现有协议**：复用已有 UART 帧格式，通过扩展命令码实现 OTA 功能
- **进度可视**：提供实时进度查询，支持 Web 页面和 APP 调用
- **互斥保护**：OTA 过程中禁止其他操作，防止状态冲突

### 1.3 现有系统基础

| 组件 | 说明 |
|------|------|
| WiFi 模式 | STA + AP 并存 |
| MQTT | 已连接云端 Broker，支持数据透传 |
| UART | 115200 波特率，帧格式：`0xAA + CMD + LEN(2B) + DATA + XOR`，有字节转义 |
| HTTP 服务器 | 端口 8888，提供配网页面 |
| 现有命令码 | 0x01-0x0A, 0xFE(ACK), 0xFF(NACK) |

---

## 2. 系统架构

### 2.1 OTA 系统整体架构

```
┌──────────────────────────────────────────────────────────────────────┐
│                         OTA 触发来源                                  │
│  ┌─────────────┐    ┌─────────────┐    ┌──────────────────────────┐  │
│  │  Web 页面   │    │  手机 APP   │    │  MQTT 云端命令（预留）    │  │
│  │  (端口8888) │    │  (HTTP API) │    │                          │  │
│  └──────┬──────┘    └──────┬──────┘    └──────────┬───────────────┘  │
│         │                  │                      │                  │
│         └──────────┬───────┴──────────────────────┘                  │
│                    ▼                                                  │
│  ┌─────────────────────────────────────────────────────────────┐    │
│  │                    ESP32-C3 OTA Manager                     │    │
│  │  ┌──────────────┐  ┌──────────────┐  ┌──────────────────┐  │    │
│  │  │ HTTP Client  │  │ OTA State    │  │ Progress Tracker │  │    │
│  │  │ (下载固件)   │  │ Machine      │  │ (进度跟踪)       │  │    │
│  │  └──────┬───────┘  └──────┬───────┘  └──────────────────┘  │    │
│  │         │                 │                                  │    │
│  │         ▼                 ▼                                  │    │
│  │  ┌──────────────┐  ┌──────────────┐                        │    │
│  │  │ ESP OTA      │  │ ARM OTA      │                        │    │
│  │  │ (Update库)   │  │ (UART转发)   │                        │    │
│  │  └──────┬───────┘  └──────┬───────┘                        │    │
│  └─────────┼────────────────┼─────────────────────────────────┘    │
│            │                │                                       │
│            ▼                ▼                                       │
│  ┌──────────────┐  ┌──────────────────────────────────────────┐    │
│  │ ESP32 Flash  │  │              UART                        │    │
│  │ (自身固件)   │  │  0xAA + CMD + LEN + DATA + XOR          │    │
│  └──────────────┘  └──────────────────┬───────────────────────┘    │
│                                       │                              │
│                                       ▼                              │
│                              ┌─────────────────┐                    │
│                              │   GD32F303      │                    │
│                              │   (ARM MCU)     │                    │
│                              │   Bootloader    │                    │
│                              │   IAP 模式      │                    │
│                              └─────────────────┘                    │
└──────────────────────────────────────────────────────────────────────┘
```

### 2.2 数据流

#### ESP 自身 OTA 流程

```
用户(Web/APP) ──POST /ota/esp──► ESP32 HTTP Server
                                    │
                                    ▼
                              ESP32 HTTP Client
                                    │
                              ──GET firmware.bin──► 远程固件服务器
                                    │
                              ◄──固件二进制数据────
                                    │
                                    ▼
                              Update.begin(size)
                              Update.write(data)
                              Update.end(true)
                                    │
                                    ▼
                              ESP32 重启
                                    │
                              ┌─────┴─────┐
                              ▼           ▼
                           成功运行    失败回滚
```

#### ARM 固件 OTA 流程

```
用户(Web/APP) ──POST /ota/arm──► ESP32 HTTP Server
                                    │
                                    ▼
                              ESP32 HTTP Client
                                    │
                              ──GET arm_firmware.bin──► 远程固件服务器
                                    │
                              ◄──固件二进制数据────
                                    │
                                    ▼
                              ESP32 缓存固件分包
                                    │
                              ──UART CMD_ARM_OTA_START──► GD32F303
                              ◄──CMD_ARM_OTA_ACK────────
                                    │
                              ──UART CMD_ARM_OTA_DATA──► GD32F303 (循环)
                              ◄──CMD_ARM_OTA_ACK────────
                                    │
                              ──UART CMD_ARM_OTA_END───► GD32F303
                              ◄──CMD_ARM_OTA_ACK────────
                                    │
                                    ▼
                              GD32F303 校验并跳转
```

---

## 3. ESP 自身 OTA（HTTP 服务器 OTA）

### 3.1 功能概述

ESP32-C3（或 ESP8266）通过 HTTP 从远程服务器下载固件 `.bin` 文件，写入 Flash 的备用分区，校验成功后重启切换分区。

### 3.2 支持的芯片与库

| 芯片 | OTA 库 | 说明 |
|------|--------|------|
| ESP32-C3 | `Update.h` (Arduino Core) | 使用双分区（Factory + OTA）方案 |
| ESP8266 | `ESPhttpUpdate.h` | 封装了 HTTP 下载 + Flash 写入 |

### 3.3 触发方式

| 触发源 | 方式 | 说明 |
|--------|------|------|
| Web 页面 | 输入固件 URL 或选择文件上传 | 通过 HTTP API 调用 |
| 手机 APP | HTTP POST 请求 | 调用 `/ota/esp` 接口 |

### 3.4 ESP32-C3 OTA 实现流程

```
状态: IDLE
  │
  ▼
[1] 接收 OTA 请求 (URL / 文件上传)
  │
  ▼
[2] 参数校验
  │   - URL 格式校验（仅限 http:// 或 https://）
  │   - 固件大小检查（<= 1MB）
  │   - 检查是否已有 OTA 进行中
  │
  ├─ 校验失败 → 返回错误，状态回到 IDLE
  │
  ▼
[3] 设置 OTA 锁（禁止其他操作）
  │   - 暂停 MQTT 数据转发
  │   - 暂停 UART 数据透传
  │   - HTTP 服务器仅保留 OTA 相关接口
  │
  ▼
[4] 下载固件
  │   - HTTP GET 请求获取固件
  │   - LED 呼吸灯指示（下载中快闪）
  │   - 超时保护：60s
  │
  ├─ 下载失败 → 释放锁，返回错误，状态回到 IDLE
  │
  ▼
[5] 写入 Flash
  │   - Update.begin(firmwareSize)
  │   - 分块写入 Update.write()
  │   - LED 呼吸灯指示（写入中慢闪）
  │   - 超时保护：120s
  │
  ├─ 写入失败 → Update.abort()，释放锁，返回错误
  │
  ▼
[6] 校验并完成
  │   - Update.end(true)  // true = 自动重启
  │   - ESP32 重启到新固件
  │
  ├─ 校验失败 → 回滚到旧固件
  │
  ▼
[7] 新固件启动
  │   - 标记新分区为有效
  │   - 恢复正常工作
```

### 3.5 ESP8266 OTA 实现流程

ESP8266 使用 `ESPhttpUpdate` 库简化实现：

```cpp
#include <ESP8266httpUpdate.h>

WiFiClient client;
ESPhttpUpdate.update(client, firmwareUrl, firmwareVersion);
// 返回: HTTP_UPDATE_OK / HTTP_UPDATE_FAILED / HTTP_UPDATE_NO_UPDATES
```

### 3.6 LED 指示

| 阶段 | LED 状态 | 说明 |
|------|----------|------|
| 空闲 | 常灭或正常状态 | 无 OTA 操作 |
| 下载中 | 快闪（200ms 周期） | 正在从服务器下载固件 |
| 写入中 | 慢闪（500ms 周期） | 正在写入 Flash |
| 校验中 | 常亮 | 正在校验固件完整性 |
| 成功 | 闪烁 3 次后常灭 | 升级成功，即将重启 |
| 失败 | 快闪 5 次后常灭 | 升级失败，已回滚 |

### 3.7 回滚机制

ESP32 使用双分区方案（Factory + OTA_0），内置回滚支持：

```cpp
// 在 setup() 中检查是否需要回滚
void checkRollback() {
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            // 新固件首次启动，标记为有效
            esp_ota_mark_app_valid_cancel_rollback();
            // 如果 60s 内未调用此函数，ESP-IDF 会自动回滚
        }
    }
}
```

### 3.8 固件版本号校验（可选）

```
请求中携带 version 字段:
POST /ota/esp
{"url": "http://server/firmware.bin", "version": "1.2.0"}

ESP32 比较逻辑:
- 如果 version > 当前版本 → 允许升级
- 如果 version <= 当前版本 → 返回 {"result": "error", "message": "已是最新版本"}
- 如果未提供 version → 跳过版本检查，直接升级
```

---

## 4. ARM (GD32F303) 固件串口 OTA

### 4.1 功能概述

ESP32-C3 从 HTTP 服务器下载 ARM 固件，通过 UART 将固件分包发送给 GD32F303。GD32F303 的 Bootloader 支持 IAP（在应用编程），负责将固件写入 Flash。

> **注意**：GD32F303 的 Bootloader 实现由 ARM 端开发人员负责，本文档仅定义 ESP 端的协议和流程。

### 4.2 整体流程

```
ESP32                                GD32F303
  │                                     │
  │  [1] 从 HTTP 服务器下载 ARM 固件      │
  │      下载到 ESP32 RAM 缓冲区          │
  │                                     │
  │  [2] CMD_ARM_OTA_START (0x10)       │
  │───────►                             │
  │      DATA: {firmware_size, version,  │
  │              total_packets, md5}     │
  │                                     │
  │  [3] CMD_ARM_OTA_ACK (0x13)         │
  │◄──────                              │
  │      DATA: {ready, max_packet_size} │
  │                                     │
  │  [4] 循环发送数据包                   │
  │                                     │
  │  CMD_ARM_OTA_DATA (0x11)            │
  │───────►                             │
  │      DATA: {packet_index, data}     │
  │                                     │
  │  CMD_ARM_OTA_ACK (0x13)             │
  │◄──────                              │
  │      DATA: {packet_index}            │
  │                                     │
  │  ... (重复直到所有包发送完毕)          │
  │                                     │
  │  [5] CMD_ARM_OTA_END (0x12)         │
  │───────►                             │
  │      DATA: {total_size, md5}         │
  │                                     │
  │  [6] CMD_ARM_OTA_ACK (0x13)         │
  │◄──────                              │
  │      DATA: {verify_ok, new_version} │
  │                                     │
  │  [7] GD32F303 校验并跳转到新固件      │
  │                                     │
```

### 4.3 ESP32 端实现要点

#### 4.3.1 固件下载

- ESP32 通过 HTTP GET 从远程服务器下载 ARM 固件 `.bin` 文件
- 固件大小限制：256KB
- 下载过程中 ESP32 将固件缓存到内部缓冲区（分块下载，逐块转发）
- 不需要一次性将全部固件加载到内存，采用流式处理

#### 4.3.2 分包策略

| 参数 | 值 | 说明 |
|------|-----|------|
| 默认包大小 | 256 字节 | 每包 DATA 中的固件数据长度 |
| 最大包大小 | 512 字节 | 受 UART 帧最大 DATA 长度限制 |
| 包序号 | 0-based | 从 0 开始递增 |
| 重传次数上限 | 3 次 | 超过则 OTA 失败 |
| 包间延时 | 10ms | 等待 ARM 写入 Flash |

#### 4.3.3 流式转发方案

为节省 ESP32 内存，采用流式转发：

```
1. ESP32 开始 HTTP 下载
2. 每下载一个分包大小（256B）的数据
3. 立即通过 UART 发送给 GD32F303
4. 等待 ACK 后继续下载下一个分包
5. 循环直到全部数据发送完毕
```

> **优势**：ESP32 仅需 256B 缓冲区，无需一次性缓存 256KB 固件。

#### 4.3.4 MD5 校验

- ESP32 在下载前获取固件的 MD5 值（可通过 HTTP Header 或额外 API 获取）
- 传输结束时将 MD5 发送给 ARM，ARM 端进行完整性校验
- 如果无法获取 MD5，可使用 CRC32 替代

---

## 5. UART 协议扩展（OTA 命令码）

### 5.1 新增命令码总览

在现有命令码（0x01-0x0A, 0xFE, 0xFF）基础上，新增 OTA 相关命令码 0x10-0x15：

| 命令码 | 名称 | 方向 | 说明 |
|--------|------|------|------|
| 0x10 | CMD_ARM_OTA_START | ESP32 → ARM | 通知 ARM 进入 Bootloader/IAP 模式 |
| 0x11 | CMD_ARM_OTA_DATA | ESP32 → ARM | 发送固件数据包 |
| 0x12 | CMD_ARM_OTA_END | ESP32 → ARM | 固件传输结束，校验并跳转 |
| 0x13 | CMD_ARM_OTA_ACK | ARM → ESP32 | 确认收到数据包 |
| 0x14 | CMD_ARM_OTA_NACK | ARM → ESP32 | 请求重传 |
| 0x15 | CMD_ARM_OTA_INFO | ARM → ESP32 | 上报 ARM 固件信息（版本、大小等） |

### 5.2 CMD_ARM_OTA_START (0x10) — 通知 ARM 进入 IAP 模式

**方向**: ESP32 → ARM

**DATA 格式**（JSON 字符串）：

```json
{
    "firmware_size": 131072,
    "version": "1.0.0",
    "total_packets": 512,
    "packet_size": 256,
    "md5": "a1b2c3d4e5f6..."
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| firmware_size | uint32 | 固件总大小（字节） |
| version | string | 目标固件版本号 |
| total_packets | uint16 | 总分包数 |
| packet_size | uint16 | 每包数据大小（字节） |
| md5 | string | 固件 MD5 校验值（32字符十六进制） |

**ARM 响应**：CMD_ARM_OTA_ACK (0x13)

```json
{
    "packet_index": 0,
    "ready": true,
    "max_packet_size": 256
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| packet_index | uint16 | 0（START 响应固定为 0） |
| ready | bool | 是否准备好接收数据 |
| max_packet_size | uint16 | ARM 支持的最大包大小 |

**帧示例**：

```
发送（ESP32 → ARM）:
AA 10 00 XX {"firmware_size":131072,"version":"1.0.0","total_packets":512,"packet_size":256,"md5":"a1b2c3..."} [XOR]

响应（ARM → ESP32）:
AA 13 00 XX {"packet_index":0,"ready":true,"max_packet_size":256} [XOR]
```

---

### 5.3 CMD_ARM_OTA_DATA (0x11) — 发送固件数据包

**方向**: ESP32 → ARM

**DATA 格式**（二进制结构）：

```
+------------------+------------------+------------------+
| packet_index (2B)| data_len    (1B) | data        (nB) |
| 高字节  | 低字节  |                  |                  |
+------------------+------------------+------------------+
```

| 字段 | 长度 | 说明 |
|------|------|------|
| packet_index | 2B | 包序号（0-based，big-endian） |
| data_len | 1B | 本包数据长度（最后一包可能小于 packet_size） |
| data | nB | 固件二进制数据 |

> **注意**：此命令的 DATA 域使用二进制格式而非 JSON，以提高传输效率。

**ARM 响应**：CMD_ARM_OTA_ACK (0x13) 或 CMD_ARM_OTA_NACK (0x14)

**帧示例**：

```
发送（ESP32 → ARM）:
AA 11 01 01 [packet_index_H] [packet_index_L] [data_len] [data_0] [data_1] ... [data_n] [XOR]

响应成功（ARM → ESP32）:
AA 13 00 02 [packet_index_H] [packet_index_L] [XOR]

响应失败（ARM → ESP32）:
AA 14 00 02 [packet_index_H] [packet_index_L] [XOR]
```

---

### 5.4 CMD_ARM_OTA_END (0x12) — 传输结束

**方向**: ESP32 → ARM

**DATA 格式**（JSON 字符串）：

```json
{
    "total_size": 131072,
    "total_packets": 512,
    "md5": "a1b2c3d4e5f6..."
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| total_size | uint32 | 固件总大小（字节） |
| total_packets | uint16 | 实际发送的总包数 |
| md5 | string | 固件 MD5 校验值 |

**ARM 响应**：CMD_ARM_OTA_ACK (0x13)

```json
{
    "packet_index": 65535,
    "verify_ok": true,
    "new_version": "1.0.0"
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| packet_index | uint16 | 0xFFFF（END 响应固定值） |
| verify_ok | bool | 校验是否通过 |
| new_version | string | ARM 端读取的新固件版本号 |

**帧示例**：

```
发送（ESP32 → ARM）:
AA 12 00 XX {"total_size":131072,"total_packets":512,"md5":"a1b2c3..."} [XOR]

响应（ARM → ESP32）:
AA 13 00 XX {"packet_index":65535,"verify_ok":true,"new_version":"1.0.0"} [XOR]
```

---

### 5.5 CMD_ARM_OTA_ACK (0x13) — 确认响应

**方向**: ARM → ESP32

**DATA 格式**：

根据上下文不同，DATA 内容有三种形式：

| 场景 | DATA 格式 | 说明 |
|------|-----------|------|
| 响应 START | JSON: `{"packet_index":0,"ready":true,"max_packet_size":256}` | 确认进入 IAP 模式 |
| 响应 DATA | 二进制: `packet_index(2B)` | 确认收到数据包 |
| 响应 END | JSON: `{"packet_index":65535,"verify_ok":true,"new_version":"1.0.0"}` | 校验结果 |

---

### 5.6 CMD_ARM_OTA_NACK (0x14) — 请求重传

**方向**: ARM → ESP32

**DATA 格式**（二进制结构）：

```
+------------------+------------------+
| packet_index (2B)| reason      (1B) |
+------------------+------------------+
```

| 字段 | 长度 | 说明 |
|------|------|------|
| packet_index | 2B | 需要重传的包序号（big-endian） |
| reason | 1B | 错误原因码 |

**错误原因码**：

| 值 | 说明 |
|-----|------|
| 0x01 | XOR 校验错误 |
| 0x02 | Flash 写入失败 |
| 0x03 | 包序号不连续 |
| 0x04 | 数据长度异常 |
| 0x05 | 缓冲区溢出 |

**帧示例**：

```
ARM → ESP32:
AA 14 00 03 [packet_index_H] [packet_index_L] [reason] [XOR]
```

---

### 5.7 CMD_ARM_OTA_INFO (0x15) — ARM 固件信息上报

**方向**: ARM → ESP32

**触发时机**：
- ESP32 上电后主动查询（发送 CMD_ARM_OTA_INFO 请求）
- ARM 在 OTA 完成后主动上报

**DATA 格式**（JSON 字符串）：

```json
{
    "version": "1.0.0",
    "firmware_size": 131072,
    "bootloader_version": "1.0.0",
    "flash_used": 98304,
    "flash_total": 262144,
    "chip_id": "GD32F303xx"
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| version | string | 当前 ARM 固件版本号 |
| firmware_size | uint32 | 固件大小（字节） |
| bootloader_version | string | Bootloader 版本号 |
| flash_used | uint32 | 已使用 Flash 空间 |
| flash_total | uint32 | 总 Flash 空间 |
| chip_id | string | 芯片型号 |

**帧示例**：

```
ARM → ESP32:
AA 15 00 XX {"version":"1.0.0","firmware_size":131072,"bootloader_version":"1.0.0","flash_used":98304,"flash_total":262144,"chip_id":"GD32F303xx"} [XOR]
```

---

### 5.8 命令码汇总表（含已有命令码）

| 命令码 | 名称 | 方向 | 类型 | 说明 |
|--------|------|------|------|------|
| 0x01 | CMD_SET_BROKER | ARM → ESP32 | 配置 | 设置 MQTT Broker 地址 |
| 0x02 | CMD_PUBLISH | ARM → ESP32 | 数据 | 发布 MQTT 消息 |
| 0x03 | CMD_COMMAND_RECV | ESP32 → ARM | 控制 | 转发云端命令 |
| 0x04 | CMD_COMMAND_SEND | ARM → ESP32 | 控制 | 命令执行结果 |
| 0x05 | CMD_FACTORY_RESET | ARM → ESP32 | 配置 | 恢复出厂设置 |
| 0x06 | CMD_HEARTBEAT | ESP32 → ARM | 心跳 | 心跳信号 |
| 0x08 | CMD_SET_SN | ARM → ESP32 | 配置 | 设置序列号 |
| 0x09 | CMD_SET_KEY | ARM → ESP32 | 配置 | 设置设备密钥 |
| 0x0A | CMD_SET_AP_SSID | ARM → ESP32 | 配置 | 设置热点名称 |
| **0x10** | **CMD_ARM_OTA_START** | **ESP32 → ARM** | **OTA** | **通知进入 IAP 模式** |
| **0x11** | **CMD_ARM_OTA_DATA** | **ESP32 → ARM** | **OTA** | **发送固件数据包** |
| **0x12** | **CMD_ARM_OTA_END** | **ESP32 → ARM** | **OTA** | **传输结束，校验跳转** |
| **0x13** | **CMD_ARM_OTA_ACK** | **ARM → ESP32** | **OTA** | **确认收到数据包** |
| **0x14** | **CMD_ARM_OTA_NACK** | **ARM → ESP32** | **OTA** | **请求重传** |
| **0x15** | **CMD_ARM_OTA_INFO** | **ARM → ESP32** | **OTA** | **上报固件信息** |
| 0xFE | CMD_ACK | ESP32 → ARM | 通用 | 确认响应 |
| 0xFF | CMD_NACK | ESP32 → ARM | 通用 | 否定响应 |

---

## 6. HTTP API 设计

### 6.1 概述

OTA HTTP API 运行在 ESP32 内置 HTTP 服务器上（端口 8888），供 Web 页面和手机 APP 调用。所有接口使用 JSON 格式。

**Base URL**: `http://<ESP32_IP>:8888`

### 6.2 POST /ota/esp — 触发 ESP 自身 OTA

**请求**：

```http
POST /ota/esp HTTP/1.1
Content-Type: application/json

{
    "url": "http://server/firmware.bin",
    "version": "1.2.0"
}
```

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| url | string | 是 | 固件下载 URL，仅限 http:// 或 https:// |
| version | string | 否 | 目标固件版本号，用于版本比较 |

**响应（成功）**：

```json
{
    "result": "ok",
    "message": "ESP OTA started"
}
```

**响应（失败）**：

```json
{
    "result": "error",
    "message": "URL format invalid"
}
```

**错误码说明**：

| message | 说明 |
|---------|------|
| URL format invalid | URL 格式错误，仅支持 HTTP/HTTPS |
| OTA already in progress | 已有 OTA 操作进行中 |
| Version not newer | 目标版本不高于当前版本 |
| Download failed | 固件下载失败 |
| Firmware too large | 固件超过 1MB 限制 |
| Write failed | Flash 写入失败 |
| Verify failed | 固件校验失败 |

---

### 6.3 POST /ota/arm — 触发 ARM 固件 OTA

**请求**：

```http
POST /ota/arm HTTP/1.1
Content-Type: application/json

{
    "url": "http://server/arm_firmware.bin",
    "version": "1.0.0"
}
```

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| url | string | 是 | ARM 固件下载 URL |
| version | string | 否 | 目标固件版本号 |

**响应（成功）**：

```json
{
    "result": "ok",
    "message": "ARM OTA started"
}
```

**响应（失败）**：

```json
{
    "result": "error",
    "message": "ARM not ready"
}
```

**错误码说明**：

| message | 说明 |
|---------|------|
| URL format invalid | URL 格式错误 |
| OTA already in progress | 已有 OTA 操作进行中 |
| ARM not ready | ARM 未响应或未准备好 |
| Download failed | 固件下载失败 |
| Firmware too large | 固件超过 256KB 限制 |
| Transfer failed | UART 传输失败 |
| Verify failed | ARM 端校验失败 |
| Timeout | 操作超时 |

---

### 6.4 GET /ota/progress — 查询 OTA 进度

**请求**：

```http
GET /ota/progress HTTP/1.1
```

**响应（进行中）**：

```json
{
    "target": "arm",
    "progress": 45,
    "status": "writing",
    "detail": "正在写入第 231/512 包",
    "elapsed": 12,
    "remaining_estimate": 15
}
```

**响应（完成）**：

```json
{
    "target": "esp",
    "progress": 100,
    "status": "done",
    "detail": "升级成功，设备即将重启",
    "elapsed": 8,
    "remaining_estimate": 0
}
```

**响应（空闲）**：

```json
{
    "target": null,
    "progress": 0,
    "status": "idle",
    "detail": null,
    "elapsed": 0,
    "remaining_estimate": 0
}
```

**字段说明**：

| 字段 | 类型 | 说明 |
|------|------|------|
| target | string / null | OTA 目标：`"esp"` / `"arm"` / `null`（空闲） |
| progress | int | 进度百分比 0-100 |
| status | string | 状态：见下表 |
| detail | string / null | 详细描述 |
| elapsed | int | 已耗时（秒） |
| remaining_estimate | int | 预计剩余时间（秒），0 表示无法估算 |

**status 状态值**：

| 值 | 说明 |
|-----|------|
| idle | 空闲，无 OTA 操作 |
| downloading | 正在从服务器下载固件 |
| writing | 正在写入 Flash（ESP）或通过 UART 传输（ARM） |
| verifying | 正在校验固件 |
| done | 升级完成 |
| error | 升级失败 |

---

### 6.5 GET /ota/info — 查询设备固件信息

**请求**：

```http
GET /ota/info HTTP/1.1
```

**响应**：

```json
{
    "esp_version": "1.0.0",
    "esp_sdk_version": "v5.1.2",
    "arm_version": "1.0.0",
    "arm_bootloader_version": "1.0.0",
    "free_heap": 123456,
    "chip_model": "ESP32-C3",
    "chip_cores": 1,
    "flash_size": 4194304,
    "ota_partition_size": 1048576,
    "ota_remaining": 3
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| esp_version | string | ESP32 当前固件版本号 |
| esp_sdk_version | string | ESP-IDF / Arduino Core 版本 |
| arm_version | string | ARM 当前固件版本号（从 ARM 查询） |
| arm_bootloader_version | string | ARM Bootloader 版本号 |
| free_heap | uint32 | ESP32 可用堆内存（字节） |
| chip_model | string | 芯片型号 |
| chip_cores | uint8 | CPU 核心数 |
| flash_size | uint32 | Flash 总大小（字节） |
| ota_partition_size | uint32 | OTA 分区大小（字节） |
| ota_remaining | uint8 | OTA 剩余可用次数（ESP32 分区磨损估算） |

---

### 6.6 API 调用时序

#### ESP OTA 时序

```
Web/APP          ESP32 HTTP Server       ESP32 OTA Manager      远程服务器
   │                     │                       │                    │
   │  POST /ota/esp      │                       │                    │
   │────────────────────►│                       │                    │
   │                     │  startEspOTA(url)     │                    │
   │                     │──────────────────────►│                    │
   │                     │                       │  GET firmware.bin  │
   │                     │                       │───────────────────►│
   │                     │                       │◄───────────────────│
   │                     │                       │  firmware data     │
   │  {"result":"ok"}    │                       │                    │
   │◄────────────────────│                       │                    │
   │                     │                       │                    │
   │  GET /ota/progress  │                       │                    │
   │────────────────────►│                       │                    │
   │  {"progress":45,    │                       │                    │
   │   "status":"writing"}│                      │                    │
   │◄────────────────────│                       │                    │
   │                     │                       │                    │
   │  GET /ota/progress  │                       │                    │
   │────────────────────►│                       │                    │
   │  {"progress":100,   │                       │                    │
   │   "status":"done"}  │                       │                    │
   │◄────────────────────│                       │                    │
   │                     │                       │                    │
   │                     │              [ESP32 重启]                   │
```

#### ARM OTA 时序

```
Web/APP          ESP32 HTTP Server       ESP32 OTA Manager      远程服务器      GD32F303
   │                     │                       │                    │              │
   │  POST /ota/arm      │                       │                    │              │
   │────────────────────►│                       │                    │              │
   │                     │  startArmOTA(url)     │                    │              │
   │                     │──────────────────────►│                    │              │
   │                     │                       │  GET arm_fw.bin    │              │
   │                     │                       │───────────────────►│              │
   │                     │                       │◄───────────────────│              │
   │                     │                       │  firmware data     │              │
   │  {"result":"ok"}    │                       │                    │              │
   │◄────────────────────│                       │                    │              │
   │                     │                       │                    │              │
   │                     │                       │  OTA_START (0x10)  │              │
   │                     │                       │──────────────────────────────────►│
   │                     │                       │◄──────────────────────────────────│
   │                     │                       │  OTA_ACK (0x13)    │              │
   │                     │                       │                    │              │
   │                     │                       │  OTA_DATA (0x11)   │              │
   │                     │                       │──────────────────────────────────►│
   │                     │                       │◄──────────────────────────────────│
   │                     │                       │  OTA_ACK (0x13)    │              │
   │                     │                       │  ... (循环)        │              │
   │                     │                       │                    │              │
   │                     │                       │  OTA_END (0x12)    │              │
   │                     │                       │──────────────────────────────────►│
   │                     │                       │◄──────────────────────────────────│
   │                     │                       │  OTA_ACK (0x13)    │              │
   │                     │                       │  verify_ok=true    │              │
   │                     │                       │                    │              │
   │  GET /ota/progress  │                       │                    │              │
   │────────────────────►│                       │                    │              │
   │  {"progress":100,   │                       │                    │              │
   │   "status":"done"}  │                       │                    │              │
   │◄────────────────────│                       │                    │              │
```

---

## 7. Web 页面设计

### 7.1 页面结构

在现有配网 Web 服务器（端口 8888）上新增 OTA 页面。

#### 页面路由

| 路径 | 说明 |
|------|------|
| `/` | 现有配网页面（状态页），添加 OTA 入口链接 |
| `/ota` | OTA 升级页面（新增） |

#### 状态页修改

在现有状态页的导航栏或页面底部添加 OTA 入口：

```html
<a href="/ota" class="ota-link">固件升级 (OTA)</a>
```

### 7.2 OTA 页面布局

```
┌─────────────────────────────────────────────────────┐
│  CS 逆变器 - 固件升级 (OTA)                          │
├─────────────────────────────────────────────────────┤
│                                                     │
│  ┌─── 设备信息 ───────────────────────────────────┐ │
│  │ ESP 固件版本: V1.0.0    ARM 固件版本: V1.0.0  │ │
│  │ 芯片型号: ESP32-C3      可用内存: 123KB       │ │
│  │ Flash 大小: 4MB          OTA 分区: 1MB        │ │
│  └───────────────────────────────────────────────┘ │
│                                                     │
│  ┌─── ESP 固件升级 ──────────────────────────────┐ │
│  │                                               │ │
│  │  固件 URL: [http://server/firmware.bin    ]   │ │
│  │  版本号:   [1.2.0                        ]   │ │
│  │                                               │ │
│  │  或选择本地文件: [选择文件...]                   │ │
│  │                                               │ │
│  │  [开始升级 ESP]                                │ │
│  │                                               │ │
│  │  进度: ████████░░░░░░░░░░░░ 45%               │ │
│  │  状态: 正在写入 Flash...                       │ │
│  └───────────────────────────────────────────────┘ │
│                                                     │
│  ┌─── ARM 固件升级 ──────────────────────────────┐ │
│  │                                               │ │
│  │  固件 URL: [http://server/arm_firmware.bin ]   │ │
│  │  版本号:   [1.0.0                        ]   │ │
│  │                                               │ │
│  │  或选择本地文件: [选择文件...]                   │ │
│  │                                               │ │
│  │  [开始升级 ARM]                                │ │
│  │                                               │ │
│  │  进度: ████████████████████░░ 85%              │ │
│  │  状态: 正在传输第 436/512 包...                │ │
│  └───────────────────────────────────────────────┘ │
│                                                     │
│  ┌─── 升级日志 ──────────────────────────────────┐ │
│  │ [12:00:01] ESP OTA 开始                       │ │
│  │ [12:00:03] 正在下载固件... (128KB/256KB)       │ │
│  │ [12:00:05] 下载完成，开始写入 Flash             │ │
│  │ [12:00:08] 写入完成，校验中...                 │ │
│  │ [12:00:09] 校验通过，设备即将重启              │ │
│  └───────────────────────────────────────────────┘ │
│                                                     │
│  [返回状态页]                                       │
└─────────────────────────────────────────────────────┘
```

### 7.3 页面功能说明

#### 7.3.1 设备信息区域

- 页面加载时自动调用 `GET /ota/info` 获取设备信息
- 显示 ESP 和 ARM 的固件版本、芯片型号、内存信息

#### 7.3.2 ESP 固件升级区域

- **URL 输入**：手动输入固件下载地址
- **文件上传**：点击"选择文件"选择本地 `.bin` 文件（上传到 ESP32 后触发 OTA）
- **开始升级按钮**：调用 `POST /ota/esp`
- **进度条**：轮询 `GET /ota/progress` 更新进度（每 1 秒）

#### 7.3.3 ARM 固件升级区域

- 功能与 ESP 升级区域类似
- 调用 `POST /ota/arm` 触发 ARM OTA

#### 7.3.4 升级日志区域

- 实时显示 OTA 过程中的关键事件
- 使用 JavaScript 定时轮询进度接口，解析 `detail` 字段追加日志

### 7.4 文件上传方案

当用户选择本地文件上传时，流程如下：

```
1. 用户选择 .bin 文件
2. 浏览器通过 HTTP POST multipart/form-data 上传文件到 ESP32
3. ESP32 接收文件并保存到临时缓冲区
4. 自动触发 OTA 流程（等同于 URL 下载完成后的步骤）
```

**上传接口**：

```http
POST /ota/upload HTTP/1.1
Content-Type: multipart/form-data; boundary=----WebKitFormBoundary

------WebKitFormBoundary
Content-Disposition: form-data; name="file"; filename="firmware.bin"
Content-Type: application/octet-stream

[二进制固件数据]
------WebKitFormBoundary
Content-Disposition: form-data; name="target"

esp
------WebKitFormBoundary--
```

| 字段 | 类型 | 说明 |
|------|------|------|
| file | file | 固件文件（.bin） |
| target | string | 目标：`esp` 或 `arm` |

**响应**：

```json
{
    "result": "ok",
    "message": "File uploaded, OTA started",
    "size": 262144
}
```

---

## 8. OTA 状态机设计

### 8.1 ESP32 OTA Manager 状态机

```
                         ┌──────────┐
                         │   IDLE   │◄──────────────────────────────┐
                         │  (空闲)  │                               │
                         └────┬─────┘                               │
                              │                                     │
                    收到 OTA 请求                                    │
                    (ESP 或 ARM)                                    │
                              │                                     │
                              ▼                                     │
                    ┌─────────────────┐                             │
                    │   VALIDATING    │                             │
                    │  (参数校验)      │                             │
                    └────────┬────────┘                             │
                             │                                      │
                  ┌──────────┴──────────┐                           │
                  │                     │                           │
             校验通过               校验失败                        │
                  │                     │                           │
                  ▼                     └───────────────────────────┘
          ┌──────────────┐
          │    LOCKED    │
          │  (OTA 锁定)  │
          └──────┬───────┘
                 │
        ┌────────┴────────┐
        │                 │
   ESP OTA           ARM OTA
        │                 │
        ▼                 ▼
 ┌──────────────┐ ┌──────────────┐
 │  ESP_DOWNLOAD│ │ ARM_DOWNLOAD│
 │ (ESP下载固件)│ │ (ARM下载固件)│
 └──────┬───────┘ └──────┬───────┘
        │                │
        ▼                ▼
 ┌──────────────┐ ┌──────────────┐
 │  ESP_WRITE   │ │ARM_TRANSFER  │
 │ (写入Flash)  │ │(UART传输固件)│
 └──────┬───────┘ └──────┬───────┘
        │                │
        ▼                ▼
 ┌──────────────┐ ┌──────────────┐
 │ ESP_VERIFY   │ │ARM_VERIFY    │
 │ (校验固件)   │ │(等待ARM校验) │
 └──────┬───────┘ └──────┬───────┘
        │                │
        ▼                ▼
 ┌──────────────┐ ┌──────────────┐
 │   ESP_DONE   │ │  ARM_DONE    │
 │ (完成/重启)  │ │ (完成)       │
 └──────────────┘ └──────────────┘

        任何阶段出错 ──► ┌──────────────┐
                        │    ERROR     │
                        │  (错误状态)   │
                        └──────┬───────┘
                               │
                    释放锁，回到 IDLE
```

### 8.2 状态定义

| 状态 | 值 | 说明 |
|------|-----|------|
| OTA_STATE_IDLE | 0 | 空闲，可接受新 OTA 请求 |
| OTA_STATE_VALIDATING | 1 | 正在校验请求参数 |
| OTA_STATE_LOCKED | 2 | 已获取 OTA 锁，暂停其他功能 |
| OTA_STATE_ESP_DOWNLOAD | 3 | 正在下载 ESP 固件 |
| OTA_STATE_ESP_WRITE | 4 | 正在写入 ESP Flash |
| OTA_STATE_ESP_VERIFY | 5 | 正在校验 ESP 固件 |
| OTA_STATE_ESP_DONE | 6 | ESP OTA 完成 |
| OTA_STATE_ARM_DOWNLOAD | 7 | 正在下载 ARM 固件 |
| OTA_STATE_ARM_TRANSFER | 8 | 正在通过 UART 传输 ARM 固件 |
| OTA_STATE_ARM_VERIFY | 9 | 等待 ARM 校验完成 |
| OTA_STATE_ARM_DONE | 10 | ARM OTA 完成 |
| OTA_STATE_ERROR | 11 | OTA 出错 |

### 8.3 状态转换规则

| 当前状态 | 事件 | 下一状态 | 动作 |
|----------|------|----------|------|
| IDLE | 收到有效 OTA 请求 | VALIDATING | 解析参数 |
| VALIDATING | 校验通过 | LOCKED | 获取 OTA 锁 |
| VALIDATING | 校验失败 | IDLE | 返回错误信息 |
| LOCKED | ESP OTA | ESP_DOWNLOAD | 开始下载 |
| LOCKED | ARM OTA | ARM_DOWNLOAD | 开始下载 |
| ESP_DOWNLOAD | 下载完成 | ESP_WRITE | 开始写入 Flash |
| ESP_DOWNLOAD | 下载失败 | ERROR | 释放锁 |
| ESP_WRITE | 写入完成 | ESP_VERIFY | 开始校验 |
| ESP_WRITE | 写入失败 | ERROR | 释放锁 |
| ESP_VERIFY | 校验通过 | ESP_DONE | 标记成功，准备重启 |
| ESP_VERIFY | 校验失败 | ERROR | 回滚 |
| ESP_DONE | 重启 | IDLE | (重启后自动) |
| ARM_DOWNLOAD | 下载完成 | ARM_TRANSFER | 开始 UART 传输 |
| ARM_DOWNLOAD | 下载失败 | ERROR | 释放锁 |
| ARM_TRANSFER | 传输完成 | ARM_VERIFY | 等待 ARM 校验 |
| ARM_TRANSFER | 传输失败 | ERROR | 释放锁 |
| ARM_VERIFY | 校验通过 | ARM_DONE | 标记成功 |
| ARM_VERIFY | 校验失败 | ERROR | 释放锁 |
| ARM_DONE | 完成 | IDLE | 释放锁 |
| ERROR | 清理完成 | IDLE | 释放锁 |

### 8.4 OTA 锁机制

OTA 过程中需要锁定以下资源，防止状态冲突：

| 资源 | 锁定操作 | 说明 |
|------|----------|------|
| MQTT 数据透传 | 暂停转发 | 避免 OTA 数据与正常数据冲突 |
| UART 数据透传 | 暂停转发 | ARM OTA 需要独占 UART |
| HTTP 配网接口 | 禁用 | 防止 OTA 过程中修改配置 |
| OTA 接口 | 仅保留进度查询 | 允许查询进度，禁止新 OTA 请求 |

```cpp
class OTALock {
public:
    bool acquire() {
        if (_locked) return false;
        _locked = true;
        // 暂停 MQTT 透传
        // 暂停 UART 透传
        // 禁用配网接口
        return true;
    }

    void release() {
        _locked = false;
        // 恢复 MQTT 透传
        // 恢复 UART 透传
        // 恢复配网接口
    }

    bool isLocked() { return _locked; }

private:
    bool _locked = false;
};
```

---

## 9. 安全考虑

### 9.1 固件 URL 限制

- 仅允许 `http://` 和 `https://` 协议
- 禁止 `file://`、`ftp://` 等其他协议
- URL 长度限制：256 字符
- 域名白名单（可选）：限制只能从指定服务器下载

```cpp
bool isValidOTAUrl(const String& url) {
    if (url.startsWith("http://") || url.startsWith("https://")) {
        return url.length() <= 256;
    }
    return false;
}
```

### 9.2 固件大小限制

| 目标 | 最大大小 | 说明 |
|------|----------|------|
| ESP32-C3 | 1MB (1048576 字节) | 受 OTA 分区大小限制 |
| ESP8266 | 512KB | 受可用 Flash 空间限制 |
| GD32F303 | 256KB (262144 字节) | 受 ARM Flash 布局限制 |

### 9.3 超时保护

| 阶段 | 超时时间 | 说明 |
|------|----------|------|
| HTTP 下载 | 60s | 连接 + 下载总超时 |
| Flash 写入（ESP） | 120s | 写入 + 校验总超时 |
| UART 单包传输 | 5s | 单包发送 + 等待 ACK |
| UART 总传输 | 300s | ARM OTA 全程超时 |
| ARM 校验 | 30s | 等待 ARM 端校验完成 |

### 9.4 OTA 过程中的操作限制

- **禁止并发 OTA**：同一时间只允许一个 OTA 操作（ESP 或 ARM）
- **禁止配置修改**：OTA 过程中禁止修改 WiFi、MQTT、SN 等配置
- **禁止重启命令**：OTA 过程中忽略重启命令
- **心跳保持**：OTA 过程中 ESP32 心跳继续发送，维持 MQTT 在线状态

### 9.5 固件完整性校验

| 校验方式 | ESP OTA | ARM OTA | 说明 |
|----------|---------|---------|------|
| 大小校验 | 是 | 是 | 下载前检查 Content-Length |
| MD5 校验 | 可选 | 推荐 | 传输完成后比对 MD5 |
| CRC32 校验 | 可选 | 可选 | 备选方案 |
| 签名校验 | 预留 | 预留 | 未来可增加 RSA 签名验证 |

### 9.6 回滚保护

- ESP32：利用 ESP-IDF 内置的 anti-rollback 功能，新固件校验失败自动回滚
- ARM：由 ARM Bootloader 负责，OTA_END 校验失败时不跳转，保持旧固件运行

---

## 10. 错误处理与超时机制

### 10.1 ESP OTA 错误处理

| 错误场景 | 处理方式 | 用户提示 |
|----------|----------|----------|
| URL 无法访问 | 释放锁，回到 IDLE | "下载失败：无法连接服务器" |
| 下载超时 | 释放锁，回到 IDLE | "下载失败：超时（60s）" |
| 固件过大 | 释放锁，回到 IDLE | "固件超过大小限制（1MB）" |
| Flash 写入失败 | Update.abort()，释放锁 | "写入失败：Flash 错误" |
| 校验失败 | 回滚到旧固件 | "校验失败，已回滚" |
| 新固件启动失败 | 自动回滚（ESP-IDF） | "新固件启动失败，已回滚" |

### 10.2 ARM OTA 错误处理

| 错误场景 | 处理方式 | 用户提示 |
|----------|----------|----------|
| ARM 未响应 START | 重试 3 次，失败则中止 | "ARM 未响应，请检查连接" |
| ARM 返回 NACK | 重传当前包，最多 3 次 | "重传中..." |
| 单包超时 | 重传当前包，最多 3 次 | "传输超时，正在重试" |
| 重传次数耗尽 | 中止 OTA，释放锁 | "传输失败：重传次数超限" |
| ARM 校验失败 | 通知 ARM 保持旧固件 | "ARM 校验失败，请重试" |
| UART 通信中断 | 中止 OTA，释放锁 | "UART 通信中断" |

### 10.3 重传机制

ARM OTA 数据传输采用 ACK/NACK 确认机制：

```
ESP32 发送 DATA 包
    │
    ├── 收到 ACK → 发送下一包
    │
    ├── 收到 NACK → 重传当前包（重试计数 +1）
    │                  │
    │                  ├── 重试 <= 3 → 重传
    │                  └── 重试 > 3 → OTA 失败
    │
    └── 超时（5s 无响应）→ 重传当前包（重试计数 +1）
                            │
                            ├── 重试 <= 3 → 重传
                            └── 重试 > 3 → OTA 失败
```

---

## 11. 注意事项

### 11.1 开发注意事项

1. **内存管理**：ARM OTA 采用流式转发，ESP32 仅需 256-512 字节缓冲区，避免内存不足
2. **UART 速率**：OTA 数据传输使用现有 115200 波特率，256 字节包约需 22ms，加上 10ms 间隔，每包约 32ms，256KB 固件约需 64 秒
3. **非阻塞设计**：OTA 过程不应阻塞主循环，建议使用状态机 + 非阻塞方式实现
4. **进度上报**：进度信息存储在全局变量中，HTTP API 和 Web 页面通过轮询获取

### 11.2 测试建议

1. **正常升级测试**：验证 ESP 和 ARM 均可正常完成 OTA 升级
2. **断电恢复测试**：OTA 过程中断电，验证设备能正常启动（ESP 回滚，ARM 保持旧固件）
3. **网络异常测试**：模拟下载超时、连接失败等网络异常
4. **大文件测试**：使用接近大小限制的固件文件测试
5. **并发测试**：同时触发 ESP 和 ARM OTA，验证互斥机制
6. **版本回退测试**：使用低版本固件测试版本校验逻辑
7. **错误固件测试**：使用损坏的固件文件，验证校验和回滚机制

### 11.3 ARM 端开发要求

> 以下内容供 ARM 端开发人员参考：

1. **Bootloader**：GD32F303 需要实现 Bootloader，支持 IAP 模式
2. **Flash 布局**：建议使用双 App 分区方案（App1 + App2），支持回滚
3. **OTA 命令处理**：Bootloader 需要处理 0x10-0x14 命令
4. **MD5 计算**：ARM 端需要在接收完成后计算 MD5 并与 ESP32 发送的值比对
5. **版本存储**：固件版本号应存储在固定的 Flash 地址，便于 ESP32 查询

### 11.4 未来扩展

| 功能 | 优先级 | 说明 |
|------|--------|------|
| MQTT 触发 OTA | 中 | 通过 MQTT 命令触发 OTA，支持远程批量升级 |
| 固件签名验证 | 高 | 使用 RSA/ECDSA 签名验证固件真实性 |
| 差分升级 | 低 | 仅传输差异部分，减少下载量 |
| OTA 分批推送 | 低 | 支持按 SN 范围分批推送升级 |
| 升级日志上报 | 中 | OTA 结果通过 MQTT 上报到云端 |

---

## 附录 A：命令码常量定义

```c
// ========== OTA 命令码 ==========
#define CMD_ARM_OTA_START    0x10  // ESP32 → ARM: 通知进入 IAP 模式
#define CMD_ARM_OTA_DATA     0x11  // ESP32 → ARM: 发送固件数据包
#define CMD_ARM_OTA_END      0x12  // ESP32 → ARM: 传输结束
#define CMD_ARM_OTA_ACK      0x13  // ARM → ESP32: 确认响应
#define CMD_ARM_OTA_NACK     0x14  // ARM → ESP32: 请求重传
#define CMD_ARM_OTA_INFO     0x15  // ARM → ESP32: 固件信息上报

// ========== OTA 参数 ==========
#define OTA_PACKET_SIZE        256     // 默认分包大小
#define OTA_MAX_PACKET_SIZE    512     // 最大分包大小
#define OTA_MAX_RETRY          3       // 最大重传次数
#define OTA_PACKET_INTERVAL_MS 10      // 包间延时（ms）
#define OTA_ESP_MAX_SIZE       1048576 // ESP 固件最大 1MB
#define OTA_ARM_MAX_SIZE       262144  // ARM 固件最大 256KB
#define OTA_DOWNLOAD_TIMEOUT   60000   // 下载超时 60s
#define OTA_WRITE_TIMEOUT      120000  // 写入超时 120s
#define OTA_PACKET_TIMEOUT     5000    // 单包超时 5s
#define OTA_TOTAL_TIMEOUT      300000  // ARM OTA 总超时 300s
#define OTA_VERIFY_TIMEOUT     30000   // 校验超时 30s
```

## 附录 B：OTA 进度数据结构

```cpp
// OTA 目标类型
enum OTATarget {
    OTA_TARGET_NONE = 0,
    OTA_TARGET_ESP  = 1,
    OTA_TARGET_ARM  = 2
};

// OTA 状态
enum OTAStatus {
    OTA_STATUS_IDLE        = 0,
    OTA_STATUS_DOWNLOADING = 1,
    OTA_STATUS_WRITING     = 2,
    OTA_STATUS_VERIFYING   = 3,
    OTA_STATUS_DONE        = 4,
    OTA_STATUS_ERROR       = 5
};

// OTA 进度信息
struct OTAProgress {
    OTATarget target;
    OTAStatus status;
    uint8_t   progress;         // 0-100
    uint32_t  bytesProcessed;   // 已处理字节数
    uint32_t  bytesTotal;       // 总字节数
    uint32_t  packetIndex;      // 当前包序号（ARM OTA）
    uint32_t  packetTotal;      // 总包数（ARM OTA）
    uint32_t  retryCount;       // 重传次数
    String    errorMessage;     // 错误信息
    unsigned long startTime;    // 开始时间 (ms)
    unsigned long lastUpdateTime; // 最后更新时间 (ms)
};
```

## 附录 C：ARM OTA DATA 包二进制格式详解

CMD_ARM_OTA_DATA (0x11) 的 DATA 域使用二进制格式以最大化传输效率：

```
DATA 域结构（共 3 + data_len 字节）:
┌────────────────────┬────────────────────┬──────────────────────────┐
│  packet_index      │    data_len        │        data              │
│  (2 bytes, BE)     │    (1 byte)        │  (data_len bytes)        │
├────────────────────┼────────────────────┼──────────────────────────┤
│  高字节  │  低字节  │                    │                          │
│  [0]     │  [1]    │  [2]               │  [3] ... [3+n-1]        │
└────────────────────┴────────────────────┴──────────────────────────┘

示例：发送第 5 包，包含 256 字节数据
DATA[0] = 0x00        // packet_index 高字节
DATA[1] = 0x05        // packet_index 低字节
DATA[2] = 0x00        // data_len = 256 (0x00 表示 256，因为 1 字节最大 255)

特殊情况：当 data_len == 256 时，data_len 字段值为 0x00
当 data_len < 256 时，data_len 字段值为实际长度

DATA[3..258] = 固件数据 (256 bytes)
```

> **注意**：由于 `data_len` 为 1 字节（最大 255），当包大小为 256 时，`data_len` 字段值为 `0x00`。接收端需特殊处理：如果 `data_len == 0`，实际数据长度为 `packet_size`（默认 256）。

---

> **相关文档**:
> - `ARM_ESP32_UART_Protocol.md` — UART 帧协议详细说明
> - `MQTT协议文档.md` — MQTT 通信协议
> - `系统参数规范_48V离网逆变器.md` — 系统参数定义
