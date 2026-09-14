# BLE 本地模式实现方案 (V3 定稿版)

> **For agentic workers:** REQUIRED SUB-SKILL: Use compose:subagent (recommended) or compose:execute to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 为 CS-L10-6K2 采集器实现 BLE 本地模式，严格对齐《BLE_Local_Communication_Protocol.md V1.0》+ 设计文档 2 处协议修订，支持 App 通过蓝牙进行设备绑定（含 PIN 校验）、鉴权、遥测推送、命令控制。

**Architecture:** 新增 `ble_ct` 组件实现 CSIV-CT 服务，复用现有 `cmd_handler` 命令映射，device_key（App 本地生成）+ HMAC-SHA256 应用层鉴权 + PIN（PRODUCT_SECRET + SN 派生）防抢绑。

**Tech Stack:** NimBLE (ESP-IDF), FreeRTOS, cJSON, mbedtls (HMAC-SHA256 + Base64)

## 全局约束

- **UUID 严格对齐协议文档**，不得自创
- CSIV-PR 配网服务保持不变（不加加密标志，避免破坏现有配网流程）
- **CSIV-CT 特征不设 ENC 标志**（绑定时需未配对状态读 INFO / 写 AUTH，ENC 会死锁）
- SM_LVL=1（加密）+ 应用层 device_key 鉴权（安全模型以应用层为准）
- MTU 512，DLE 开启
- 未鉴权拒绝 COMMAND 写入（返回 `Insufficient Authentication`），不推送 TELEMETRY
- 命令幂等：相同 command_id 返回首次结果（10 分钟过期）
- BLE OTA 不实现，OTA 继续走 WiFi HTTP
- **单连接约束**：第二个 BLE 连接应被拒绝
- **时间校准**：鉴权成功时用 msg.ts 校准设备时钟，NVS 持久化
- **PIN 校验**：配网（CSIV-PR）和绑定（AUTH bind）双入口共用锁定计数（按 MAC，5 次锁 30 分钟）
- **device_key 由 App 本地生成**（32B 随机），后端仅登记 SHA-256 摘要

## 协议文档参考

- 位置：`d:\CS_APP_PROJECT\cs_inv_monitor-develop\cs_inv_monitor-develop\docs\BLE_Local_Communication_Protocol.md`
- 版本：V1.0（2026-08-06）

---

## 1. GATT 服务定义（对齐协议 §2 + 设计文档 §5.1 修订①）

### 1.1 服务 UUID

```
43534956-4354-1000-8000-00805f9b34fb  (CSIV-CT)
```

### 1.2 特征列表

| 名称 | UUID | 权限 | 说明 |
|------|------|------|------|
| AUTH | `43534956-4155-1000-8000-00805f9b34fb` | Write + Notify | challenge-response 鉴权 + 绑定 + PIN 校验 |
| TELEMETRY | `43534956-544c-1000-8000-00805f9b34fb` | **Read + Notify**（修订①） | 遥测推送 |
| COMMAND | `43534956-434d-1000-8000-00805f9b34fb` | Write | 控制命令 |
| CMD_RESULT | `43534956-4352-1000-8000-00805f9b34fb` | Notify | 命令回报 |
| INFO | `43534956-494e-1000-8000-00805f9b34fb` | Read | 设备信息快照 |

### 1.3 命令映射表（协议 §7.3 action → cmd_handler 命令名）

| 协议 action | cmd_handler cmd_name | params | scale |
|-------------|---------------------|--------|-------|
| `power_on` | `output_priority` | `{value: 0}` | 1 |
| `power_off` | `output_priority` | `{value: 1}` | 1 |
| `set_power` | `set_charge_limit` | `{power_w}` | 10 (0.1A) |
| `set_param` | 动态（params.param_id） | `{param_id, value}` | 按 cmd_map |
| `get_param` | `query_config` | `{param_id}` | — |

