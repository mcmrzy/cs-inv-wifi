# ARM 只读区 SN 改 4×u32(52B 布局)适配计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use compose:subagent (recommended) or compose:execute to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把 ESP32 固件、L10 ARM 模拟器、协议文档中"只读信息区 0x0190 起 44B/22×u16、SN=4×u16(8B, 每字高字节在前 BCD/ASCII)"的旧布局,全部适配为 ARM 新布局:SN 改为 4×u32(16B, pack(1), 小端自然字节序 ASCII),只读块扩为 52B/26×u16(0x0190-0x01A9)。

**Architecture:** 数据流 `ARM OnlyReadData_Def`(pack(1)) → UART 0x06 应答 → `g_only_read`(ESP32 端对齐结构体) → `onlyread_sn_to_str()` → NVS/MQTT。三处必须同构:① ESP32 `arm_param.h` 结构体与 `arm_poll.c` 解码;② `tools/arm_simulator_l10`(模拟 ARM 侧编解码 + 窗口长度);③ 协议文档中的地址/长度描述。SN 解码统一为:16 字节**自然序** ASCII(跳过 0x00 填充),不再有"每 16 位字高字节在前"与 BCD 分支。

**Tech Stack:** C(ESP-IDF 5.5.4, ESP32-C3)、Python 3(模拟器/逻辑镜像测试)、MSVC(宿主 cs_protocol 向量测试)、Markdown 协议文档。

## Global Constraints

- **ARM 端外部前置(不在本仓库,必须由用户同步修改并一起烧录)**:`OnlyReadDataEndIndex` 由 `0x1A5` 改为 `0x1A9`。否则 ESP32 改为请求 52B 后,ARM `ReadOnlyReadData` 越界校验 `param1+(param2>>1) > EndIndex+1` 会**静默拒绝**,0x06 通道整体失效(无 SN 同步、无 info)。
- 新只读布局(两端 pack(1) 逐字节对齐):偏移 0..9 = 5×u16(ARMFirmwareVersion…HardwareVersion);偏移 10..25 = `Inverter_SN1..4` 4×u32,每 u32 小端存放 4 个 ASCII 字符(`ch0 | ch1<<8 | ch2<<16 | ch3<<24`,内存序即字符序);偏移 26..51 = 13×u16(DeviceTypeCodeL…BLVersion,相对旧布局 +8B)。块总长 **52B = 26 个 u16**。
- SN 为 16 字符 ASCII(如 `H1CNA0013500001O` / 数字样例 `2024010123456789`),不足 16 用 `0x00` 填充;解码跳过 `0x00`,遇不可打印字节判非法(返回 false/None),**不更新 NVS、不重启**。
- 只读块全量 0x06 请求:起始 `0x0190`,长度 **52B**(`ONLYREAD_DATA_SIZE = 52`)。
- `cs_protocol.c`/TEA/PPP/帧加解密逻辑**不改**;`esp32c2_idf`(另一产品、另一协议)与 `docs/ARM_ESP32_UART_Protocol.md`(C2 协议文档)**不改**。
- 旧 ARM 固件(BCD、44B 窗口)与新 ESP32 固件不兼容:二者必须同步升级;计划不实现"52B 失败回退 44B"的兼容分支(YAGNI,需要时另立计划)。
- 所有跨文件长度/偏移常量保持一致:52 / 26 / 0x01A9 / SN 偏移 10..25 / 后续字段 +8。

---

### Task 1: `arm_param.h` — 结构体与索引宏适配 52B/4×u32

**Covers:** 新布局结构体、宏、地址表(全局约束 2/4)

**Files:**
- Modify: `D:\CS_INV_WIFI\esp32c3_l10_idf\main\telemetry\arm_param.h`

**Interfaces:**
- Consumes: 无(纯声明层)
- Produces: `OnlyReadDataDef`(52B)、`ONLYREAD_DATA_SIZE=52`、`ONLYREAD_END_INDEX=0x01A9`、`INDEX_INV_SN1..4=0x0195/0x0197/0x0199/0x019B`、后续 `INDEX_*` 字索引 +4(Task 2/3 依赖编译期一致)

- [ ] **Step 1: 改文件头布局注释**

将第 4-8 行区域:

```c
 * 布局说明:
 *  - RunParamDef     176 字节 (0x03E8 起, 88 个 u16), 以 ARM 源码为准
 *  - OnlyReadDataDef 44 字节  (0x0190 起, 22 个 u16)
 *  - ReadWiteDataDef 4 字节   (0x00C8 起)
```

改为:

```c
 * 布局说明:
 *  - RunParamDef     176 字节 (0x03E8 起, 88 个 u16), 以 ARM 源码为准
 *  - OnlyReadDataDef 52 字节  (0x0190 起, 26 个 u16; Inverter_SN1..4 = 4×u32, 16B ASCII 自然序)
 *  - ReadWiteDataDef 4 字节   (0x00C8 起)
```

- [ ] **Step 2: 替换整个"只读数据"段(第 196-249 行)**

原块从 `/* ==================== 只读数据 (OnlyReadData_Def, 44 字节) ==================== */` 起到 `#pragma pack(pop)` 结束,整体替换为:

```c
/* ==================== 只读数据 (OnlyReadData_Def, 52 字节) ==================== */
/* ARM 侧 pack(1): 5×u16(0..9) + Inverter_SN1..4 (4×u32, 偏移 10..25, 小端 ASCII
   自然序) + 13×u16 (偏移 26..51, 相对旧 44B 布局整体后移 8B)。
   全量读取 0x0190 起 52B (0x0190-0x01A9, 26 个 u16)。 */
#define ONLYREAD_START_INDEX 0x0190
#define ONLYREAD_END_INDEX   0x01A9
#define INDEX_ARM_FW_VER     0x0190
#define INDEX_DSP_FW_VER     0x0191
#define INDEX_INV_MODULE_L   0x0192
#define INDEX_INV_MODULE_H   0x0193
#define INDEX_HW_VER         0x0194
/* 4×u32 各占 2 个 u16 字: SN1@0x195, SN2@0x197, SN3@0x199, SN4@0x19B (起始字) */
#define INDEX_INV_SN1        0x0195
#define INDEX_INV_SN2        0x0197
#define INDEX_INV_SN3        0x0199
#define INDEX_INV_SN4        0x019B
#define INDEX_DEV_TYPE_CODE_L 0x019D
#define INDEX_DEV_TYPE_CODE_H 0x019E
#define INDEX_RATE_WATT      0x019F
#define INDEX_RATE_VA        0x01A0
#define INDEX_NOM_GRID_VOLT  0x01A1
#define INDEX_NOM_GRID_FREQ  0x01A2
#define INDEX_NOM_BAT_VOLT   0x01A3
#define INDEX_NOM_PV_CURR    0x01A4
#define INDEX_NOM_AC_CHG_CURR 0x01A5
#define INDEX_NOM_OP_VOLT    0x01A6
#define INDEX_NOM_OP_FREQ    0x01A7
#define INDEX_NOM_OP_POW     0x01A8
#define INDEX_BL_VERSION     0x01A9

#define ONLYREAD_DATA_SIZE   52      /* 26 个 u16 */

#pragma pack(push, 1)
typedef struct {
    uint16_t ARMFirmwareVersion;
    uint16_t DSPFirmwareVersion;
    uint16_t InverterModuleL;
    uint16_t InverterModuleH;
    uint16_t HardwareVersion;
    uint32_t Inverter_SN1;   /* 4 个 ASCII 字符, 小端: ch0 | ch1<<8 | ch2<<16 | ch3<<24 */
    uint32_t Inverter_SN2;
    uint32_t Inverter_SN3;
    uint32_t Inverter_SN4;
    uint16_t DeviceTypeCodeL;
    uint16_t DeviceTypeCodeH;
    uint16_t RateWatt;
    uint16_t RateVA;
    uint16_t NomGridVolt;
    uint16_t NomGridFreq;
    uint16_t NomBatVolt;
    uint16_t NomPvCurr;
    uint16_t NomAcChgCurr;
    uint16_t NomOpVolt;
    uint16_t NomOpFreq;
    uint16_t NomOpPow;
    uint16_t BLVersion;
} OnlyReadDataDef;           /* 52 字节 */
#pragma pack(pop)
```

- [ ] **Step 3: 编译期断言(永久保留)**

在 `#pragma pack(pop)` 之后、`#endif /* ARM_PARAM_H */` 之前追加(ESP-IDF 默认 gnu11 支持文件级 `_Static_assert`,防日后结构体误改):

```c
_Static_assert(sizeof(OnlyReadDataDef) == 52, "OnlyReadDataDef must be 52B");
```

- [ ] **Step 4: 提交**

```bash
git add esp32c3_l10_idf/main/telemetry/arm_param.h
git commit -m "feat(telemetry): ARM 只读区 SN 改 4×u32, 布局扩为 52B/0x01A9"
```

---

### Task 2: `verify_sn_sync.py` — 重写为"16B ASCII 自然序"逻辑镜像(先于固件解码落地)

**Covers:** 新 SN 解码语义 + 同步/重启/JSON 逻辑(全局约束 3)

**Files:**
- Rewrite: `D:\CS_INV_WIFI\tools\arm_simulator_l10\verify_sn_sync.py`

**Interfaces:**
- Consumes: 无(独立可执行)
- Produces: 可执行规范:`sn_bytes_to_str(raw16)`、`SimSync`、`main_loop_decide`、`publish_info_json`(Task 3 的 C 实现按此语义逐行对齐)

- [ ] **Step 1: 整文件重写为以下内容**

