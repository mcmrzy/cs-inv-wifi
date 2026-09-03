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
check("短 SN 右补零 → 自然序保留", sn_bytes_to_str(b"ABC" + b'\x00' * 13) == "ABC")
check("中段 0x00 → 跳过且保序", sn_bytes_to_str(b"A\x00B" + b'\x00' * 13) == "AB")

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