### 1.4 遥测核心子集（BLE 通道裁剪）

```json
{
  "device_sn": "H1CNA00135000014",
  "ac": { "voltage": 230.1, "current": 2.4, "power": 552, "frequency": 50.02, "load_percent": 9.2 },
  "battery": { "soc": 86.5, "voltage": 52.3, "current": -3.2, "power": -167, "charge_state": "discharging" },
  "pv": { "voltage": 182.0, "current": 6.8, "power": 1238 },
  "sys_status": { "work_mode": "offgrid", "fault_code": 0, "temperature": 38.5 },
  "energy": { "today_generation": 12.4, "today_consumption": 8.1 },
  "load_power": 552,
  "updated_at": "2026-08-06T10:20:30Z"
}
```

> 约 300~400 字节，MTU 512 单帧可承载。

---

## 2. 文件结构

### 2.1 新增文件

| 文件 | 职责 |
|------|------|
| `components/ble_ct/ble_ct.h` | CSIV-CT 服务 API |
| `components/ble_ct/ble_ct.c` | CSIV-CT GATT 服务实现 + GAP 事件处理 |
| `components/ble_ct/ble_ct_auth.h` | 鉴权模块 API |
| `components/ble_ct/ble_ct_auth.c` | device_key + HMAC-SHA256 鉴权 + 时钟校准 |
| `components/ble_ct/ble_ct_pin.h` | PIN 校验 API |
| `components/ble_ct/ble_ct_pin.c` | PIN 计算 + 锁定计数 |
| `components/ble_ct/ble_ct_telemetry.h` | 遥测推送 API |
| `components/ble_ct/ble_ct_telemetry.c` | 遥测分帧推送 |
| `components/ble_ct/ble_ct_cmd.h` | 命令通道 API |
| `components/ble_ct/ble_ct_cmd.c` | 命令接收与回报 + 幂等缓存 |
| `components/ble_ct/CMakeLists.txt` | 组件构建配置 |
| `components/ble_ct/Kconfig` | 配置选项 |

### 2.2 修改文件

| 文件 | 修改内容 |
|------|---------|
| `components/ble_prov/ble_prov.c` | 添加 MTU 协商、事件转发钩子、PIN 校验前置 |
| `components/ble_prov/ble_prov.h` | 导出事件回调注册 API |
| `main/app/app_main.c` | 初始化 ble_ct |
| `main/telemetry/telemetry.c` | 添加 BLE 遥测推送钩子 |
| `main/telemetry/cmd_handler.c` | 添加 BLE 命令入口 |
| `sdkconfig.defaults` | 启用 LESC、NVS 持久化 |

---

## 3. 实施任务（14 Task，4 组）

### 依赖关系

```
Task 1 (无 ENC) → Task 2 (事件转发) → Task 3 (时钟校准) → Task 4 (PIN 校验)
                                                                    ↓
                                              Task 5-12 (P1/P2，可并行)
                                                                    ↓
                                              Task 13 (协议文档) → Task 14 (联调)
```

---

### 【P0 基础】Task 1: 无 ENC 标志确认

**修订:** ② 特征不加链路层 ENC 标志
**为什么先做:** PIN/绑定都依赖能写 AUTH，ENC 会导致死锁

**Files:**
- Verify: `components/ble_ct/ble_ct.c`（GATT 属性表）

- [ ] **Step 1: 确认 GATT 属性表无 ENC 标志**

在 `ble_ct.c` 的 `s_ct_gatt_svcs[]` 中确认所有特征权限不含 `BLE_GATT_CHR_F_READ_ENC` / `BLE_GATT_CHR_F_WRITE_ENC`：

```c
// AUTH: Write + Notify（无 ENC）
.flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY,
// TELEMETRY: Read + Notify（无 ENC）
.flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
// COMMAND: Write（无 ENC）
.flags = BLE_GATT_CHR_F_WRITE,
// CMD_RESULT: Notify（无 ENC）
.flags = BLE_GATT_CHR_F_NOTIFY,
// INFO: Read（无 ENC）
.flags = BLE_GATT_CHR_F_READ,
```