```python
#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
verify_sn_sync.py — SN 同步逻辑验证 (与 arm_poll.c 实现逐行对齐, 新 52B 布局)

SN 区域 = OnlyReadDataDef 偏移 10..25 共 16 字节 (Inverter_SN1..4 = 4×u32,
ARM 小端自然字节序存放 ASCII 字符)。解码: 跳过 0x00, 遇不可打印字节判非法。

场景:
  A. NVS 旧值 != ARM SN → 写 NVS + 置重启标志 + 日志 (was 为旧值)
  B. NVS == ARM SN     → 零写入
  C. SN 含非法字符     → 不更新不重启
  D. 主循环重启判定     → s_sn_changed && !s_prov_busy → 重启; 配网中不重启
  E. 重启后一致         → 后续轮询零写入
  F. publish_info JSON  → sn 字段正确插入 / 非法 SN 省略且 JSON 合法

用法: python verify_sn_sync.py
退出码: 0 = 全部通过
"""

import sys
import json

# ---------------- 与 arm_poll.c onlyread_sn_to_str 对齐的转换 ----------------
def sn_bytes_to_str(raw):
    """raw: 16 字节 (偏移 10..25, 4×u32 小端 ASCII 自然序)。
    跳过 0x00 填充; 非 0x00 且非可打印 (0x20-0x7E) → 非法返回 None;
    全 0x00 / 长度非 16 → None。"""
    if len(raw) != 16:
        return None
    out = []
    for c in raw:
        if c == 0x00:
            continue
        if not (0x20 <= c <= 0x7E):
            return None
        out.append(chr(c))
    return ''.join(out) if out else None


def bytes_from_sn(s):
    """SN 字符串 → 16 字节 (ASCII 自然序, 右补 0x00), 与 ARM 端 4×u32 写一致。"""
    s = s.strip()
    if not s or len(s) > 16:
        return None
    if any(not (0x20 <= ord(c) <= 0x7E) for c in s):
        return None
    return s.encode('ascii').ljust(16, b'\x00')


# ---------------- 与 arm_poll.c sync_sn_from_onlyread 对齐的同步逻辑 ----------------
class SimSync:
    def __init__(self, nvs_sn):
        self.nvs_sn = nvs_sn            # g_device_sn (NVS 加载值)
        self.g_device_sn = nvs_sn
        self.s_sn_changed = False
        self.nvs_writes = 0
        self.logs = []

    def sync(self, raw16):
        arm_sn = sn_bytes_to_str(raw16)
        if arm_sn is None:
            return                       # 非法 SN: 直接返回
        if arm_sn != self.g_device_sn:   # 仅不一致才写 NVS
            old = self.g_device_sn
            self.nvs_sn = arm_sn         # storage_set_sn(arm_sn)
            self.g_device_sn = arm_sn    # strlcpy(g_device_sn, ...)
            self.s_sn_changed = True
            self.nvs_writes += 1
            self.logs.append("SN from ARM: %s (was %s), restart to apply" % (arm_sn, old))


# ---------------- 与 app_main.c main_loop_task 对齐的重启判定 ----------------
def main_loop_decide(changed, prov_busy):
    return changed and not prov_busy    # → vTaskDelay(2s) → esp_restart()


# ---------------- 与 telemetry.c publish_info 对齐的 JSON 构造 ----------------
def publish_info_json(raw16):
    sn_str = sn_bytes_to_str(raw16)
    sn_json = '"sn":"%s",' % sn_str if sn_str else ""
    return ("{" + sn_json +
            '"model":"CS-L10-6K2","manufacturer":"辰烁科技",'
            '"firmware_arm":"V1.4","firmware_esp":"V1.2.0.L10","firmware_dsp":"V1.0",'
            '"firmware_bms":null,"device_type":"off_grid_inverter","phase":"single",'
            '"rated_power":5000,"rated_voltage":24.0,"rated_frequency":0.60,'
            '"battery_nominal_voltage":0.60,"battery_type":"LiFePO4",'
            '"cell_count":0,"temp_sensor_count":0,'
            '"inverter_module":"16908546","hardware_version":"V0.0",'
            '"bootloader_version":"V1.1"}')


# ---------------- 场景执行 ----------------
PASS = 0

def check(name, cond):
    global PASS
    if cond:
        PASS += 1
        print("  [PASS] %s" % name)
    else:
        print("  [FAIL] %s" % name)
        sys.exit(1)

# 模拟器默认 SN (simulator.py _init_only_read): ASCII "2024010123456789" (16 字符)
ARM_SN_STR = "2024010123456789"
SIM_SN = bytes_from_sn(ARM_SN_STR)          # 16B ASCII 自然序
BAD_SN = bytearray(bytes_from_sn("H1ZZ0X1001000001"))  # 合法 16B
BAD_SN[10] = 0x0F                           # 非法字符
BAD_SN = bytes(BAD_SN)

print("== 场景 A: NVS 旧值 H1CNA0013500001O != ARM SN ==")
s = SimSync(nvs_sn="H1CNA0013500001O")
s.sync(SIM_SN)
check("转换结果正确", sn_bytes_to_str(SIM_SN) == ARM_SN_STR)
check("NVS 写入 1 次", s.nvs_writes == 1)
check("s_sn_changed 置位", s.s_sn_changed)
check("g_device_sn 已更新", s.g_device_sn == ARM_SN_STR)
check("日志含旧值(was 未丢失)", s.logs and s.logs[0].endswith("(was H1CNA0013500001O), restart to apply"))
check("重启判定: 非配网中 → 重启", main_loop_decide(s.s_sn_changed, False) is True)
check("重启判定: 配网中 → 不重启", main_loop_decide(s.s_sn_changed, True) is False)
# 重启后模拟 (NVS 已是新值): 后续轮询走比较分支
s2 = SimSync(nvs_sn=ARM_SN_STR)
s2.sync(SIM_SN)
check("重启后一致 → 零写入", s2.nvs_writes == 0 and not s2.s_sn_changed and not s2.logs)

print("== 场景 B: NVS == ARM SN (首次启动已一致) ==")
s = SimSync(nvs_sn=ARM_SN_STR)
s.sync(SIM_SN)
check("零写入", s.nvs_writes == 0)
check("不置标志", not s.s_sn_changed)
check("无日志", not s.logs)

print("== 场景 C: 非法字符 (0x0F) → 不更新不重启 ==")
check("转换返回 None (拒绝)", sn_bytes_to_str(BAD_SN) is None)
s = SimSync(nvs_sn="H1CNA0013500001O")
s.sync(BAD_SN)
check("不更新", s.g_device_sn == "H1CNA0013500001O")
check("不置标志", not s.s_sn_changed)
check("零写入", s.nvs_writes == 0)
check("不重启", main_loop_decide(s.s_sn_changed, False) is False)
check("全 0x00 → None", sn_bytes_to_str(b'\x00' * 16) is None)
check("长度非 16 → None", sn_bytes_to_str(b'abc') is None)

print("== 场景 E: 多轮轮询不重复写 ==")
s = SimSync(nvs_sn="H1CNA0013500001O")
for _ in range(10):      # 10 次 0x06 应答 (重启前)
    s.sync(SIM_SN)
check("仅首次写入, 后续零写入", s.nvs_writes == 1)

print("== 场景 F: publish_info JSON ==")
j = publish_info_json(SIM_SN)
check("sn 字段插入在 model 之前", j.startswith('{"sn":"2024010123456789","model"'))
payload = json.loads(j)
check("JSON 可解析且 sn 值正确", payload["sn"] == ARM_SN_STR)
check("其余字段不受影响", payload["model"] == "CS-L10-6K2" and payload["firmware_arm"] == "V1.4")
j2 = publish_info_json(BAD_SN)
check("非法 SN → 省略 sn 字段", '"sn"' not in j2 and j2.startswith('{"model"'))
payload2 = json.loads(j2)
check("无 sn 时 JSON 仍合法", payload2["model"] == "CS-L10-6K2" and "sn" not in payload2)

print("\n全部通过 (%d checks)" % PASS)
```

