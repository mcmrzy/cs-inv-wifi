# ARM 与 ESP32 通信协议文档

> **文档版本**: v3.0  
> **适用产品**: CS-I10-6k2 48V 单相离网逆变器  
> **适用对象**: ARM 端开发人员  
> **最后更新**: 2026-05-20  
> **相关文档**: `系统参数规范_48V离网逆变器.md`、`MQTT协议文档.md`、`BMS_ARM_Communication_Protocol.md`

---

## 目录

1. [概述](#1-概述)
2. [帧格式](#2-帧格式)
3. [命令码定义](#3-命令码定义)
4. [详细命令说明](#4-详细命令说明)
5. [MQTT 数据上报格式](#5-mqtt-数据上报格式)
6. [控制命令处理](#6-控制命令处理)
7. [示例代码](#7-示例代码)
8. [接收数据流程](#8-接收数据流程)
9. [注意事项](#9-注意事项)

---

## 1. 概述

### 1.1 协议用途

本文档定义 ARM MCU 与 ESP32-C3 WiFi 模块之间的 UART 通信协议。ESP32 作为 MQTT 网关，负责：

1. **透传 ARM 数据**：将 ARM 的数据原样转发到云端 MQTT Broker
2. **转发云端命令**：将云端的控制命令转发给 ARM
3. **自动维护连接**：WiFi 连接、MQTT 连接、心跳、LWT 遗嘱
4. **IP 定位**：WiFi 连接后自动获取设备地理位置

### 1.2 通信参数

| 参数 | 值 | 说明 |
|------|-----|------|
| 波特率 | 115200 | 标准波特率 |
| 数据位 | 8 | - |
| 停止位 | 1 | - |
| 校验 | 无 | - |
| 流控 | 无 | - |

### 1.3 物理连接

```
ARM MCU          ESP32-C3
-------          --------
TX  ────────────► RX (GPIO1)
RX ◄──────────── TX (GPIO0)
GND ──────────── GND
```

### 1.4 ESP32 职责

| 功能 | 说明 |
|------|------|
| 数据透传 | 不解析 ARM payload 内容，只做 SN 校验 + 主题路由 |
| status 心跳 | 每 60s 自动上报 `{"online":true,"rssi":-45,"location":{...}}` |
| LWT 遗嘱 | 异常断开时自动发布 `{"online":false}` |
| SN 校验 | 校验 CS-SN-STD-001 V1.1 格式，无效 SN 返回 NACK |

---

## 2. 帧格式

### 2.1 帧结构

```
+--------+--------+--------+--------+--------+--------+--------+
| HEADER |  CMD   | LEN_H  | LEN_L  |  DATA  |  ...   |  XOR   |
|  0xAA  |  1B    |  1B    |  1B    |  n B   |        |  1B    |
+--------+--------+--------+--------+--------+--------+--------+
```

| 字段 | 长度 | 说明 |
|------|------|------|
| HEADER | 1B | 帧头，固定 `0xAA` |
| CMD | 1B | 命令码，见第3节 |
| LEN_H | 1B | 数据长度高字节 (len >> 8) |
| LEN_L | 1B | 数据长度低字节 (len & 0xFF) |
| DATA | n B | 数据负载，最大 512 字节 |
| XOR | 1B | 校验和，计算方式见下文 |

### 2.2 校验和计算

```c
uint8_t calc_xor(uint8_t cmd, uint16_t len, const uint8_t* data) {
    uint8_t xor = 0xAA;  // FRAME_HEADER
    xor ^= cmd;
    xor ^= (uint8_t)((len >> 8) & 0xFF);
    xor ^= (uint8_t)(len & 0xFF);
    for (uint16_t i = 0; i < len; i++) {
        xor ^= data[i];
    }
    return xor;
}
```

### 2.3 字节转义

由于帧头 `0xAA` 可能出现在数据域中，需要进行字节转义：

| 原始字节 | 转义后 | 说明 |
|----------|--------|------|
| `0xAA` | `0x55 0x01` | 帧头转义 |
| `0x55` | `0x55 0x00` | 转义字符本身转义 |

**转义规则**:
- 发送时：HEADER 之后的所有字节（CMD、LEN、DATA、XOR），如果遇到 `0xAA` 或 `0x55`，需要转义
- 接收时：收到 `0x55` 后，下一个字节需要反转义

---

## 3. 命令码定义

### 3.1 命令码表

| 命令码 | 名称 | 方向 | 说明 |
|--------|------|------|------|
| 0x01 | CMD_SET_BROKER | ARM → ESP32 | 设置 MQTT Broker 地址 |
| 0x02 | CMD_PUBLISH | ARM → ESP32 | 发布数据到 MQTT |
| 0x03 | CMD_COMMAND_RECV | ESP32 → ARM | ESP32 收到 MQTT 命令，转发给 ARM |
| 0x04 | CMD_COMMAND_SEND | ARM → ESP32 | ARM 发送命令执行结果 |
| 0x05 | CMD_FACTORY_RESET | ARM → ESP32 | 恢复出厂设置 |
| 0x06 | CMD_HEARTBEAT | ESP32 → ARM | ESP32 心跳（60s） |
| 0x08 | CMD_SET_SN | ARM → ESP32 | 设置设备序列号 |
| 0x09 | CMD_SET_KEY | ARM → ESP32 | 设置设备密钥 |
| 0x0A | CMD_SET_AP_SSID | ARM → ESP32 | 设置热点名称 |
| 0xFE | CMD_ACK | ESP32 → ARM | 确认响应 |
| 0xFF | CMD_NACK | ESP32 → ARM | 否定响应 |

### 3.2 响应机制

- **ACK (0xFE)**: 命令执行成功
- **NACK (0xFF)**: 命令执行失败或格式错误

**需要等待 ACK 的命令**：
- CMD_SET_BROKER (0x01)
- CMD_SET_SN (0x08)
- CMD_SET_KEY (0x09)
- CMD_SET_AP_SSID (0x0A)
- CMD_FACTORY_RESET (0x05)

**不需要等待 ACK 的命令**：
- CMD_PUBLISH (0x02)：发送后即可继续其他操作
- CMD_COMMAND_SEND (0x04)：单向通知

---

## 4. 详细命令说明

### 4.1 CMD_SET_BROKER (0x01) — 设置 MQTT Broker

**方向**: ARM → ESP32  
**DATA 内容**: `host:port` 的 ASCII 字符串（不含空字符）

**完整帧示例**（设置 Broker 为 `jiuxiaoyw.online:8883`）：

```
┌────────┬────────┬────────┬────────┬──────────────────────────────────┬────────┐
│ HEADER │  CMD   │ LEN_H  │ LEN_L  │           DATA (22字节)           │  XOR   │
│  0xAA  │  0x01  │  0x00  │  0x16  │ j i u x i a o y w . o n l i n e  │ 0xXX   │
│        │        │        │        │ : 8 8 8 3                        │        │
└────────┴────────┴────────┴────────┴──────────────────────────────────┴────────┘

十六进制: AA 01 00 16 6A 69 75 78 69 61 6F 79 77 2E 6F 6E 6C 69 6E 65 3A 38 38 38 33 [XOR]
```

**说明**:
- ESP32 收到后会保存到 NVS，并尝试连接新的 Broker
- 如果连接成功，会发送 ACK；失败发送 NACK

---

### 4.2 CMD_PUBLISH (0x02) — 发布 MQTT 消息

**方向**: ARM → ESP32  
**DATA 内容**: JSON 字符串（**必须为标准 JSON 格式，key 必须有双引号**）

**DATA JSON 结构**:
```json
{
    "topic": "data/ac",
    "payload": "{\"voltage\":220.5,\"current\":8.52}",
    "sn": "H1CNA0013500001O"
}
```

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| topic | string | 是 | MQTT 子主题（不含前缀 `cs_inv/{sn}/`） |
| payload | string | 是 | 要发布的 JSON 数据（字符串形式，需转义引号） |
| sn | string | 否 | 设备序列号（用于 ESP32 校验，可选） |

**完整帧示例**（发布 AC 数据，DATA 长度假设为 150 字节）：

```
┌────────┬────────┬────────┬────────┬──────────────────────────────────┬────────┐
│ HEADER │  CMD   │ LEN_H  │ LEN_L  │        DATA (JSON字符串)          │  XOR   │
│  0xAA  │  0x02  │  0x00  │  0x96  │ {"topic":"data/ac","payload":...  │ 0xXX   │
└────────┴────────┴────────┴────────┴──────────────────────────────────┴────────┘

十六进制: AA 02 00 96 7B 22 74 6F 70 69 63 22 3A 22 64 61 74 61 2F 61 63 22 ... [XOR]
```

**说明**:
- ESP32 会将消息发布到 `cs_inv/{sn}/{topic}` 主题
- 不需要等待 ACK，发送后即可继续其他操作
- **重要**: payload 内部必须是有效的 JSON 字符串，需要对引号进行转义（`\"`）

---

### 4.3 CMD_COMMAND_RECV (0x03) — 接收 MQTT 命令

**方向**: ESP32 → ARM  
**DATA 内容**: JSON 字符串

**DATA JSON 结构**（无参数命令）:
```json
{
    "topic": "ac_on",
    "payload": ""
}
```

**DATA JSON 结构**（带参数命令）:
```json
{
    "topic": "set_power_limit",
    "payload": "{\"value\":80}"
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| topic | string | 命令主题（如 `ac_on`、`bms/charge_current`） |
| payload | string | 命令参数（JSON 字符串或空字符串） |

**完整帧示例**（收到 `ac_on` 命令）：

```
┌────────┬────────┬────────┬────────┬──────────────────────────────────┬────────┐
│ HEADER │  CMD   │ LEN_H  │ LEN_L  │        DATA (28字节)              │  XOR   │
│  0xAA  │  0x03  │  0x00  │  0x1C  │ {"topic":"ac_on","payload":""}   │ 0xXX   │
└────────┴────────┴────────┴────────┴──────────────────────────────────┴────────┘

十六进制: AA 03 00 1C 7B 22 74 6F 70 69 63 22 3A 22 61 63 5F 6F 6E 22 2C 22 70 61 79 6C 6F 61 64 22 3A 22 22 7D [XOR]
```

**说明**:
- 当 ESP32 从 MQTT 收到 `cs_inv/{sn}/cmd` 主题的消息时，会通过此命令转发给 ARM
- ARM 需要解析 topic 和 payload，执行相应操作
- 执行完成后，ARM 应通过 CMD_COMMAND_SEND (0x04) 返回执行结果

---

### 4.4 CMD_COMMAND_SEND (0x04) — 发送命令执行结果

**方向**: ARM → ESP32  
**DATA 内容**: JSON 字符串

**DATA JSON 结构**:
```json
{
    "result": "ok",
    "cmd": "bms/charge_current",
    "message": "充电电流已设置为 50.0A",
    "timestamp": 1715000000
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| result | string | `ok` 成功 / `error` 失败 |
| cmd | string | 原始命令 topic |
| message | string | 执行结果描述 |
| timestamp | int | Unix 时间戳（秒） |

**完整帧示例**（命令执行成功）：

```
┌────────┬────────┬────────┬────────┬──────────────────────────────────┬────────┐
│ HEADER │  CMD   │ LEN_H  │ LEN_L  │        DATA (JSON字符串)          │  XOR   │
│  0xAA  │  0x04  │  0x00  │  0xXX  │ {"result":"ok","cmd":"ac_on",...  │ 0xXX   │
└────────┴────────┴────────┴────────┴──────────────────────────────────┴────────┘
```

**说明**:
- ESP32 将结果转发到 `cs_inv/{sn}/cmd/response` 主题

---

### 4.5 CMD_SET_SN (0x08) — 设置序列号

**方向**: ARM → ESP32  
**DATA 内容**: ASCII 字符串，16 字符（CS-SN-STD-001 V1.1）

**完整帧示例**（设置 SN 为 `H1CNA0013500001O`）：

```
┌────────┬────────┬────────┬────────┬──────────────────────────────────┬────────┐
│ HEADER │  CMD   │ LEN_H  │ LEN_L  │        DATA (16字节)              │  XOR   │
│  0xAA  │  0x08  │  0x00  │  0x10  │ H 1 C N A 0 0 1 3 5 0 0 0 0 0 1 O │ 0xXX   │
└────────┴────────┴────────┴────────┴──────────────────────────────────┴────────┘

十六进制: AA 08 00 10 48 31 43 4E 41 30 30 31 33 35 30 30 30 30 30 31 4F [XOR]
```

**SN 格式规范 (CS-SN-STD-001 V1.1)**:

```
MM-CC-PPPP-YW-SSSSS-C
│  │  │    │  │     │
│  │  │    │  │     └─ 校验位 (CRC-16-CCITT)
│  │  │    │  └─────── 序列号 (5位数字)
│  │  │    └────────── 年月码 (年码1位 + 月码1位)
│  │  └─────────────── 产品型号 (4位)
│  └────────────────── 客户代码 (2位)
└──────────────────── 制造商代码 (2位)
```

**说明**:
- ESP32 会校验 SN 格式，无效 SN 返回 NACK
- SN 变更会触发 ESP32 重启，重新连接 MQTT
- SN 不变时只保存，不重启

---

### 4.6 CMD_SET_KEY (0x09) — 设置设备密钥

**方向**: ARM → ESP32  
**DATA 内容**: ASCII 字符串，最大 64 字节

**完整帧示例**（设置密钥为 `a1b2c3d4e5f6`）：

```
┌────────┬────────┬────────┬────────┬──────────────────────────────────┬────────┐
│ HEADER │  CMD   │ LEN_H  │ LEN_L  │        DATA (12字节)              │  XOR   │
│  0xAA  │  0x09  │  0x00  │  0x0C  │ a 1 b 2 c 3 d 4 e 5 f 6          │ 0xXX   │
└────────┴────────┴────────┴────────┴──────────────────────────────────┴────────┘

十六进制: AA 09 00 0C 61 31 62 32 63 33 64 34 65 35 66 36 [XOR]
```

**说明**:
- 用于设备认证（当前 MQTT 使用固定用户名密码，此字段保留）

---

### 4.7 CMD_SET_AP_SSID (0x0A) — 设置热点名称

**方向**: ARM → ESP32  
**DATA 内容**: ASCII 字符串，2-31 字节

**完整帧示例**（设置热点名为 `MyInverter`）：

```
┌────────┬────────┬────────┬────────┬──────────────────────────────────┬────────┐
│ HEADER │  CMD   │ LEN_H  │ LEN_L  │        DATA (10字节)              │  XOR   │
│  0xAA  │  0x0A  │  0x00  │  0x0A  │ M y I n v e r t e r              │ 0xXX   │
└────────┴────────┴────────┴────────┴──────────────────────────────────┴────────┘

十六进制: AA 0A 00 0A 4D 79 49 6E 76 65 72 74 65 72 [XOR]
```

**说明**:
- 设置 ESP32 的 AP 热点名称
- 留空（长度为 0）恢复默认格式 `CS_INV_XXXXXX`
- 设置后 ESP32 自动重启

---

### 4.8 CMD_FACTORY_RESET (0x05) — 恢复出厂设置

**方向**: ARM → ESP32  
**DATA 内容**: 无数据（长度为 0）

**完整帧示例**：

```
┌────────┬────────┬────────┬────────┬────────┐
│ HEADER │  CMD   │ LEN_H  │ LEN_L  │  XOR   │
│  0xAA  │  0x05  │  0x00  │  0x00  │  0xAF  │
└────────┴────────┴────────┴────────┴────────┘

十六进制: AA 05 00 00 AF
```

**说明**:
- ESP32 清除所有配置（SN、WiFi、Broker、密钥）
- 清除后自动重启，进入 AP 配网模式

---

### 4.9 CMD_HEARTBEAT (0x06) — 心跳包

**方向**: ESP32 → ARM  
**DATA 内容**: 无数据（长度为 0）

**完整帧示例**：

```
┌────────┬────────┬────────┬────────┬────────┐
│ HEADER │  CMD   │ LEN_H  │ LEN_L  │  XOR   │
│  0xAA  │  0x06  │  0x00  │  0x00  │  0xAC  │
└────────┴────────┴────────┴────────┴────────┘

十六进制: AA 06 00 00 AC
```

**说明**:
- ESP32 每 60 秒发送一次心跳
- 表示 ESP32 在线且正常工作
- ARM 收到后无需回复

---

## 5. MQTT 数据上报格式

ARM 通过 CMD_PUBLISH 上报数据，ESP32 透传到 MQTT。以下是各主题的 payload 格式：

> 完整字段定义见 `系统参数规范_48V离网逆变器.md`

### 5.1 主题总览

| topic | 频率 | 说明 |
|-------|------|------|
| `info` | 连接时一次 | 设备信息 |
| `data/ac` | 5s | 交流输出 |
| `data/battery` | 5s | 电池 BMS |
| `data/pv` | 5s | 光伏 MPPT |
| `data/status` | 5s | 逆变器系统状态 |
| `data/energy` | 60s | 能量统计 |
| `data/cells` | 30s | 电芯详细数据 |
| `data/alarm` | 事件触发 | 告警/故障事件 |

### 5.2 info（设备信息）

```json
{
    "sn": "H1CNA0013500001O",
    "model": "CS1P3K-48",
    "manufacturer": "辰烁科技",
    "firmware_arm": "V1.0.0",
    "firmware_esp": "V1.0.0",
    "type": "off_grid",
    "phase": "single",
    "rated_power": 3000,
    "rated_voltage": 220,
    "rated_freq": 50,
    "battery_voltage": 48,
    "battery_types": ["LiFePO4", "NCM", "LeadAcid"],
    "mppt_count": 1,
    "pv_max_voltage": 150,
    "pv_max_power": 2000,
    "bms_count": 1,
    "cell_count": 16
}
```

### 5.3 data/ac（交流输出）

```json
{
    "voltage": 220.5,
    "current": 8.52,
    "power": 1870.3,
    "apparent": 1875.6,
    "reactive": 120.5,
    "frequency": 50.02,
    "pf": 0.997,
    "load_percent": 62.4,
    "thd_v": 1.8,
    "thd_i": 2.5,
    "dc_injection": 15.0
}
```

### 5.4 data/battery（电池 BMS）

```json
{
    "soc": 78.5,
    "soh": 96.2,
    "voltage": 51.2,
    "current": 25.5,
    "power": 1305.6,
    "capacity_remain": 78.5,
    "capacity_total": 100.0,
    "cycle_count": 152,
    "temp_max": 28.5,
    "temp_min": 25.0,
    "cell_volt_max": 3.35,
    "cell_volt_min": 3.28,
    "cell_volt_diff": 0.07,
    "charge_state": "charging",
    "battery_type": "LiFePO4",
    "protect_status1": 0,
    "protect_status2": 0
}
```

### 5.5 data/pv（光伏 MPPT）

```json
{
    "pv_voltage": 85.3,
    "pv_current": 12.5,
    "pv_power": 1066.3,
    "mppt_state": "tracking"
}
```

### 5.6 data/status（逆变器系统状态）

```json
{
    "state": "inverting",
    "fault_code": 0,
    "alarm_code": 0,
    "temp_inv": 48.5,
    "temp_mos": 55.2,
    "temp_ambient": 32.0,
    "dc_bus_voltage": 380.0,
    "fan_speed": 60,
    "efficiency": 94.6
}
```

### 5.7 data/energy（能量统计）

```json
{
    "daily_pv": 8.56,
    "total_pv": 1250.3,
    "daily_charge": 7.80,
    "total_charge": 1100.5,
    "daily_discharge": 6.20,
    "total_discharge": 980.8,
    "daily_load": 6.00,
    "total_load": 950.2,
    "runtime_hours": 8640
}
```

### 5.8 data/cells（电芯详情）

```json
{
    "cell_count": 16,
    "voltages": [3.32, 3.33, 3.31, 3.32, 3.35, 3.30, 3.32, 3.31, 3.33, 3.32, 3.31, 3.32, 3.34, 3.29, 3.32, 3.31],
    "temps": [26.5, 26.8, 26.2, 26.5, 27.0, 26.0, 26.5, 26.3, 26.8, 26.5, 26.2, 26.5, 26.9, 25.8, 26.5, 26.4],
    "charge_ah_total": 12580.3,
    "discharge_ah_total": 12450.6
}
```

### 5.9 data/alarm（告警事件）

```json
{
    "event": "alarm",
    "timestamp": 1715000000,
    "source": "bms",
    "fault_code": 257,
    "fault_desc": "单体过压保护",
    "alarm_code": 0,
    "trigger": {
        "cell_no": 5,
        "cell_voltage": 3.65,
        "threshold": 3.60
    }
}
```

---

## 6. 控制命令处理

### 6.1 命令列表

ARM 收到 CMD_COMMAND_RECV 后，需解析 topic 并执行相应操作：

#### 逆变器控制

| topic | payload | 说明 |
|-------|---------|------|
| `ac_on` | `""` | 交流输出开启 |
| `ac_off` | `""` | 交流输出关闭 |
| `set_voltage` | `{"value":220}` | 设定输出电压 (V) |
| `set_freq` | `{"value":50}` | 设定输出频率 (Hz) |
| `set_power_limit` | `{"value":80}` | 功率限制（额定功率百分比 0~100） |
| `eco_mode` | `{"value":1}` | 节能模式：0=关, 1=开 |
| `restart` | `""` | 软重启 |
| `set_report_interval` | `{"value":5}` | 上报间隔 (秒) |
| `query` | `""` | 立即上报全量数据 |

#### BMS 控制（ARM 转发到 Modbus）

| topic | payload | BMS 寄存器 | 说明 |
|-------|---------|-----------|------|
| `bms/charge_enable` | `{"value":1}` | 0x1200 | 充放电使能 |
| `bms/charge_current` | `{"value":500}` | 0x1201 | 最大充电电流（×0.1A） |
| `bms/discharge_current` | `{"value":1000}` | 0x1202 | 最大放电电流（×0.1A） |
| `bms/charge_volt` | `{"value":584}` | 0x1203 | 充电截止电压（×0.1V） |
| `bms/discharge_volt` | `{"value":430}` | 0x1204 | 放电截止电压（×0.1V） |
| `bms/balance_enable` | `{"value":1}` | 0x1205 | 均衡使能 |
| `bms/balance_threshold` | `{"value":50}` | 0x1206 | 均衡启动压差 (mV) |

#### MPPT 控制

| topic | payload | 说明 |
|-------|---------|------|
| `mppt_on` | `""` | 启用 MPPT |
| `mppt_off` | `""` | 禁用 MPPT |
| `mppt_power_limit` | `{"value":1000}` | PV 功率限制 (W) |

### 6.2 命令处理流程

```
1. 收到 CMD_COMMAND_RECV (0x03)
2. 解析 JSON 获取 topic 和 payload
3. 根据 topic 执行相应操作
4. 构造结果 JSON
5. 通过 CMD_COMMAND_SEND (0x04) 发送结果
```

### 6.3 命令回复示例

**成功**:
```json
{
    "result": "ok",
    "cmd": "bms/charge_current",
    "message": "充电电流已设置为 50.0A",
    "timestamp": 1715000000
}
```

**失败**:
```json
{
    "result": "error",
    "cmd": "set_voltage",
    "message": "电压值超出范围（200-250V）",
    "timestamp": 1715000000
}
```

---

## 7. 示例代码

### 7.1 C 语言实现

```c
#include <stdint.h>
#include <string.h>

#define FRAME_HEADER     0xAA
#define FRAME_ESCAPE     0x55
#define FRAME_ESC_HEADER 0x01
#define FRAME_ESC_ESCAPE 0x00
#define FRAME_DATA_MAX   512

// 命令码
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

// 计算校验和
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
void send_escaped_byte(uint8_t b, void (*send_byte)(uint8_t)) {
    if (b == FRAME_HEADER) {
        send_byte(FRAME_ESCAPE);
        send_byte(FRAME_ESC_HEADER);
    } else if (b == FRAME_ESCAPE) {
        send_byte(FRAME_ESCAPE);
        send_byte(FRAME_ESC_ESCAPE);
    } else {
        send_byte(b);
    }
}

// 发送完整帧
void send_frame(uint8_t cmd, const uint8_t* data, uint16_t len, 
                void (*send_byte)(uint8_t)) {
    uint8_t xor = calc_xor(cmd, len, data);
    
    // 发送帧头（不转义）
    send_byte(FRAME_HEADER);
    
    // 发送 CMD（需要转义）
    send_escaped_byte(cmd, send_byte);
    
    // 发送长度（需要转义）
    send_escaped_byte((uint8_t)((len >> 8) & 0xFF), send_byte);
    send_escaped_byte((uint8_t)(len & 0xFF), send_byte);
    
    // 发送数据（需要转义）
    for (uint16_t i = 0; i < len; i++) {
        send_escaped_byte(data[i], send_byte);
    }
    
    // 发送校验和（需要转义）
    send_escaped_byte(xor, send_byte);
}

// 示例：发布 AC 数据
void publish_ac_data(void (*send_byte)(uint8_t)) {
    // 构造 payload
    const char* payload = "{\"voltage\":220.5,\"current\":8.52,\"power\":1870.3}";
    
    // 构造完整 JSON
    char json[256];
    snprintf(json, sizeof(json),
        "{\"topic\":\"data/ac\",\"payload\":\"%s\",\"sn\":\"H1CNA0013500001O\"}",
        payload);
    
    send_frame(CMD_PUBLISH, (const uint8_t*)json, strlen(json), send_byte);
}

// 示例：发送命令执行结果
void send_command_result(void (*send_byte)(uint8_t), 
                         const char* cmd, const char* result, const char* message) {
    char json[256];
    snprintf(json, sizeof(json),
        "{\"result\":\"%s\",\"cmd\":\"%s\",\"message\":\"%s\",\"timestamp\":%ld}",
        result, cmd, message, get_unix_timestamp());
    
    send_frame(CMD_COMMAND_SEND, (const uint8_t*)json, strlen(json), send_byte);
}
```

### 7.2 Python 实现（测试用）

```python
import serial
import json

FRAME_HEADER = 0xAA
FRAME_ESCAPE = 0x55

def calc_xor(cmd, data):
    xor = FRAME_HEADER
    xor ^= cmd
    xor ^= (len(data) >> 8) & 0xFF
    xor ^= len(data) & 0xFF
    for b in data:
        xor ^= b
    return xor

def escape_byte(b):
    if b == FRAME_HEADER:
        return bytes([FRAME_ESCAPE, 0x01])
    elif b == FRAME_ESCAPE:
        return bytes([FRAME_ESCAPE, 0x00])
    else:
        return bytes([b])

def send_frame(ser, cmd, data):
    frame = bytes([FRAME_HEADER])
    frame += escape_byte(cmd)
    frame += escape_byte((len(data) >> 8) & 0xFF)
    frame += escape_byte(len(data) & 0xFF)
    for b in data:
        frame += escape_byte(b)
    frame += escape_byte(calc_xor(cmd, data))
    ser.write(frame)

# 示例：发布数据
def publish_data(ser, topic, payload_dict, sn="H1CNA0013500001O"):
    payload_str = json.dumps(payload_dict, ensure_ascii=False)
    obj = {"topic": topic, "payload": payload_str, "sn": sn}
    data = json.dumps(obj, ensure_ascii=False).encode('utf-8')
    send_frame(ser, 0x02, data)

# 使用示例
if __name__ == "__main__":
    ser = serial.Serial("COM3", 115200, timeout=1)
    
    # 发布 AC 数据
    publish_data(ser, "data/ac", {
        "voltage": 220.5,
        "current": 8.52,
        "power": 1870.3,
        "frequency": 50.02,
        "pf": 0.997
    })
    
    ser.close()
```

---

## 8. 接收数据流程

### 8.1 状态机实现

```c
typedef enum {
    RX_WAIT_HEADER,
    RX_WAIT_CMD,
    RX_WAIT_LEN_H,
    RX_WAIT_LEN_L,
    RX_WAIT_DATA,
    RX_WAIT_XOR
} RxState;

typedef struct {
    RxState state;
    uint8_t cmd;
    uint16_t len;
    uint16_t recv_len;
    uint8_t data[FRAME_DATA_MAX];
    uint8_t xor;
    uint8_t esc_next;
} RxContext;

void rx_init(RxContext* ctx) {
    memset(ctx, 0, sizeof(RxContext));
    ctx->state = RX_WAIT_HEADER;
}

// 处理接收到的字节
// 返回 1 表示完整帧接收成功
int rx_process_byte(RxContext* ctx, uint8_t b, void (*on_frame)(uint8_t cmd, const uint8_t* data, uint16_t len)) {
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

---

## 9. 注意事项

### 9.1 通信规范

1. **波特率**: 必须使用 115200，双方需保持一致
2. **帧间隔**: 建议两帧之间间隔至少 10ms
3. **数据长度**: 单帧数据最大 512 字节，超过需分帧
4. **JSON 格式**: CMD_PUBLISH 的数据必须是有效的 JSON 字符串，**key 必须用双引号包裹**
5. **ACK 超时**: 对于需要 ACK 的命令，建议设置 1-2 秒超时

### 9.2 数据校验

ARM 端应确保发送的电气数据在合理物理范围内：

| 参数 | 最小值 | 最大值 | 说明 |
|------|--------|--------|------|
| AC 电压 | 200V | 250V | 单相离网输出 |
| AC 功率 | 0W | 额定功率 | 不超过逆变器额定值 |
| 频率 | 49.5Hz | 50.5Hz | 离网自生成频率 |
| 功率因数 | 0.0 | 1.0 | **严禁超过 1.0** |
| PV 电压 | 0V | 150V | 48V 系统 MPPT 范围 |
| 电池电压 | 40V | 60V | 16S LiFePO4 典型范围 |
| SOC | 0% | 100% | - |
| 温度 | -20°C | 85°C | 设备工作温度 |

### 9.3 离网特性

- **频率**：由逆变器自身产生，非电网同步
- **电池电流**：正值=充电，负值=放电
- **无并网/旁路模式**：纯离网运行

### 9.4 调试建议

1. 使用逻辑分析仪或 USB 转串口工具监听通信
2. 先测试 CMD_HEARTBEAT 确保物理连接正常
3. 逐步增加复杂度：先测试 CMD_SET_SN，再测试 CMD_PUBLISH
4. 检查校验和计算是否正确
5. 确认字节转义逻辑无误
6. 验证 JSON 格式是否标准（可使用在线 JSON 校验工具）

---

## 附录：MQTT 主题规范

ESP32 会自动订阅和发布以下主题：

| 主题 | 方向 | 说明 |
|------|------|------|
| `cs_inv/{sn}/status` | ESP32 → 云端 | 在线状态 + RSSI + IP 定位（ESP32 自动生成） |
| `cs_inv/{sn}/info` | ARM → 云端 | 设备信息（ARM 上报） |
| `cs_inv/{sn}/data/ac` | ARM → 云端 | 交流输出数据 |
| `cs_inv/{sn}/data/battery` | ARM → 云端 | 电池 BMS 数据 |
| `cs_inv/{sn}/data/pv` | ARM → 云端 | 光伏 MPPT 数据 |
| `cs_inv/{sn}/data/status` | ARM → 云端 | 逆变器系统状态 |
| `cs_inv/{sn}/data/energy` | ARM → 云端 | 能量统计 |
| `cs_inv/{sn}/data/cells` | ARM → 云端 | 电芯详细数据 |
| `cs_inv/{sn}/data/alarm` | ARM → 云端 | 告警/故障事件 |
| `cs_inv/{sn}/cmd` | 云端 → ARM | 控制命令下发 |
| `cs_inv/{sn}/cmd/response` | ARM → 云端 | 命令执行结果 |

其中 `{sn}` 为设备序列号，如 `H1CNA0013500001O`。

> **注意**: `status` 主题由 ESP32 自动生成，ARM 无需上报。包含 `online`、`rssi`、`location` 字段。