- [ ] **Step 2: Commit**

```bash
git add components/ble_ct/ble_ct.c
git commit -m "feat(ble): confirm no ENC flags on CSIV-CT characteristics"
```

---

### 【P0 基础】Task 2: 事件转发（连接/断开/订阅/MTU）

**修订:** ③ 连接/断开/订阅事件转发
**依赖:** Task 1
**为什么:** ble_ct 状态机依赖这些事件，否则是死的

**Files:**
- Modify: `components/ble_prov/ble_prov.c`
- Modify: `components/ble_prov/ble_prov.h`
- Modify: `components/ble_ct/ble_ct.c`

**Interfaces:**
- Produces: `ble_ct_on_gap_event()` API

- [ ] **Step 1: 在 ble_prov.h 添加事件转发回调注册**

```c
typedef int (*BleGapEventForwardCb)(struct ble_gap_event *event, void *arg);
void ble_prov_set_gap_event_forward(BleGapEventForwardCb cb);
```

- [ ] **Step 2: 在 ble_prov.c 的 gap_event_cb 中转发事件**

```c
static BleGapEventForwardCb s_gap_forward_cb = NULL;

void ble_prov_set_gap_event_forward(BleGapEventForwardCb cb) {
    s_gap_forward_cb = cb;
}

static int gap_event_cb(struct ble_gap_event *event, void *arg) {
    // 转发给 ble_ct
    if (s_gap_forward_cb) {
        s_gap_forward_cb(event, arg);
    }
    
    // 原有逻辑保持不变
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        // ...
    case BLE_GAP_EVENT_DISCONNECT:
        // ...
    }
    return 0;
}
```

- [ ] **Step 3: 在 ble_ct.c 实现事件处理**

```c
int ble_ct_on_gap_event(struct ble_gap_event *event, void *arg) {
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            // 单连接守卫
            if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
                ble_gap_terminate(event->connect.conn_handle,
                                  BLE_ERR_REM_USER_CONN_TERM);
                return 0;
            }
            s_conn_handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "CT connected (conn=%d)", s_conn_handle);
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        if (event->disconnect.conn.conn_handle == s_conn_handle) {
            s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            s_telemetry_subscribed = false;
            s_result_subscribed = false;
            s_auth_subscribed = false;
            ble_ct_auth_reset_session();  // 断开即失效
            // 注意：锁定计数不清零，防断开重连绕过锁定
        }
        break;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_telemetry_handle) {
            s_telemetry_subscribed = event->subscribe.cur_notify;
        } else if (event->subscribe.attr_handle == s_result_handle) {
            s_result_subscribed = event->subscribe.cur_notify;
        } else if (event->subscribe.attr_handle == s_auth_handle) {
            s_auth_subscribed = event->subscribe.cur_notify;
        }
        break;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU updated: %d", event->mtu.value);
        break;
    }
    return 0;
}
```

- [ ] **Step 4: 在 app_main.c 注册转发**

```c
ble_prov_set_gap_event_forward(ble_ct_on_gap_event);
```

- [ ] **Step 5: 编译验证**

```bash
idf.py build
```

- [ ] **Step 6: Commit**

```bash
git add components/ble_prov/ components/ble_ct/ main/app/app_main.c
git commit -m "feat(ble): add GAP event forwarding from ble_prov to ble_ct"
```

---

### 【P0 基础】Task 3: 时钟校准

**修订:** ① 时钟校准机制
**依赖:** Task 2
**为什么:** 鉴权 ts 校验、命令防重放、绑定窗口过期的公共底座

**Files:**
- Create: `components/ble_ct/ble_ct_clock.h`
- Create: `components/ble_ct/ble_ct_clock.c`