- [ ] **Step 2: 运行确认全绿**

Run: `python D:\CS_INV_WIFI\tools\arm_simulator_l10\verify_sn_sync.py`
Expected: 逐行 `[PASS]` … 末尾 `全部通过 (N checks)`,退出码 0。

- [ ] **Step 3: 提交**

```bash
git add tools/arm_simulator_l10/verify_sn_sync.py
git commit -m "test(simulator): SN 逻辑镜像改为 16B ASCII 自然序 (52B 布局)"
```

---

### Task 3: `arm_poll.c` — 解码与字段表对齐新布局

**Covers:** ESP32 解码(全局约束 3)、0x06 请求/校验长度自动随 `ONLYREAD_DATA_SIZE=52`(全局约束 4)

**Files:**
- Modify: `D:\CS_INV_WIFI\esp32c3_l10_idf\main\telemetry\arm_poll.c`
- Modify: `D:\CS_INV_WIFI\esp32c3_l10_idf\main\telemetry\arm_poll.h`(注释)
- Modify: `D:\CS_INV_WIFI\esp32c3_l10_idf\main\telemetry\telemetry.c`(注释)

**Interfaces:**
- Consumes: Task 1 的 `OnlyReadDataDef`(SN 为 4×u32);Task 2 的可执行规范
- Produces: 新 `onlyread_sn_to_str`(语义与 Task 2 `sn_bytes_to_str` 一致);调试字段表 `s_onlyread_fields`(新偏移)

- [ ] **Step 1: 替换调试字段表(arm_poll.c:126-138 区域)**

原表替换为:

```c
/* 只读信息 0x0190-0x01A9 (OnlyReadDataDef, 52B; SN 为 4×u32) */
static const arm_dbg_field_t s_onlyread_fields[] = {
    { "ARMFirmwareVersion",   0,  2 }, { "DSPFirmwareVersion",  2,  2 },
    { "InverterModuleL",      4,  2 }, { "InverterModuleH",     6,  2 },
    { "HardwareVersion",      8,  2 },
    { "Inverter_SN1",        10,  4 }, { "Inverter_SN2",       14,  4 },
    { "Inverter_SN3",        18,  4 }, { "Inverter_SN4",       22,  4 },
    { "DeviceTypeCodeL",     26,  2 }, { "DeviceTypeCodeH",    28,  2 },
    { "RateWatt",            30,  2 }, { "RateVA",             32,  2 },
    { "NomGridVolt",         34,  2 }, { "NomGridFreq",        36,  2 },
    { "NomBatVolt",          38,  2 }, { "NomPvCurr",          40,  2 },
    { "NomAcChgCurr",        42,  2 }, { "NomOpVolt",          44,  2 },
    { "NomOpFreq",           46,  2 }, { "NomOpPow",           48,  2 },
    { "BLVersion",           50,  2 },
};
```

- [ ] **Step 2: 替换 `onlyread_sn_to_str`(arm_poll.c:188-241 区域, 注释+函数一起)**

原函数整体替换为:

```c
/* Inverter_SN1..4 (4×u32, 偏移 10~25) = 16 字节 ASCII (小端自然字节序, 与 ARM
   端 pOnlyReadData->Inverter_SN1 = ch0 | ch1<<8 | ch2<<16 | ch3<<24 一致);
   跳过 0x00 填充; 含不可打印字节/全 0x00 → 非法返回 false */
bool onlyread_sn_to_str(const OnlyReadDataDef *o, char *out, size_t len)
{
    const uint8_t *b;
    char *p = out;

    if (!o || !out || len < 17) {
        return false;
    }
    b = (const uint8_t *)&o->Inverter_SN1;
    for (int i = 0; i < 16; i++) {
        uint8_t c = b[i];
        if (c == 0x00) {
            continue;
        }
        if (c < 0x20 || c > 0x7E) {
            return false;   /* 非法字符 */
        }
        *p++ = (char)c;
    }
    if (p == out) {
        return false;       /* 全 0x00 */
    }
    *p = '\0';
    return true;
}
```

- [ ] **Step 3: 同步三处注释**

- `arm_poll.c:125` 上方注释若仍写 "44B/22 个 u16" 一并改为 "52B/26 个 u16"。
- `arm_poll.h:27`:
  `/* 只读信息 SN (Inverter_SN1..4, BCD) → 16 位数字字符串; 非法 BCD 返回 false */`
  → `/* 只读信息 SN (Inverter_SN1..4, 4×u32, 16B ASCII 自然序) → 字符串; 非法返回 false */`
- `telemetry.c:216`:
  `/* SN: 只读信息 Inverter_SN1..4 (BCD 16 位数字); 非法 BCD 时省略字段 */`
  → `/* SN: 只读信息 Inverter_SN1..4 (4×u32, ≤16 位 ASCII); 非法时省略字段 */`

- [ ] **Step 4: 确认无其他旧语义残留**

