# CS INV 逆变器 WiFi 模块 — BLE 配网协议规范

> 版本：V1.2  
> 日期：2026-07-08  
> 适用设备：ESP32-C3 / ESP32-C2 WiFi 通讯模块

---

## 1. 概述

CS INV 逆变器 WiFi 模块支持通过 BLE（蓝牙低功耗）进行 WiFi 配网。用户无需切换手机网络，直接通过 App 即可将 WiFi 凭据发送给设备。

**配网方式对比**：

| 方式 | 需切网络 | 兼容性 | 状态 |
|------|---------|--------|------|
| BLE 配网 | 否 | 好 | **当前使用** |
| SmartConfig | 否 | 一般 | 已弃用 |
| AP 热点配网 | 是 | 好 | 保留（备用） |

---

## 2. BLE 广播

### 2.1 设备广播名

```
CS_INV_<完整SN>
```

示例：SN 为 `H1CNA00135000014` 的设备，广播名为 `CS_INV_H1CNA00135000014`。

**App 获取 SN 的三种途径**：

| 途径 | UUID | 值 |
|------|------|-----|
| 广播名（Scan Response） | — | `CS_INV_完整SN` |
| GAP Device Name | `0x2A00` | 完整 SN |
| 自定义 SN 特征 | `43534956-534e-...` | 完整 SN |

### 2.2 广播包结构

| 包类型 | 内容 | 用途 |
|--------|------|------|
| 主广播包 (ADV_IND) | 服务 UUID | App 过滤 CS INV 设备 |
| Scan Response (SCAN_RSP) | 完整设备名（含 SN） | 用户识别具体设备 |

**说明**：BLE 传统广播每包限 31 字节，128-bit UUID 占 18 字节，剩余空间不足以放入完整 SN，因此设备名放在 Scan Response 中。主流 BLE 库会自动合并两包，App 端无需特殊处理。

### 2.3 广播参数

| 参数 | 值 |
|------|-----|
| 广播间隔 | NimBLE 默认（~100ms） |
| 发射功率 | +9 dBm |
| 超时 | 5 分钟（300 秒） |
| 连接后行为 | 停止广播；断开后自动恢复广播 |

---

## 3. GATT 服务定义

### 3.1 服务 UUID

```
43534956-5052-1000-8000-00805f9b34fb
```

UUID 编码说明：`43534956` = ASCII "CSIV"，`5052` = ASCII "PR"（Provisioning）。

### 3.2 特征列表

**GAP 标准特征**：

| 名称 | UUID | 权限 | 数据格式 | 示例值 |
|------|------|------|----------|--------|
| Device Name | `0x2A00` | 只读 | UTF-8 字符串 | `H1CNA00135000014` |

**自定义配网特征**：

| 名称 | UUID | 权限 | 数据格式 | 示例值 |
|------|------|------|----------|--------|
| 设备 SN | `43534956-534e-1000-8000-00805f9b34fb` | 只读 | UTF-8 字符串 | `H1CNA00135000014` |
| 固件版本 | `43534956-4657-1000-8000-00805f9b34fb` | 只读 | UTF-8 字符串 | `V1.3.0.20260701` |
| MAC 地址 | `43534956-4d41-1000-8000-00805f9b34fb` | 只读 | UTF-8 字符串 | `08:92:72:BD:A6:B0` |
| WiFi SSID | `43534956-5353-1000-8000-00805f9b34fb` | 读写 | UTF-8 字符串 | `MyWiFi` |
| WiFi 密码 | `43534956-5057-1000-8000-00805f9b34fb` | 读写 | UTF-8 字符串 | `password123` |
| 配网状态 | `43534956-5354-1000-8000-00805f9b34fb` | 读 + 通知 | UTF-8 字符串 | `waiting` |

UUID 简写对照表（第 3 段标识）：

| 特征 | 第3段 | 记忆 |
|------|-------|------|
| SN | `534e` | "SN" |
| 固件版本 | `4657` | "FW" |
| MAC | `4d41` | "MA" |
| SSID | `5353` | "SS" |
| 密码 | `5057` | "PW" |
| 状态 | `5354` | "ST" |

---

## 4. 配网流程

### 4.1 完整时序图