**Interfaces:**
- Produces: `ble_ct_now()` / `ble_ct_calibrate()` API

- [ ] **Step 1: 创建 ble_ct_clock.h**

```c
#ifndef BLE_CT_CLOCK_H
#define BLE_CT_CLOCK_H

#include <stdint.h>

// 获取设备虚拟时钟（Unix 秒）
uint32_t ble_ct_now(void);

// 校准时钟（鉴权/绑定成功后调用）
void ble_ct_calibrate(uint32_t unix_ts);

// 从 NVS 恢复时钟基准（启动时调用）
void ble_ct_clock_init(void);

#endif
```

- [ ] **Step 2: 创建 ble_ct_clock.c**

```c
#include "ble_ct_clock.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "ble_ct_clock";

static uint32_t s_time_base = 0;      // Unix 秒
static uint32_t s_base_uptime_ms = 0; // 校准时 uptime

static uint32_t uptime_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

uint32_t ble_ct_now(void) {
    if (s_time_base == 0) return 0;  // 未校时
    return s_time_base + (uptime_ms() - s_base_uptime_ms) / 1000;
}

void ble_ct_calibrate(uint32_t unix_ts) {
    s_time_base = unix_ts;
    s_base_uptime_ms = uptime_ms();
    ESP_LOGI(TAG, "Time calibrated: %u (uptime=%u)", unix_ts, s_base_uptime_ms);
    
    // NVS 持久化
    nvs_handle_t handle;
    if (nvs_open("ble_ct", NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_u32(handle, "time_base", unix_ts);
        nvs_set_u32(handle, "base_uptime", s_base_uptime_ms);
        nvs_commit(handle);
        nvs_close(handle);
    }
}

void ble_ct_clock_init(void) {
    nvs_handle_t handle;
    if (nvs_open("ble_ct", NVS_READONLY, &handle) == ESP_OK) {
        uint32_t tb = 0, bu = 0;
        if (nvs_get_u32(handle, "time_base", &tb) == ESP_OK &&
            nvs_get_u32(handle, "base_uptime", &bu) == ESP_OK &&
            tb > 0) {
            s_time_base = tb;
            s_base_uptime_ms = bu;
            ESP_LOGI(TAG, "Clock restored: base=%u, now=%u", tb, ble_ct_now());
        }
        nvs_close(handle);
    }
}
```

- [ ] **Step 3: 在 CMakeLists.txt 添加源文件**

- [ ] **Step 4: 编译验证**

```bash
idf.py build
```

- [ ] **Step 5: Commit**

```bash
git add components/ble_ct/ble_ct_clock.h components/ble_ct/ble_ct_clock.c components/ble_ct/CMakeLists.txt
git commit -m "feat(ble): add virtual clock with NVS persistence"
```

---

### 【P0 基础】Task 4: PIN 校验（最重要的新增）

**修订:** ⑫ PIN 防抢绑
**依赖:** Task 3（时钟做锁定计时）
**为什么:** 防抢绑的物理屏障

**Files:**
- Create: `components/ble_ct/ble_ct_pin.h`
- Create: `components/ble_ct/ble_ct_pin.c`

**Interfaces:**
- Consumes: `ble_ct_now()`, `mbedtls_md_hmac()`
- Produces: `ble_ct_pin_verify()` API

- [ ] **Step 0: 测试向量先行（最关键）**

先用 PC 脚本用同一算法生成 3 组 (SN → PIN) 测试向量：

```python
import hmac, hashlib

PRODUCT_SECRET = b"CS_INV_L10_2026_SECRET"  # 与固件一致
test_sns = ["H1CNA00135000014", "H1CNA00135000015", "H1CNA00135000016"]

for sn in test_sns:
    digest = hmac.new(PRODUCT_SECRET, sn.encode(), hashlib.sha256).digest()
    pin = ((digest[0] << 16) | (digest[1] << 8) | digest[2]) % 1000000
    print(f"SN={sn} → PIN={pin:06d}")
```