Run: 在 `D:\CS_INV_WIFI\esp32c3_l10_idf` 下搜索
`rg -n "BCD|44 字节|44B|0x01A5|Inverter_SN1.*2," main/telemetry`
预期: 不再命中旧 SN 解码描述(Task 4/5 文档与模拟器例外, 由各自任务处理)。

- [ ] **Step 5: 编译验证(此处仅做语法级快检, 全量构建在 Task 6)**

Run(工作目录 `D:\CS_INV_WIFI\esp32c3_l10_idf`):
```powershell
$env:IDF_PATH="D:\.espressif\v5.5.4\esp-idf"; $env:IDF_TOOLS_PATH="C:\Users\29538\.espressif"; $env:IDF_PYTHON_ENV_PATH="C:\Users\29538\.espressif\python_env\idf5.5_py3.13_env"; $T="C:\Users\29538\.espressif\tools"; $env:PATH="$T\cmake\3.30.2\bin;$T\ninja\1.12.1;$T\riscv32-esp-elf\esp-14.2.0_20260121\riscv32-esp-elf\bin;$T\idf-exe\1.0.3;$T\ccache\4.11.2;$env:PATH"; & "$env:IDF_PYTHON_ENV_PATH\Scripts\python.exe" "$env:IDF_PATH\tools\idf.py" build
```
Expected: 增量构建成功(仅重编 telemetry 相关),无 error;若有 `_Static_assert` 错误则回到 Task 1 检查结构体。

- [ ] **Step 6: 提交**

```bash
git add esp32c3_l10_idf/main/telemetry/arm_poll.c esp32c3_l10_idf/main/telemetry/arm_poll.h esp32c3_l10_idf/main/telemetry/telemetry.c
git commit -m "fix(telemetry): SN 解码改 16B ASCII 自然序并适配 52B 只读布局"
```

---

### Task 4: `simulator.py` 及配套工具 — ARM 侧模拟改为 4×u32/52B

**Covers:** 模拟器布局/编解码/CLI(全局约束 2/3/4),测试向量与环回/联调脚本同步

**Files:**
- Modify: `D:\CS_INV_WIFI\tools\arm_simulator_l10\simulator.py`
- Modify: `D:\CS_INV_WIFI\tools\arm_simulator_l10\uart_loopback_test.py`(常量)
- Modify: `D:\CS_INV_WIFI\tools\arm_simulator_l10\run_sn_sync_test.py`(CLI 参数)
- Modify: `D:\CS_INV_WIFI\tools\arm_simulator_l10\gen_vectors.py`(0x06 请求长度)
- Regenerate: `D:\CS_INV_WIFI\tools\arm_simulator_l10\test_vectors.h`

**Interfaces:**
- Consumes: Task 2/3 的编码规范(字符自然序 16B、52B 窗口)
- Produces: 模拟 ARM 正确发出 52B/4×u32 SN 的应答;`--sn <ascii>` CLI(替代 `--sn-bcd`);`ONLYREAD_SIZE=52`

- [ ] **Step 1: 模块头注释(第 9 行)**

`参数模型 ParamModel — 控制参数 84B / 可读写 4B / 只读 44B / 运行参数 176B,` → `… / 只读 52B / …`

- [ ] **Step 2: 删除未使用常量 `ONLYREAD_FIELDS`(第 155-164 行)**

整块(含上方 `# 只读信息字段 (OnlyReadDataDef 44B)` 注释)删除。先确认无引用(已确认全库仅此处定义)。

- [ ] **Step 3: `DEVICE_INFO_FIELDS`(第 169-187 行)偏移整体后移并改 SN 长度**

原表整块替换为(注意第 5 行起偏移 +8, SN 宽 8→16):

```python
# ======================================================================
# 设备信息 tab 字段元数据: (名称, 字节偏移, 字节数, 类型, 单位, 描述)
# ARM pack(1) 52B 布局: SN=4×u32 占偏移 10..25, 其后字段相对旧 44B 布局 +8
# ======================================================================
DEVICE_INFO_FIELDS = [
    ("ARMFirmwareVersion",  0,  2, "ver",   "",       "ARM固件版本 (如 V1.1→0x0101)"),
    ("DSPFirmwareVersion",  2,  2, "ver",   "",       "DSP固件版本 (如 V2.3→0x0203)"),
    ("InverterModule",      4,  4, "str4",  "",       "逆变器型号 (4字节ASCII)"),
    ("HardwareVersion",     8,  2, "ver",   "",       "硬件版本"),
    ("Inverter_SN",       10, 16, "sn",    "",       "设备序列号 (16字节ASCII, 4×u32 小端自然序, 可编辑)"),
    ("DeviceTypeCode",    26,  4, "hex4",  "",       "设备类型码 (小端)"),
    ("RateWatt",          30,  2, "u16",   "W",      "额定功率"),
    ("RateVA",            32,  2, "u16",   "VA",     "额定视在功率"),
    ("NomGridVolt",       34,  2, "u16",   "0.1V",   "额定电网电压"),
    ("NomGridFreq",       36,  2, "u16",   "0.01Hz", "额定电网频率"),
    ("NomBatVolt",        38,  2, "u16",   "0.01V",  "额定电池电压"),
    ("NomPvCurr",         40,  2, "u16",   "A",      "额定光伏电流"),
    ("NomAcChgCurr",      42,  2, "u16",   "0.1A",   "额定市电充电电流"),
    ("NomOpVolt",         44,  2, "u16",   "0.1V",   "额定输出电压"),
    ("NomOpFreq",         46,  2, "u16",   "0.01Hz", "额定输出频率"),
    ("NomOpPow",          48,  2, "u16",   "W",      "额定输出功率"),
    ("BLVersion",         50,  2, "ver",   "",       "Bootloader版本"),
]
```