```
    手机 App                          ESP32-C3 设备
       │                                    │
       │  ① BLE 扫描（过滤服务 UUID）         │
       │ ─────────────────────────────────→ │  广播中
       │  发现设备 "CS_INV_000014"           │
       │                                    │
       │  ② BLE 连接                        │
       │ ─────────────────────────────────→ │
       │  连接成功                            │
       │                                    │
       │  ③ 读取设备信息                      │
       │ ── read(SN) ─────────────────────→ │
       │ ←─ "H1CNA00135000014" ──────────── │
       │ ── read(FW) ─────────────────────→ │
       │ ←─ "V1.3.0.20260701" ──────────── │
       │ ── read(MAC) ────────────────────→ │
       │ ←─ "08:92:72:BD:A6:B0" ────────── │
       │                                    │
       │  ④ 订阅状态通知                      │
       │ ── setNotify(status) ────────────→ │
       │                                    │
       │  ⑤ 写入 WiFi SSID                  │
       │ ── write(SSID, "MyWiFi") ────────→ │  暂存 SSID
       │                                    │
       │  ⑥ 写入 WiFi 密码（触发配网）         │
       │ ── write(PASS, "password123") ───→ │  收到密码
       │                                    │  → 开始连接 WiFi
       │ ←─ notify("connecting") ───────── │
       │                                    │
       │  ⑦ 等待结果（最多约 15 秒）           │
       │                                    │
       │  ┌─ 成功 ──────────────────────────│─────────────┐
       │  │ ←─ notify("connected") ─────── │  WiFi 连接成功│
       │  │                                │  → 保存凭据  │
       │  └────────────────────────────────│─ → 连接 MQTT │
       │                                    │
       │  ┌─ 失败（密码错误等）──────────────│─────────────┐
       │  │ ←─ notify("failed") ────────── │  连接失败    │
       │  │                                │  → 回退 AP   │
       │  └────────────────────────────────│─ → BLE 继续  │
       │                                    │
       │  ┌─ 未找到 SSID ──────────────────│─────────────┐
       │  │ ←─ notify("not_found") ─────── │  SSID 不存在 │
       │  │                                │  → 回退 AP   │
       │  └────────────────────────────────│─ → BLE 继续  │
       │                                    │
       │  ⑧ App 根据状态提示用户              │
       │                                    │
```

**关键设计**：配网时 WiFi 仅尝试连接 1 次（约 15 秒），失败立即反馈，不重复尝试。
BLE 在整个配网过程中持续广播，连接失败后 App 可重新写入凭据重试，无需重新扫描。

### 4.2 操作说明

**步骤 ① — 扫描**

- 过滤条件：广播数据中包含服务 UUID `43534956-5052-1000-8000-00805f9b34fb`
- 显示内容：设备广播名（`CS_INV_完整SN`）
- 建议同时显示信号强度（RSSI），方便用户判断距离

**步骤 ② — 连接**

- 连接超时建议：10 秒
- 连接失败时提示用户靠近设备重试

**步骤 ③ — 读取设备信息**

- 读取 SN 有两种方式（推荐方式一）：
  - **方式一**：直接读 GAP Device Name（UUID `0x2A00`），无需发现自定义服务
  - **方式二**：发现自定义服务后读 SN 特征（UUID `43534956-534e-...`）
- 建议读取 SN 并显示给用户确认，避免配错设备

**步骤 ④ — 订阅通知**

- 对状态特征（`...5354-...`）启用 Notify
- 必须在写入凭据前完成订阅
- 设备会推送状态变化：`connecting` / `connected` / `failed` / `not_found`

**步骤 ⑤ ⑥ — 写入凭据**

- **先写 SSID，再写密码**（顺序无关，但建议保持一致）
- 数据格式：UTF-8 字符串，不含 null 终止符
- 开放网络（无密码）：密码特征写入空字符串或 `""`
- 密码写入后设备立即开始连接 WiFi

**步骤 ⑦ — 等待结果**

- 监听状态通知，典型等待时间 3-15 秒
- 收到 `connected` → 提示成功，可安全断开 BLE
- 收到 `failed` → 提示"连接失败，请检查密码"，App 可重新写入凭据重试
- 收到 `not_found` → 提示"未找到该 WiFi，请检查名称"
- BLE 持续广播，失败后无需重新扫描，直接重试

---

## 5. 状态值定义

状态通过 `CHAR_PROV_STATUS_UUID`（`43534956-5354-1000-8000-00805f9b34fb`）特征的 **Notify** 推送给 App。

| 状态值 | 含义 | 触发时机 | App 应对 |
|--------|------|---------|---------|
| `waiting` | 初始状态 | BLE 配网服务启动时 | 正常流程 |
| `connecting` | 正在连接 WiFi | 密码写入后，开始连接 WiFi | 显示加载动画，等待结果 |
| `connected` | WiFi 连接成功 | WiFi 成功获取 IP 地址 | 提示成功，可断开 BLE |
| `not_found` | SSID 不存在 | WiFi 扫描找不到目标 SSID | 提示"未找到该 WiFi，请检查名称" |
| `failed` | 连接失败 | 密码错误或连接超时 | 提示"连接失败，请检查密码" |