输出作为固件测试用例。

- [ ] **Step 1: 创建 ble_ct_pin.h**

```c
#ifndef BLE_CT_PIN_H
#define BLE_CT_PIN_H

#include <stdbool.h>
#include <stdint.h>

// 校验 PIN（配网和绑定共用）
// peer_addr: 对端 MAC 地址（6 字节），用于锁定计数
// 返回: true=正确, false=错误（含锁定中）
bool ble_ct_pin_verify(const char *sn, const char *pin, const uint8_t *peer_addr);

// 获取锁定剩余秒数（0=未锁定）
uint32_t ble_ct_pin_lockout_remaining(const uint8_t *peer_addr);

// 计算 PIN（供工厂工具调用或测试）
uint32_t ble_ct_compute_pin(const char *sn);

#endif
```

- [ ] **Step 2: 创建 ble_ct_pin.c**

```c
#include "ble_ct_pin.h"
#include "ble_ct_clock.h"
#include "esp_log.h"
#include "mbedtls/md.h"
#include <string.h>

static const char *TAG = "ble_ct_pin";

// 产品线密钥（分片拼接，提高反编译提取门槛）
static const char S1[] = "CS_INV_L10";
static const char S2[] = "_2026_";
static const char S3[] = "SECRET";

#define MAX_FAIL_COUNT 5
#define LOCKOUT_DURATION_S (30 * 60)  // 30 分钟

// 按 MAC 的锁定表
typedef struct {
    uint8_t addr[6];
    int fail_count;
    uint32_t lockout_until;  // Unix 秒
} PinLockout;

#define MAX_LOCKOUT_ENTRIES 4
static PinLockout s_lockouts[MAX_LOCKOUT_ENTRIES];

// 拼接密钥
static void get_product_secret(uint8_t *buf, size_t *len) {
    size_t n = 0;
    memcpy(buf + n, S1, strlen(S1)); n += strlen(S1);
    memcpy(buf + n, S2, strlen(S2)); n += strlen(S2);
    memcpy(buf + n, S3, strlen(S3)); n += strlen(S3);
    *len = n;
}

uint32_t ble_ct_compute_pin(const char *sn) {
    uint8_t secret[64];
    size_t secret_len;
    get_product_secret(secret, &secret_len);
    
    uint8_t digest[32];
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    mbedtls_md_hmac(md, secret, secret_len, (const uint8_t *)sn, strlen(sn), digest);
    
    return ((digest[0] << 16) | (digest[1] << 8) | digest[2]) % 1000000;
}

static PinLockout* find_lockout(const uint8_t *addr) {
    int empty = -1;
    for (int i = 0; i < MAX_LOCKOUT_ENTRIES; i++) {
        if (memcmp(s_lockouts[i].addr, addr, 6) == 0) return &s_lockouts[i];
        if (empty < 0 && s_lockouts[i].fail_count == 0) empty = i;
    }
    if (empty >= 0) {
        memset(&s_lockouts[empty], 0, sizeof(PinLockout));
        memcpy(s_lockouts[empty].addr, addr, 6);
        return &s_lockouts[empty];
    }
    return NULL;
}

bool ble_ct_pin_verify(const char *sn, const char *pin, const uint8_t *peer_addr) {
    // 检查锁定
    PinLockout *entry = find_lockout(peer_addr);
    if (entry && entry->fail_count >= MAX_FAIL_COUNT) {
        uint32_t now = ble_ct_now();
        if (now < entry->lockout_until) {
            ESP_LOGW(TAG, "PIN locked out for %us", entry->lockout_until - now);
            return false;
        }
        entry->fail_count = 0;  // 锁定过期
    }
    
    // 计算正确 PIN
    uint32_t expected = ble_ct_compute_pin(sn);
    char expected_str[8];
    snprintf(expected_str, sizeof(expected_str), "%06u", expected);
    
    // 比对
    if (strcmp(pin, expected_str) == 0) {
        if (entry) entry->fail_count = 0;
        ESP_LOGI(TAG, "PIN verified OK");
        return true;
    }
    
    // 失败
    if (entry) {
        entry->fail_count++;
        if (entry->fail_count >= MAX_FAIL_COUNT) {
            entry->lockout_until = ble_ct_now() + LOCKOUT_DURATION_S;
            ESP_LOGW(TAG, "PIN lockout: %d failures, locked for 30min", entry->fail_count);
        }
    }
    ESP_LOGW(TAG, "PIN mismatch: expected %s, got %s", expected_str, pin);
    return false;
}

uint32_t ble_ct_pin_lockout_remaining(const uint8_t *peer_addr) {
    PinLockout *entry = find_lockout(peer_addr);
    if (!entry || entry->fail_count < MAX_FAIL_COUNT) return 0;
    uint32_t now = ble_ct_now();
    if (now >= entry->lockout_until) {
        entry->fail_count = 0;
        return 0;
    }
    return entry->lockout_until - now;
}
```