- [ ] **Step 4: 替换 SN 解码函数(第 190-238 区域, 含上方注释)**

`onlyread_sn_bcd_to_str` 整体替换为:

```python
# ======================================================================
# SN 编解码辅助函数 (与 arm_poll.c onlyread_sn_to_str 对齐, 52B 布局)
# ======================================================================
def onlyread_sn_to_str(data: bytearray) -> str:
    """将 only_read 偏移10处 16 字节 (Inverter_SN1..4 = 4×u32, 小端 ASCII 自然序)
    转为字符串: 跳过 0x00; 含不可打印字节 → '?' * 16 (GUI 标记非法)"""
    raw = bytes(data[10:26])
    out = []
    for c in raw:
        if c == 0x00:
            continue
        if not (0x20 <= c <= 0x7E):
            return "?" * 16
        out.append(chr(c))
    return ''.join(out)
```

- [ ] **Step 5: 替换 SN 编码函数(第 240-280 区域)**

`str_to_sn_bcd` 整体替换为:

```python
def str_to_sn(s: str) -> str:
    """将 SN 字符串 (≤16 位 ASCII) 规范化: 原样返回, 校验可打印; 供 _write_sn 使用"""
    s = s.strip()
    if not s or len(s) > 16:
        raise ValueError("SN 长度须为 1..16 字符")
    if any(not (0x20 <= ord(c) <= 0x7E) for c in s):
        raise ValueError("SN 只能含可打印 ASCII 字符")
    return s


def sn_to_bytes(s: str) -> bytearray:
    """SN 字符串 → 16 字节, 右补 0x00; 与 ARM 端 4×u32 小端写入一致"""
    return bytearray(s.encode('ascii').ljust(16, b'\x00'))
```

- [ ] **Step 6: `ParamModel` 长度与初始化**

- 第 318-319 行:
  ```python
  ONLYREAD_START = 0x0190
  ONLYREAD_SIZE = 44      # 0x0190-0x01A5 (SN 8B: 4 个 u16)
  ```
  → `ONLYREAD_SIZE = 52      # 0x0190-0x01A9 (SN 16B: 4×u32)`
- `__init__(self, sn_bcd=None)` → `__init__(self, sn=None)`,签名内注释 `# 只读信息 (44B)` → `(52B)`,调用 `self._init_only_read(sn_bcd)` → `self._init_only_read(sn)`;默认值在 `_init_only_read` 内兜底。
- `_init_only_read`(第 450-475 行)整体替换为:

```python
    def _init_only_read(self, sn=None):
        # SN: Inverter_SN1..4 = 4×u32 (16B ASCII 小端自然序); 可用 --sn 覆盖
        sn_str = sn or "2024010123456789"
        if len(sn_str) > 16:
            sn_str = sn_str[:16]
        set_u16(self.only_read, 0, 0x0101)   # ARMFirmwareVersion V1.1
        set_u16(self.only_read, 2, 0x0203)   # DSPFirmwareVersion V2.3
        set_u16(self.only_read, 4, 0x6B32)   # InverterModuleL 'k2'
        set_u16(self.only_read, 6, 0x0000)   # InverterModuleH
        set_u16(self.only_read, 8, 0x0100)   # HardwareVersion V1.0
        for i in range(4):                   # SN1..4: 每 u32 4 字符 (小端)
            off = 10 + i * 4
            chunk = sn_to_bytes(sn_str[i * 4:(i + 1) * 4])
            set_u32(self.only_read, off,
                    chunk[0] | (chunk[1] << 8) | (chunk[2] << 16) | (chunk[3] << 24))
        # 偏移 26..51 (相对旧 44B 布局 +8)
        for off, v in ((26, 0xC001), (28, 0x0000), (30, 6000), (32, 6000),
                       (34, 2300), (36, 5000), (38, 5120), (40, 4000),
                       (42, 1000), (44, 2300), (46, 5000), (48, 6000),
                       (50, 0x0001)):
            set_u16(self.only_read, off, v)
```

  注意: `sn_str` 若 <16 字符,`sn_to_bytes(sn_str[i*4:(i+1)*4])` 每次截取可能为空串 → `ljust(16)` 后取前 4 字节为 0,正确补零。

- [ ] **Step 7: `device_info_values` 的 `sn` 分支(第 658-660 行)**

```python
            elif typ == "sn":
                raw = onlyread_sn_bcd_to_str(self.only_read)
                disp = raw if "?" not in raw else raw + " (非法字符)"
```
→ 调用名改为 `onlyread_sn_to_str`。

- [ ] **Step 8: GUI 文案与写 SN**

- 第 848 行 label: `只读设备信息 (44B OnlyReadDataDef — 蓝色字段可编辑)` → `(52B …)`。
- 第 878 行提示: `数字: 2024010123456789\n字母: H1CNA00135000014` 保留(仍 ≤16 位),如含 BCD 措辞则改为 "SN: 1..16 位可打印 ASCII"。
- `_write_sn`(第 967-983 行)整体替换为:

```python
    def _write_sn(self):
        """将输入的SN写入 only_read 的 SN 区域 (16 位 ASCII, 模拟 ARM 4×u32 写入)"""
        s = self.sn_var.get().strip()
        try:
            s = str_to_sn(s)
        except ValueError as e:
            messagebox.showerror("SN 格式错误", str(e))
            return
        raw = sn_to_bytes(s)
        for i in range(4):  # Inverter_SN1..4: 偏移 10..25, 每 u32 4 字符
            off = 10 + i * 4
            set_u32(self.model.only_read, off,
                    raw[off - 10] | (raw[off - 9] << 8) | (raw[off - 8] << 16) | (raw[off - 7] << 24))
        # 刷新界面
        self._refresh_info_tab()
        sn_vals = [get_u32(self.model.only_read, 10 + i * 4) for i in range(4)]
        sn_str = "SN1-4(u32): 0x%08X 0x%08X 0x%08X 0x%08X" % tuple(sn_vals)
        self.log("SN 已写入: '%s' → [%s] (模拟 ARM 已生效)" % (s, sn_str), "info")
        self.sn_bcd_label.config(text=sn_str)
        self.sn_status_label.config(text="写入成功 ✓", foreground="green")
```