**状态流转**：

```
waiting ──→ connecting ──→ connected    （成功）
                     ├──→ not_found     （SSID 不存在）
                     └──→ failed        （密码错误/超时）
```

**配网重试策略**：配网场景下 WiFi 仅尝试 1 次（约 15 秒），失败后 BLE 继续广播，App 可直接重新写入凭据再次尝试，无需重新扫描连接。

---

## 6. 错误处理

| 场景 | 设备行为 | BLE 状态反馈 | App 建议 |
|------|---------|-------------|---------|
| WiFi 密码错误 | 连接失败（约 15 秒） | `notify("failed")` | 提示"密码错误，请重试"，允许重新写入凭据 |
| WiFi SSID 不存在 | 连接失败（约 15 秒） | `notify("not_found")` | 提示"未找到该 WiFi，请检查名称" |
| WiFi 信号弱/超时 | 连接失败（约 15 秒） | `notify("failed")` | 提示用户靠近路由器重试 |
| BLE 连接中断 | 设备自动恢复广播 | 无（BLE 已断开） | App 自动重连或提示用户 |
| 多台设备同时配网 | 各自独立广播 | 各自独立 | App 列表展示，用户选择 |

**配网失败后**：BLE 继续广播，App 无需重新扫描，可直接重新写入 SSID 和密码再次尝试。

---

## 7. App 端实现参考

### 7.1 Flutter（flutter_blue_plus）

```dart
// 1. 扫描
flutter_blue_plus.startScan(
  withServices: [Guid("43534956-5052-1000-8000-00805f9b34fb")],
  timeout: Duration(seconds: 10),
);

// 2. 连接
await device.connect();

// 3. 发现服务
List<BluetoothService> services = await device.discoverServices();

// 4. 读取 SN（方式一：直接读 GAP Device Name）
final deviceNameChar = services
    .firstWhere((s) => s.uuid == Guid("00001800-0000-1000-8000-00805f9b34fb"))
    .characteristics
    .firstWhere((c) => c.uuid == Guid("00002a00-0000-1000-8000-00805f9b34fb"));
String sn = String.fromCharCodes(await deviceNameChar.read());

// 4b. 读取 SN（方式二：自定义特征）
// final snChar = services.first.characteristics.firstWhere(
//   (c) => c.uuid == Guid("43534956-534e-1000-8000-00805f9b34fb"));
// String sn = String.fromCharCodes(await snChar.read());

// 5. 订阅状态通知
final statusChar = services.first.characteristics.firstWhere(
  (c) => c.uuid == Guid("43534956-5354-1000-8000-00805f9b34fb"));
statusChar.lastValueStream.listen((value) {
  String status = String.fromCharCodes(value);
  switch (status) {
    case "connecting":  // 正在连接 WiFi
      break;
    case "connected":   // WiFi 连接成功，可断开 BLE
      break;
    case "failed":      // 连接失败（密码错误等），可重新写入凭据重试
      break;
    case "not_found":   // SSID 不存在，提示用户检查 WiFi 名称
      break;
  }
});

await statusChar.setNotifyValue(true);

// 6. 写入 SSID
final ssidChar = services.first.characteristics.firstWhere(
  (c) => c.uuid == Guid("43534956-5353-1000-8000-00805f9b34fb"));
await ssidChar.write("MyWiFi".codeUnits);

// 7. 写入密码（触发配网）
final passChar = services.first.characteristics.firstWhere(
  (c) => c.uuid == Guid("43534956-5057-1000-8000-00805f9b34fb"));
await passChar.write("password123".codeUnits);

// 8. 等待 connected 通知...
```

### 7.2 Android（Kotlin）

```kotlin
// 扫描过滤
val filter = ScanFilter.Builder()
    .setServiceUuid(ParcelUuid.fromString("43534956-5052-1000-8000-00805f9b34fb"))
    .build()

// 特征 UUID
val UUID_GAP_NAME = UUID.fromString("00002a00-0000-1000-8000-00805f9b34fb") // GAP Device Name = SN
val UUID_SN     = UUID.fromString("43534956-534e-1000-8000-00805f9b34fb")
val UUID_FW     = UUID.fromString("43534956-4657-1000-8000-00805f9b34fb")
val UUID_MAC    = UUID.fromString("43534956-4d41-1000-8000-00805f9b34fb")
val UUID_SSID   = UUID.fromString("43534956-5353-1000-8000-00805f9b34fb")
val UUID_PASS   = UUID.fromString("43534956-5057-1000-8000-00805f9b34fb")
val UUID_STATUS = UUID.fromString("43534956-5354-1000-8000-00805f9b34fb")
```