- [ ] **Step 3: 测试向量验证**

在 `ble_ct.c` 的 `ble_ct_start()` 中临时添加：

```c
// 测试向量验证（开发阶段，完成后删除）
ESP_LOGI(TAG, "PIN test: SN=H1CNA00135000014 → %06u", ble_ct_compute_pin("H1CNA00135000014"));
ESP_LOGI(TAG, "PIN test: SN=H1CNA00135000015 → %06u", ble_ct_compute_pin("H1CNA00135000015"));
```

与 PC 脚本输出比对，确认一致后删除测试代码。

- [ ] **Step 4: 编译验证**

```bash
idf.py build
```

- [ ] **Step 5: Commit**

```bash
git add components/ble_ct/ble_ct_pin.h components/ble_ct/ble_ct_pin.c
git commit -m "feat(ble): add PIN verification with HMAC-SHA256 and lockout"
```

---

### 【P1 可靠性】Task 5: 跨文件句柄导出

**修订:** ④
**Files:** Modify `ble_ct.c`, `ble_ct.h`

- [ ] **Step 1: 在 ble_ct.h 添加导出函数**

```c
uint16_t ble_ct_get_telemetry_handle(void);
uint16_t ble_ct_get_result_handle(void);
bool ble_ct_is_telemetry_subscribed(void);
bool ble_ct_is_result_subscribed(void);
```

- [ ] **Step 2: 在 ble_ct.c 实现**

```c
uint16_t ble_ct_get_telemetry_handle(void) { return s_telemetry_handle; }
uint16_t ble_ct_get_result_handle(void) { return s_result_handle; }
bool ble_ct_is_telemetry_subscribed(void) { return s_telemetry_subscribed; }
bool ble_ct_is_result_subscribed(void) { return s_result_subscribed; }
```

- [ ] **Step 3: 修改 ble_ct_telemetry.c / ble_ct_cmd.c 使用导出函数**

- [ ] **Step 4: Commit**

```bash
git commit -m "fix(ble): export GATT handles for cross-file access"
```

---

### 【P1 可靠性】Task 6: 幂等缓存归位

**修订:** ⑤
**Files:** Modify `ble_ct_cmd.c`

- [ ] **Step 1: 将幂等缓存从 auth 模块移到 cmd 模块**

```c
// ble_ct_cmd.c 内部
typedef struct {
    char command_id[16];
    char result_json[256];
    uint32_t timestamp_ms;
    bool valid;
} CmdIdempotentEntry;

#define CACHE_SIZE 8
#define CACHE_EXPIRE_MS (10 * 60 * 1000)  // 10 分钟

static CmdIdempotentEntry s_cache[CACHE_SIZE];
static int s_cache_idx = 0;
```

- [ ] **Step 2: Commit**