- `_refresh_info_tab`(第 995-997 行): `onlyread_sn_bcd_to_str` → `onlyread_sn_to_str`;预览行 `sn_vals = [get_u16(...10 + i * 2...) for i in range(4)]` 与 `"SN1-4: 0x%04X ..."` 改为按 u32 读取(同上 `_write_sn` 的 `sn_vals`/格式串一致)。

- [ ] **Step 9: headless CLI 由 `--sn-bcd` 改为 `--sn`**

- `headless_run(port, baud, sn_bcd=None)` → `headless_run(port, baud, sn=None)`,`ParamModel(sn_bcd)` → `ParamModel(sn)`。
- 删除 `parse_sn_bcd`(第 1070-1078 行),新增:

```python
def parse_sn(s):
    """--sn: 1..16 位 ASCII SN 字符串, 如 '2024010123456789'"""
    s = s.strip()
    if not s or len(s) > 16:
        raise argparse.ArgumentTypeError("--sn 需要 1..16 个字符")
    return s
```

- main()(第 1081-1096 行): `--sn-bcd` 参数与打印替换为 `--sn`,调用 `headless_run(args.port, args.baud, args.sn)`。

- [ ] **Step 10: `uart_loopback_test.py` 常量**

- 第 38-39 行 `ONLYREAD_START = 0x0190` 保留;`ONLYREAD_SIZE = 44` → `52`。
- 第 170 行附近请求用例 `(P.CMD_READ_R_DATA, 0x0190, 44)` → `(…, 52)`。
- 文件头注释(第 10 行) `(READ_R_DATA 0x0190, 44B)` → `(READ_R_DATA 0x0190, 52B)`。

- [ ] **Step 11: `run_sn_sync_test.py` 适配 `--sn`**

- 第 78-86 行 `start_simulator(arm_port, sn_bcd=…, …)` / `--sn-bcd` → `sn` / `--sn`;第 36-37 行常量改为:
  ```python
  ARM_SN_STR = "2024010123456789"          # 模拟器默认 SN (16 位 ASCII)
  BAD_SN = "H1ZZ0X10000000F0"              # 非法: 含 0x0F? 不可 → 用含非法字符串
  ```
  注:SN 由 `--sn` 传字符串,无法携带 0x0F;非法场景改为传含非法字符的 ASCII? 非法字符是"非可打印",`--sn` 已拒绝。因此场景 C 调整为:改为依赖 verify_sn_sync.py(逻辑层已覆盖),运行联调脚本删除 `--bad-bcd` 分支或改为 `--bad-sn`(传一个含不可打印字符无法表达 → 直接删除场景 C 的 CLI 分支,注释指向 verify_sn_sync.py)。具体:移除 `--bad-bcd`/`BAD_SN_BCD` 相关代码,`main()` 中删除场景 C 块;文档字符串同步。

- [ ] **Step 12: 重新生成协议向量并跑宿主 C 测试**

- `gen_vectors.py` 第 23-24 行:`param2=44` → `param2=52`。
- Regenerate: `python D:\CS_INV_WIFI\tools\arm_simulator_l10\gen_vectors.py`(应输出 `wrote …test_vectors.h (10 vectors)`)。
- Host 测试: `powershell -ExecutionPolicy Bypass -File D:\CS_INV_WIFI\tools\arm_simulator_l10\build_test.ps1`(编译 `cs_protocol.c` + `test_cs_protocol.c` 并运行,含新 52B read_rdata 向量)。
  Expected: 退出码 0、全向量 PASS。

- [ ] **Step 13: 语法/自检**

Run: `python -m py_compile simulator.py uart_loopback_test.py run_sn_sync_test.py gen_vectors.py verify_sn_sync.py`(工作目录 `tools/arm_simulator_l10`)以及 `python verify_sn_sync.py`(应仍全绿)。

- [ ] **Step 14: 提交**

```bash
git add tools/arm_simulator_l10/
git commit -m "feat(simulator): 只读区 SN 改 4×u32/52B, --sn 替代 --sn-bcd, 向量重生成"
```

---

### Task 5: 协议文档同步 44B→52B / SN 4×u32

**Covers:** 文档中的地址/长度/SN 编码描述(全局约束 2)

**Files:**
- Modify: `D:\CS_INV_WIFI\docs\CS-L10-6K2_MQTT_上报协议设计_V2.1.md`
- Modify: `D:\CS_INV_WIFI\docs\CS-L10-6K2_MQTT_上报协议设计_V2.0.md`

**Interfaces:**
- Consumes: Task 1 的宏值(52/26/0x01A9)
- Produces: 与代码一致的协议描述

- [ ] **Step 1: V2.1 精确替换(先读后改)**

- 第 105 行:
  `| 0x06 | READ_R_DATA | 读只读数据（0x0190-0x01A5：版本/SN/额定值，44B） |`
  → `| 0x06 | READ_R_DATA | 读只读数据（0x0190-0x01A9：版本/SN/额定值，52B） |`
- 第 116 行:
  `| 只读 | 0x0190-0x01A5 | 44 字节 / 22 个 u16 | ARM/DSP 版本、模块号、SN(BCD)、硬件版本、额定参数、Bootloader 版本（见 7.1） |`
  → `| 只读 | 0x0190-0x01A9 | 52 字节 / 26 个 u16 | ARM/DSP 版本、模块号、SN（4×u32，16B ASCII）、硬件版本、额定参数、Bootloader 版本（见 7.1） |`