### 7.3 iOS（Swift / CoreBluetooth）

```swift
let serviceUUID  = CBUUID(string: "43534956-5052-1000-8000-00805f9b34fb")
let gapNameUUID  = CBUUID(string: "2A00")  // GAP Device Name = SN
let ssidUUID     = CBUUID(string: "43534956-5353-1000-8000-00805f9b34fb")
let passUUID     = CBUUID(string: "43534956-5057-1000-8000-00805f9b34fb")
let statusUUID   = CBUUID(string: "43534956-5354-1000-8000-00805f9b34fb")

// 扫描
centralManager.scanForPeripherals(withServices: [serviceUUID])
```

---

## 8. 配网触发条件

设备收到密码写入时，检查以下条件：

1. SSID 特征已有值（非空）
2. 密码特征已写入（空字符串表示开放网络）

满足条件后立即触发 WiFi 连接。

**建议 App 操作顺序**：先写 SSID → 再写密码。

---

## 9. 配网后设备行为

| 阶段 | 设备行为 |
|------|---------|
| 密码写入后 | 立即通过 BLE 通知 `connecting`，开始连接 WiFi（仅尝试 1 次） |
| WiFi 连接成功 | 通知 `connected`，保存凭据到 NVS，连接 MQTT 服务器 |
| WiFi 连接失败 | 通知 `failed` 或 `not_found`，回退 AP 模式，BLE 继续广播 |
| 配网失败后重试 | App 可直接重新写入凭据，无需重新扫描 BLE |
| 下次开机 | 从 NVS 读取凭据直接连 WiFi，正常断线重连 10 次，不进入配网 |

---

## 10. 多设备场景

当现场有多台 CS INV 设备时：

- 每台设备独立广播，设备名含各自 SN 后 6 位
- App 应列出所有发现的设备，显示 SN、MAC、RSSI
- 用户选择目标设备后再进行配网
- 同一时间只能对一台设备配网（BLE 连接独占）

---

## 附录 A：UUID 完整列表

```
GAP Device Name: 0x2A00              （完整 SN，标准 GAP 特征）
服务 UUID:       43534956-5052-1000-8000-00805f9b34fb
设备 SN:         43534956-534e-1000-8000-00805f9b34fb
固件版本:        43534956-4657-1000-8000-00805f9b34fb
MAC 地址:        43534956-4d41-1000-8000-00805f9b34fb
WiFi SSID:       43534956-5353-1000-8000-00805f9b34fb
WiFi 密码:       43534956-5057-1000-8000-00805f9b34fb
配网状态:        43534956-5354-1000-8000-00805f9b34fb
```

## 附录 B：BLE 配网日志示例

### B.1 配网成功

设备端串口输出：

```
[BLE-PROV] Starting BLE provisioning...
[BLE-PROV] Service created, device: CS_INV_H1CNA00135000014
[BLE-PROV] SN char set: 'H1CNA00135000014' (17 bytes)
[BLE-PROV]   SN:  H1CNA00135000014
[BLE-PROV]   FW:  V1.3.0.20260701
[BLE-PROV]   MAC: 08:92:72:BD:A6:B0
[BLE-PROV] Advertising as 'CS_INV_H1CNA00135000014'
[BLE-PROV] Client connected
[BLE-PROV] SSID received: MyWiFi
[BLE-PROV] Password received (11 bytes)
[BLE-PROV] Credentials ready, connecting to: MyWiFi
[PROV] Got: MyWiFi, pass_len=11
[WIFI] Connecting to MyWiFi...
[WIFI] Connected! STA_IP=192.168.1.100
```

BLE 状态通知：`waiting` → `connecting` → `connected`

### B.2 配网失败（SSID 不存在）

```
[BLE-PROV] SSID received: WrongSSID
[BLE-PROV] Password received (11 bytes)
[BLE-PROV] Credentials ready, connecting to: WrongSSID
[PROV] Got: WrongSSID, pass_len=11
[WIFI] Connecting to WrongSSID...
[WIFI] Max retries (1) -> AP mode
```

BLE 状态通知：`waiting` → `connecting` → `not_found`

### B.3 配网失败（密码错误）

```
[BLE-PROV] SSID received: MyWiFi
[BLE-PROV] Password received (8 bytes)
[BLE-PROV] Credentials ready, connecting to: MyWiFi
[PROV] Got: MyWiFi, pass_len=8
[WIFI] Connecting to MyWiFi...
[WIFI] Max retries (1) -> AP mode
```

BLE 状态通知：`waiting` → `connecting` → `failed`