```bash
git commit -m "fix(ble): move idempotent cache to cmd module with 10min TTL"
```

---

### 【P1 可靠性】Task 7: 统一用 mbedtls

**修订:** ⑥
**Files:** Modify `ble_ct_auth.c`, `ble_ct_pin.c`

- [ ] **Step 1: 替换所有 HMAC 调用**

```c
// 旧: monocypher_crypto_hmac(...)
// 新:
#include "mbedtls/md.h"
const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
mbedtls_md_hmac(md, key, key_len, msg, msg_len, output);
```

- [ ] **Step 2: 替换所有 Base64 调用**

```c
#include "mbedtls/base64.h"
mbedtls_base64_encode(out, out_len, &olen, in, in_len);
mbedtls_base64_decode(out, out_len, &olen, in, in_len);
```

- [ ] **Step 3: CMakeLists.txt 依赖改为 mbedtls**

- [ ] **Step 4: Commit**

```bash
git commit -m "refactor(ble): replace monocypher with mbedtls for HMAC and Base64"
```

---

### 【P1 可靠性】Task 8: 多帧上限处理

**修订:** ⑦
**Files:** Modify `ble_ct_telemetry.c`

- [ ] **Step 1: 超 8 帧拒绝推送并告警**

```c
if (total_frames > MAX_FRAMES) {
    ESP_LOGW(TAG, "Telemetry too large (%u bytes, %u frames > %d), skipped",
             len, total_frames, MAX_FRAMES);
    return;
}
```

- [ ] **Step 2: Commit**

```bash
git commit -m "fix(ble): reject telemetry exceeding 8 frames instead of silent truncation"
```

---

### 【P1 可靠性】Task 9: 锁定按 MAC

**修订:** ⑧
**Files:** Modify `ble_ct_auth.c`

- [ ] **Step 1: 鉴权失败计数从全局改为按 MAC**

结构与 `ble_ct_pin.c` 的 `PinLockout` 相同，复用或提取公共模块。

- [ ] **Step 2: Commit**

```bash
git commit -m "fix(ble): per-MAC lockout for auth failures"
```

---

### 【P2 对齐】Task 10: action 映射核对

**修订:** ⑨
**Files:** Modify `ble_ct_cmd.c`

- [ ] **Step 1: 核对 s_action_map 与 cmd_handler.c 的 s_cmd_map**

确认映射正确，无臆造：

| action | cmd_name | 来源 |
|--------|----------|------|
| `power_on` | `output_priority` | 语义化别名 |
| `power_off` | `output_priority` | 语义化别名 |
| `set_power` | `set_charge_limit` | 语义化别名 |
| `set_param` | 动态 | params.param_id |
| `get_param` | `query_config` | 查询命令 |
| 其余 46 个 | 直接使用 | cmd_handler 原名 |

- [ ] **Step 2: Commit**

```bash
git commit -m "fix(ble): verify action-to-cmd mapping against cmd_handler"
```

---

### 【P2 对齐】Task 11: 遥测裁剪核心子集

**修订:** ⑩
**Files:** Modify `telemetry.c`

- [ ] **Step 1: BLE 通道使用精简版 heartbeat JSON**

只包含 6 组核心字段（约 300~400 字节），与 §1.4 对齐。

- [ ] **Step 2: Commit**

```bash
git commit -m "fix(ble): trim telemetry to core subset for BLE channel"
```

---

### 【P2 对齐】Task 12: 绑定窗口守卫 + PIN 接入 AUTH

**修订:** ⑪ 绑定窗口守卫 + ⑫ PIN 接入 AUTH
**Files:** Modify `ble_ct_auth.c`, Modify `ble_prov.c`

- [ ] **Step 1: 已绑定设备上 open_bind_window() 无效**

```c
void ble_ct_auth_open_bind_window(void) {
    if (s_is_bound) {
        ESP_LOGI(TAG, "Bind window not opened: already bound");
        return;
    }
    s_bind_window_close_ms = now_ms() + BIND_WINDOW_DURATION_MS;
}
```