- 第 375 行(含 `0x0190-0x01A5（44B / 22 个 u16）` 的句子):按同一 token 规则替换(`0x0190-0x01A5`→`0x0190-0x01A9`、`44B / 22 个 u16`→`52B / 26 个 u16`)。
- 第 899 行同 token 规则替换(`（44B）` 只出现在"0x06 READ_R_DATA 读 …"描述里时→`（52B）`)。

- [ ] **Step 2: 全文 token 清扫 V2.1**

全文查找替换以下 token(仅这些精确串):
- `0x0190-0x01A5` → `0x0190-0x01A9`
- `44 字节 / 22 个 u16` → `52 字节 / 26 个 u16`
- `44B / 22 个 u16` → `52B / 26 个 u16`
- `SN(BCD)` → `SN（4×u32，16B ASCII）`
替换后再全文搜索 `44B|0x01A5|22 个 u16|SN(BCD)` 确认无残留(注意勿误伤 176B 运行参数等无关 44 数值——只替换上述完整 token)。

- [ ] **Step 3: V2.0 同步**

- 第 101 行、第 112 行做与 Step 1 相同的两处行替换;若文件其余处出现相同 token,按 Step 2 规则一并替换(V2.0 为历史版本,保持与 V2.1 同值即可)。

- [ ] **Step 4: 提交**

```bash
git add docs/CS-L10-6K2_MQTT_上报协议设计_V2.1.md docs/CS-L10-6K2_MQTT_上报协议设计_V2.0.md
git commit -m "docs: 只读区布局 44B→52B, SN 描述改 4×u32 ASCII"
```

---

### Task 6: 集成验证

**Covers:** 全部任务的收口验证(编译 + 逻辑测试 + 一致性)

**Files:** 无(只运行验证)

- [ ] **Step 1: 逻辑镜像测试**

Run: `python D:\CS_INV_WIFI\tools\arm_simulator_l10\verify_sn_sync.py`
Expected: 全 PASS、退出码 0。

- [ ] **Step 2: 协议一致性(模拟器应答 → 固件解码)离线核对**

Run(工作目录 `tools\arm_simulator_l10`):
```python
import simulator as sim
m = sim.ParamModel()               # 默认 SN "2024010123456789"
print(m.ONLYREAD_SIZE)             # 52
print(sim.onlyread_sn_to_str(m.only_read))   # 2024010123456789
print([hex(sim.get_u32(m.only_read, 10 + i*4)) for i in range(4)])
```
Expected: `52`、`2024010123456789`、按 4 字符小端拼出的 4 个 u32(第一个 `0x34333231` = '1','2','3','4' 的反序内存即 '2024' 段:校验 `0x32303432`? 首段 "2024" → u32 = '2'|'0'<<8|'2'<<16|'4'<<24 = 0x34323032;以实际输出为准,肉眼确认与 ARM 写式一致即可)。

- [ ] **Step 3: 固件全量增量编译**

Run(工作目录 `D:\CS_INV_WIFI\esp32c3_l10_idf`,命令同 Task 3 Step 5)。
Expected: 编译成功、无 error;日志中 `OnlyReadDataDef` 相关无告警。

- [ ] **Step 4: 检查 sdkconfig 未被意外改动**

`git -C D:\CS_INV_WIFI diff --stat -- esp32c3_l10_idf/sdkconfig`(若 Task 3 构建重配导致 sdkconfig 变更,说明与 `sdkconfig.defaults` 不一致,需 git checkout 该文件并重跑确认仍可增量构建)。

- [ ] **Step 5: 汇总 diff 自查**

Run: `git -C D:\CS_INV_WIFI diff --stat -- esp32c3_l10_idf/main/telemetry tools/arm_simulator_l10 docs/CS-L10-6K2_MQTT_上报协议设计_V2.1.md docs/CS-L10-6K2_MQTT_上报协议设计_V2.0.md`
Expected: 只包含本计划涉及文件;无意外文件。

- [ ] **Step 6: 硬件联调(可选, 需设备在位, 标记为人工步骤)**

1. 确认 ARM 固件已同步烧录(EndIndex=0x1A9)。
2. 烧录 ESP32 新固件。
3. `python D:\CS_INV_WIFI\tools\arm_simulator_l10\run_sn_sync_test.py --arm-port COM5`
   Expected: 场景 A/B 日志断言通过;MQTT `info` 中 `"sn"` 与铭牌一致、`rated_power/rated_voltage/bootloader_version` 数值正确(此前错位)。

---

## Self-Review 记录

- **Spec 覆盖**:全局约束 1(ARM EndIndex)为外部前置,已写入 Global Constraints 与 Task 6 Step 6;约束 2/3/4 分别落在 Task 1(结构/宏)、Task 2+3(解码语义)、Task 3(请求长度随宏)+Task 4(模拟窗口)。无遗漏。
- **无占位符**:各 Task 均给出精确文件、行区域、替换代码或命令;唯一"人工"步骤为硬件联调(明确标注可选)。
- **类型/命名一致性**:解码函数 C `onlyread_sn_to_str` ↔ Python `sn_bytes_to_str` 语义对齐;编码 `str_to_sn`/`sn_to_bytes` 仅存在于 Python;`ONLYREAD_DATA_SIZE=52` 与 `ONLYREAD_SIZE=52`、`0x01A9` 三处同值;`Inverter_SN1..4` 在 C 侧保持原名(u32),调试表/UI 用 `Inverter_SN1..4` 行。
