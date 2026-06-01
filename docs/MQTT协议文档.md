# 辰烁 48V 单相离网逆变器 — MQTT 协议文档

**版本**: V2.2
**更新时间**: 2026-05-26
**适用设备**: CS-I10-6k2 48V 单相离网逆变器（ESP32-C3 / ESP8266 WiFi 通讯模块）  
**相关文档**: `系统参数规范_48V离网逆变器.md`、`ARM_ESP32_UART_Protocol.md`

---

## 目录

1. [系统架构](#一系统架构)
2. [UART 帧协议（ARM ↔ ESP32）](#二uart-帧协议arm--esp32)
3. [MQTT 连接参数](#三mqtt-连接参数)
4. [主题格式](#四主题格式)
5. [数据上行（设备 → 云端）](#五数据上行设备--云端)
6. [数据下行（云端 → 设备）](#六数据下行云端--设备)
7. [心跳与在线状态](#七心跳与在线状态)
8. [设备配置命令](#八设备配置命令)
9. [数据库设计](#九数据库设计)
10. [ARM OTA 固件升级协议](#九arm-ota-固件升级协议)
11. [离线数据缓存机制](#十一离线数据缓存机制)
12. [注意事项](#十二注意事项)

---

## 一、系统架构

```
┌─────────────┐     UART      ┌─────────────────┐    MQTT    ┌──────────────────┐
│   ARM MCU   │◄────────────►│  ESP32-C3/8266  │◄─────────►│  后端服务器      │
│ (逆变器控制) │   二进制帧协议  │  (透明转发)       │            │  (云端/APP)      │
│ BMS+MPPT    │              │                  │            │                  │
└─────────────┘              └─────────────────┘            └──────────────────┘
```

**数据流向**：
- ARM → ESP32：`UART 帧协议`（二进制，转义，XOR 校验）
- ESP32 → 云端：`MQTT 协议`（JSON 格式），ESP32 自动上报 `status` 心跳
- 云端 → ARM：`MQTT → UART 帧协议`（ESP32 透明转发）

**ESP32 职责**：
1. 透传 ARM 数据（不解析 payload 内容，只做 SN 校验 + 主题路由）
2. 自动生成 `status` 心跳消息（在线状态 + RSSI + 本地 IP）
3. 管理 WiFi 连接、MQTT 连接、LWT 遗嘱
4. **离线数据缓存**：网络断开时自动存储数据到 NVS，恢复后补发（带时间戳）

---

## 二、UART 帧协议（ARM ↔ ESP32）

### 2.1 帧格式

```
┌────────┬────┬────────┬────────┬─────────────┬────────┐
│ 帧头    │ CMD │ 长度高  │ 长度低   │ 数据        │ XOR校验 │
│ 0xAA   │     │ LEN_H  │ LEN_L  │ DATA...     │        │
└────────┴────┴────────┴────────┴─────────────┴────────┘
```

| 字段 | 长度 | 说明 |
|------|------|------|
| 帧头 | 1字节 | 固定 `0xAA` |
| CMD | 1字节 | 命令码 |
| 长度 | 2字节 | 数据长度（big-endian） |
| 数据 | N字节 | 命令数据 |
| XOR | 1字节 | 校验字节 |

### 2.2 转义规则

帧头 `0xAA` 和转义字节 `0x55` 在数据中出现时需要转义：

| 原字节 | 转义序列 |
|--------|----------|
| `0xAA` | `0x55 0x01` |
| `0x55` | `0x55 0x00` |

> 转义只应用于帧头之后的所有字节，帧头的 `0xAA` 不转义。

### 2.3 XOR 校验

```
XOR = 0xAA ^ CMD ^ LEN_H ^ LEN_L ^ DATA[0] ^ DATA[1] ^ ... ^ DATA[N]
```

### 2.4 命令码

| CMD | 名称 | 方向 | 说明 |
|-----|------|------|------|
| `0x01` | SET_BROKER | ARM→ESP | 设置 MQTT 代理地址 |
| `0x02` | PUBLISH | ARM→ESP | 发布 MQTT 消息（ARM 数据上报） |
| `0x03` | COMMAND_RECV | ESP→ARM | 收到云端控制命令 |
| `0x04` | COMMAND_SEND | ARM→ESP | 命令执行结果回复 |
| `0x05` | FACTORY_RESET | ARM→ESP | 恢复出厂设置 |
| `0x06` | HEARTBEAT | ESP→ARM | 心跳信号（10s） |
| `0x08` | SET_SN | ARM→ESP | 设置设备序列号 |
| `0x09` | SET_KEY | ARM→ESP | 设置设备密钥 |
| `0x0A` | SET_AP_SSID | ARM→ESP | 设置热点名称 |
| `0x10` | ARM_OTA_START | ESP→ARM | ARM OTA 开始 |
| `0x11` | ARM_OTA_DATA | ESP→ARM | ARM OTA 数据 |
| `0x12` | ARM_OTA_END | ESP→ARM | ARM OTA 结束 |
| `0x13` | ARM_OTA_ACK | ARM→ESP | ARM OTA 应答 |
| `0x14` | ARM_OTA_NACK | ARM→ESP | ARM OTA 否认 |
| `0x15` | ARM_OTA_INFO | ESP→ARM | ARM OTA 固件信息查询 |
| `0xFE` | ACK | ESP→ARM | 命令应答成功（空数据） |
| `0xFF` | NACK | ESP→ARM | 命令应答失败（空数据） |

---

## 三、MQTT 连接参数

| 参数 | 值 | 说明 |
|------|-----|------|
| 协议 | mqtts:// | TLS 加密（端口 8883） |
| 端口 | 8883 | TLS 端口（默认） |
| Keepalive | 60s | 保活间隔 |
| Client ID | 设备 SN | 16 位 CS-SN-STD-001 V1.1 |
| Username | `CSKJ-INV-DEVICE-6K2` | 固定用户名 |
| Password | `CSKJINVDEVICE6K2` | 固定密码 |
| Clean Session | false | 非清理会话 |
| 默认 Broker | `jiuxiaoyw.online:8883` | 可通过配网页面修改 |

---

## 四、主题格式

```
cs_inv/{设备SN}/{子主题}
```

示例：`cs_inv/H1CNA00135000014/data/ac`

---

## 五、数据上行（设备 → 云端）

### 5.1 ARM 数据发布流程

ARM 通过 UART `CMD=0x02` 发送 JSON 到 ESP32，ESP32 透传到 MQTT：

**ARM → ESP32 帧格式**：
```
CMD=0x02
DATA={"sn":"H1CNA00135000014","topic":"data/ac","payload":"{\"voltage\":220.5,...}"}
```

**ESP32 → 云端**：
```
主题: cs_inv/H1CNA00135000014/data/ac
Payload: {"voltage":220.5,...}     ← 内层 payload 原样转发
```

### 5.2 上行主题总览

| 主题 | QoS | Retain | 频率 | 来源 | 说明 |
|------|-----|--------|------|------|------|
| `cs_inv/{sn}/status` | 1 | true | 60s | ESP32 自动 | 在线状态 + RSSI + IP |
| `cs_inv/{sn}/info` | 1 | false | 连接时 | ARM | 设备信息（上电一次） |
| `cs_inv/{sn}/data/ac` | 0 | false | 5s | ARM | 交流输出 |docs/MQTT协议文档.md
| `cs_inv/{sn}/data/battery` | 0 | false | 5s | ARM | 电池 BMS |
| `cs_inv/{sn}/data/pv` | 0 | false | 5s | ARM | 光伏 MPPT |
| `cs_inv/{sn}/data/status` | 0 | false | 5s | ARM | 逆变器系统状态 |
| `cs_inv/{sn}/data/energy` | 0 | false | 60s | ARM | 能量统计 |
| `cs_inv/{sn}/data/cells` | 0 | false | 30s | ARM | 电芯详细数据 |
| `cs_inv/{sn}/data/alarm` | 1 | false | 事件触发 | ARM | 告警/故障事件 |
| `cs_inv/{sn}/cmd/response` | 1 | false | 按需 | ARM | 控制命令执行结果 |
| `cs_inv/{sn}/ota/status` | 1 | false | 按需 | ESP32 | OTA 升级状态 |

### 5.3 各主题 Payload 格式

> 完整字段定义见 `系统参数规范_48V离网逆变器.md`

#### status（ESP32 自动生成，非 ARM 上报）

```json
{
  "online": true,
  "rssi": -45,
  "ip": "192.168.1.100"
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `online` | bool | 设备在线状态 |
| `rssi` | int | WiFi 信号强度 (dBm) |
| `ip` | string | 设备本地 IP 地址 |

#### info（ARM 上报，连接时一次）

```json
{
  "sn": "H1CNA00135000014",
  "model": "CS-I10-6k2",
  "manufacturer": "辰烁科技",
  "firmware_arm": "V1.2.3.20240510",
  "firmware_esp": "V1.0.5.20240420",
  "type": "离网逆变器",
  "rated_power": 6200,
  "rated_voltage": 220,
  "rated_freq": 50.0,
  "battery_voltage": 51.2,
  "battery_type": "LiFePO4",
  "cell_count": 16
}
```

#### data/ac（交流输出，5s）

```json
{
  "voltage": 220.5,
  "current": 8.52,
  "power": 1870.3,
  "frequency": 50.02,
  "load_percent": 30.2
}
```

#### data/battery（电池 BMS，5s）

```json
{
  "soc": 78.5,
  "soh": 96.2,
  "voltage": 51.2,
  "current": 5.5,
  "charge_state": "idle"
}
```

#### data/pv（光伏 MPPT，5s）

```json
{
  "pv_voltage": 85.3,
  "pv_current": 12.5,
  "pv_power": 1066.3,
  "mppt_state": "tracking"
}
```

#### data/status（逆变器系统状态，5s）

```json
{
  "state": "inverting",
  "fault_code": 0,
  "alarm_code": 0,
  "temp_inv": 48.5,
  "temp_mos": 55.2,
  "efficiency": 94.6
}
```

#### data/energy（能量统计，60s）

```json
{
  "daily_pv": 8.56,
  "total_pv": 1250.3,
  "runtime_hours": 8640
}
```

#### data/cells（电芯详情，30s）

```json
{
  "cell_count": 16,
  "voltages": [3.32, 3.33, 3.31, 3.32, 3.35, 3.30, 3.32, 3.31, 3.33, 3.32, 3.31, 3.32, 3.34, 3.29, 3.32, 3.31],
  "temps": [26.5, 26.8, 26.2, 26.5, 27.0, 26.0, 26.5, 26.3, 26.8, 26.5, 26.2, 26.5, 26.9, 25.8, 26.5, 26.4]
}
```

#### ota/status（OTA 升级状态，按需）

```json
{
  "target": "esp",
  "state": "downloading",
  "progress": 45,
  "message": "下载中..."
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `target` | string | 升级目标：`esp` 或 `arm` |
| `state` | string | 状态：`idle`、`downloading`、`uploading`、`verifying`、`done`、`error` |
| `progress` | int | 进度 0-100 |
| `message` | string | 状态消息 |

---

## 六、数据下行（云端 → 设备）

### 6.1 命令下发

云端通过 `cs_inv/{sn}/cmd` 下发命令，ESP32 透传到 ARM：

**云端 → ESP32 MQTT**：
```
主题: cs_inv/H1CNA00135000014/cmd
Payload: {"topic":"ac_on","payload":""}
```

**ESP32 → ARM UART**：
```
CMD=0x03 (COMMAND_RECV)
DATA={"topic":"ac_on","payload":""}
```

### 6.2 支持的控制命令

#### 逆变器控制

| 命令 | Payload | 说明 |
|------|---------|------|
| `ac_on` | `""` | 交流输出开启 |
| `ac_off` | `""` | 交流输出关闭 |
| `set_power_limit` | `{"value":80}` | 功率限制（额定功率百分比 0~100） |
| `query` | `""` | 立即上报全量数据 |

### 6.3 OTA 远程升级

云端通过 `cs_inv/{sn}/ota/cmd` 下发 OTA 命令：

**OTA 命令格式**：
```json
{
  "action": "start",
  "target": "esp",
  "url": "http://firmware.example.com/esp32-v1.0.6.bin"
}
```

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `action` | string | 是 | 操作：`start`、`stop`、`query` |
| `target` | string | 是 | 目标：`esp`（ESP32/ESP8266）、`arm`（ARM芯片） |
| `url` | string | start 时必填 | 固件下载 URL（HTTP） |

### 6.4 命令回复

ARM 执行后通过 `CMD=0x04` 回复，ESP32 转发到 `cs_inv/{sn}/cmd/response`。

---

## 七、心跳与在线状态

### 7.1 ESP32 → ARM 心跳

```
CMD=0x06 (HEARTBEAT)
DATA=(空)
```

发送间隔：10s，表示 ESP32 在线。ARM 超时 30s 未收到心跳则报警。

### 7.2 ESP32 → 云端 status 心跳

ESP32 每 60s 自动发布到 `cs_inv/{sn}/status`（QoS 1, Retain true）：

```json
{
  "online": true,
  "rssi": -45,
  "ip": "192.168.1.100"
}
```

### 7.3 LWT 遗嘱

MQTT 连接时设置 LWT，设备异常断开时自动发布：

```
主题: cs_inv/{sn}/status
Payload: {"online":false}
Retain: true
```

后端通过监听 `cs_inv/+/status` 的 retained 消息判断设备在线/离线。

---

## 八、设备配置命令

> 配置主要通过配网页面 `192.168.4.1` 进行，以下为 UART 命令方式。

### 8.1 设置 MQTT 代理 (SET_BROKER)

```
CMD=0x01
DATA="jiuxiaoyw.online:8883"
```

### 8.2 设置序列号 (SET_SN)

```
CMD=0x08
DATA="H1CNA00135000014"  (16字符 ASCII)
```

> SN 必须符合 CS-SN-STD-001 V1.1 标准，无效 SN 被 ESP32 拒绝（NACK）。SN 变更会触发 ESP32 重启。

### 8.3 设置设备密钥 (SET_KEY)

```
CMD=0x09
DATA="a1b2c3d4e5f6a1b2c3d4e5f6"
```

### 8.4 设置热点名称 (SET_AP_SSID)

```
CMD=0x0A
DATA="MyInverter"
```

留空恢复默认格式 `CS_INV_XXXXXX`。

### 8.5 恢复出厂 (FACTORY_RESET)

```
CMD=0x05
DATA=(空)
```

清除所有配置（WiFi、MQTT、SN、密钥），自动重启。

---

## 九、ARM OTA 固件升级协议

### 9.1 OTA 流程

```
ESP32                          ARM (GD32F303)
   |                                |
   |-------- CMD_OTA_START -------->|
   |<------- CMD_OTA_ACK -----------|
   |                                |
   |-------- CMD_OTA_DATA[0] ------>|
   |<------- CMD_OTA_ACK ------------|
   |        ...                     |
   |-------- CMD_OTA_DATA[N] ------>|
   |<------- CMD_OTA_ACK ------------|
   |                                |
   |-------- CMD_OTA_END ----------->|
   |<------- CMD_OTA_ACK ------------|
   |                                |
   |     (ARM 跳转到新固件)          |
```

### 9.2 命令详解

#### CMD_OTA_START (0x10)

```
数据格式: [total_size:4B][md5:16B][version_len:1B][version_str:nB]
示例: 00 00 F9 00  [32字节MD5]  05 56 31 2E 32 2E 33
      ^固件总大小 102400字节
```

#### CMD_OTA_DATA (0x11)

```
数据格式: [packet_index:2B][data:480B]
示例: 00 00  [480字节固件数据]
      ^包序号
```

#### CMD_OTA_END (0x12)

```
数据格式: [total_packets:2B][total_size:4B][md5:16B]
```

#### CMD_OTA_INFO (0x15)

查询当前固件信息，ARM 返回版本号和 MD5。

### 9.3 OTA 状态上报

ESP32 通过 MQTT 主题 `cs_inv/{sn}/ota/status` 上报进度：

| state | 说明 |
|-------|------|
| `idle` | 空闲 |
| `starting` | 启动中 |
| `uploading` | 传输中 |
| `verifying` | 校验中 |
| `done` | 完成 |
| `error` | 失败 |

---

## 十一、离线数据缓存机制

### 11.1 概述

当设备网络断开（WiFi 断开或 MQTT 服务器不可达）时，ESP32 自动将 ARM 上报的数据缓存到本地 NVS（非易失性存储），待网络恢复后自动补发。

```
正常状态:
  ARM → UART → ESP32 → MQTT 云端

网络断开:
  ARM → UART → ESP32 → NVS 缓存（带时间戳）

网络恢复:
  ESP32 → MQTT 云端（补发全部缓存数据）
```

### 11.2 缓存配置

| 参数 | 值 | 说明 |
|------|-----|------|
| 最大缓存条数 | 1000 | 超过后丢弃最旧数据（FIFO） |
| 数据保留时间 | 7 天 | 超过 7 天的数据在启动时自动清理 |
| 存储位置 | ESP32 NVS (Flash) | 断电不丢失 |
| 时间戳来源 | NTP（`ntp.aliyun.com`） | NTP 未同步时时间戳为 0 |

### 11.3 缓存触发条件

以下情况 ESP32 会将数据存入缓存而非直接发送：

1. **WiFi 未连接**（设备处于 AP 配网模式或 WiFi 连接中）
2. **MQTT 未连接**（WiFi 已连但 MQTT Broker 不可达）
3. **MQTT 连接中断**（运行中网络丢失）

> `mqtt_publish()` 返回 `-1` 表示未发送但已缓存。

### 11.4 缓存数据格式

缓存时自动添加 Unix 时间戳（秒）。MQTT 重连成功后，ESP32 按时间顺序（从旧到新）逐条发送缓存数据。

**正常发送格式**（MQTT 已连接时）：
```
主题: cs_inv/{sn}/data/ac
Payload: {"voltage":220.5,"current":8.52,...}
```

**补发格式**（从缓存发送时，自动包裹时间戳）：
```
主题: cs_inv/{sn}/data/ac
Payload: {"data":{"voltage":220.5,"current":8.52,...},"timestamp":1700000000}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `data` | object | 原始 payload 数据 |
| `timestamp` | uint32 | Unix 时间戳（秒），数据采集时间 |

> **后端注意**：收到带 `timestamp` 字段的消息时，应使用 `timestamp` 作为数据时间而非服务器接收时间。

### 11.5 补发流程

```
MQTT 连接成功
    │
    ├─ 1. 发布 online 状态
    ├─ 2. 订阅 cmd / ota/cmd 主题
    ├─ 3. 检查离线缓存数量
    │     │
    │     ├─ 有缓存 → 逐条发送（QoS 1）
    │     │           发送完毕后清空缓存
    │     │
    │     └─ 无缓存 → 正常运行
    │
    └─ 进入正常运行状态
```

### 11.6 缓存管理

| 操作 | 触发条件 | 说明 |
|------|----------|------|
| 自动清理过期数据 | 每次启动时 | 删除超过 7 天的数据 |
| FIFO 溢出处理 | 缓存满（1000条） | 自动丢弃最旧的一条 |
| 全量清空 | 补发完成后 | 所有缓存数据发送成功后清空 |
| 恢复出厂 | `CMD=0x05` 或按键 | 清除所有缓存数据 |

### 11.7 调试信息

串口日志中可通过以下关键字监控缓存状态：

| 日志关键字 | 说明 |
|-----------|------|
| `[MQTT] Not connected, cached:` | 数据已存入缓存 |
| `[OFFLINE_CACHE] Added:` | 缓存添加成功 |
| `[OFFLINE_CACHE] Cleanup:` | 过期数据清理 |
| `[MQTT] Found X cached items, flushing` | 开始补发缓存 |
| `[OFFLINE-SEND] topic=, ts=` | 单条缓存数据已发送 |
| `[OFFLINE_CACHE] Flush complete` | 补发完成 |
| `[DBG] Cached=X` | 定期状态报告（每60秒） |

---

## 十二、注意事项

1. **ESP32 是透传网关**：不解析 ARM 上报的 payload 内容，只做 SN 校验和主题路由。`status` 心跳是唯一由 ESP32 自身生成的消息。

2. **SN 校验**：ARM 通过 PUBLISH 或 SET_SN 设置 SN 时，ESP32 会校验 CS-SN-STD-001 V1.1 格式，无效 SN 返回 NACK。SN 变更会触发 MQTT 重连。

3. **离线数据时间戳**：网络断开期间的数据会自动添加 Unix 时间戳。后端应优先使用消息中的 `timestamp` 字段作为数据采集时间。NTP 未同步时时间戳可能为 0，后端需做容错处理。

4. **ARM OTA**：固件通过 UART 串口传输，每包 480 字节，带 ACK 重传机制。ESP32 支持流式传输，内存占用小（~8KB）。

5. **ESP32/ESP8266 兼容**：
   - ESP32-C3：完整功能
   - ESP8266 (ESP-07S)：需评估内存，建议禁用不需要的功能

6. **安全建议**：
   - 生产环境使用 mqtts:// (端口 8883)
   - 设备密钥加密存储
   - API 接口使用 JWT 认证

7. **消息去重**：后端建议对相同 SN + topic 的消息做时间窗口去重。

8. **离线缓存容量**：默认最多缓存 1000 条数据，保留 7 天。如果设备长时间断网（超过 7 天），超出部分数据会被丢弃。如需更大容量，可修改 `config.h` 中的 `OFFLINE_CACHE_MAX_ITEMS` 参数。

---

> **相关文档**:
> - `系统参数规范_48V离网逆变器.md` — 完整字段定义、数据校验范围、BMS 寄存器映射
> - `ARM_ESP32_UART_Protocol.md` — UART 帧协议详细说明
> - `OTA_Design.md` — OTA 升级系统设计