- [ ] **Step 2: AUTH 消息处理增加 pin_check 和 bind 带 PIN**

```c
// mode:"pin_check" — 配网写凭据前校验
// mode:"bind" + pin — 绑定时校验 PIN

if (strcmp(mode, "pin_check") == 0) {
    // 校验 PIN，返回结果
    bool ok = ble_ct_pin_verify(sn, pin, peer_addr);
    // notify {mode:"pin_check", result: ok ? "ok" : "invalid_pin"}
}

if (strcmp(mode, "bind") == 0) {
    // 校验 PIN
    if (!ble_ct_pin_verify(sn, pin, peer_addr)) {
        // notify {mode:"bind", result:"rejected", reason:"invalid_pin"}
        return;
    }
    // 校验绑定窗口
    if (s_is_bound && !ble_ct_auth_is_bind_window_open()) {
        // notify {mode:"bind", result:"rejected", reason:"already_bound"}
        return;
    }
    // 持久化 device_key
    // ...
}
```

- [ ] **Step 3: ble_prov.c 的配网写入前增加 PIN 校验**

在 `gatt_access_cb` 中处理 SSID/PASS 写入前：

```c
if (attr_handle == s_ssid_val_handle || attr_handle == s_pass_val_handle) {
    // 检查是否已通过 PIN 校验
    if (!ble_ct_pin_is_verified_for_conn(conn_handle)) {
        return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
    }
}
```

- [ ] **Step 4: Commit**

```bash
git commit -m "feat(ble): add PIN verification to provisioning and binding flows"
```

---

### 【交付】Task 13: 协议文档修订

**Files:** 3 处协议文档修订

- [ ] **Step 1: 修订① — §2.2 TELEMETRY 权限改为 Read+Notify**

- [ ] **Step 2: 修订② — §5.1 增加时间校准约定**

- [ ] **Step 3: 修订③ — §4.2/§4.3 增加 PIN 校验**

- [ ] **Step 4: Commit**

```bash
git commit -m "docs: update BLE protocol with 3 revisions (TELEMETRY Read, clock, PIN)"
```

---

### 【交付】Task 14: 全量构建 + 真机联调

- [ ] **Step 1: 全量构建**

```bash
idf.py fullclean && idf.py build
```

Expected: 零警告零错误

- [ ] **Step 2: 烧录并启动日志**

```bash
idf.py flash monitor
```

- [ ] **Step 3: 测试矩阵**

| 测试项 | 预期 |
|--------|------|
| 读 INFO（未配对） | 成功（无 ENC） |
| PIN 校验（正确） | 通过 |
| PIN 校验（错误 5 次） | 锁定 30 分钟 |
| 绑定（离网） | App 生成 device_key，设备持久化 |
| 鉴权（成功） | HMAC 比对通过，时钟校准 |
| 鉴权（3 次失败） | 锁定 60 秒 |
| 遥测推送 | 80s 节拍 + 数据突变即时推送 |
| 遥测 Read | 返回最新快照 |
| 命令执行 | command_id 幂等 |
| 单连接守卫 | 第二个连接被拒绝 |
| 断开重连 | 鉴权状态重置，锁定计数保留 |

- [ ] **Step 4: Final Commit**

```bash
git commit -m "feat(ble): BLE local mode v3 complete - PIN + auth + telemetry + commands"
```

---

## 4. 安全加固建议

| 项 | 建议 |
|----|------|
| PRODUCT_SECRET | 分片存储（S1+S2+S3），不要单字符串 |
| NVS 加密 | 量产启用 NVS Encryption（device_key、time_base） |
| Secure Boot | 量产启用 V2 Secure Boot |
| Flash Encryption | 量产启用 AES-256 Flash Encryption |
| eFuse RD_DIS | 禁止读取 eFuse 中的密钥材料 |
