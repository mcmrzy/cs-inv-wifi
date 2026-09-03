#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
simulator.py — CS-L10-6K2 ARM 端模拟器 (协议层 + 参数模型 + 响应逻辑 + GUI)

基于 protocol.py (与 ARM 源码逐字节对齐的 PPP+TEA CBC+CRC32) 构建:

  协议层   protocol.PPPReceiver / decrypt_rx / parse_frame / send_frame
  参数模型 ParamModel — 控制参数 84B / 可读写 4B / 只读 52B / 运行参数 176B,
                       运行参数按正弦波动模拟, 控制参数可手动修改
  响应逻辑 读/写命令处理, 写命令回 err=0; UTC 写入后模拟校时 (打印差值)
  GUI     tkinter: 运行参数实时表格 / 控制参数读写编辑 / 设备信息(SN) / 日志面板

用法:
  python simulator.py                # GUI 模式
  python simulator.py --headless     # 命令行模式 (选端口自动打开)
  python simulator.py --port COM5 --baud 115200 --headless
"""

import sys
import os
import time
import math
import struct
import threading
import argparse
import random
from datetime import datetime

# 允许直接运行 tools/arm_simulator_l10/simulator.py
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import protocol as P

try:
    import serial
    import serial.tools.list_ports
    HAS_SERIAL = True
except ImportError:
    HAS_SERIAL = False

try:
    import tkinter as tk
    from tkinter import ttk, scrolledtext, messagebox
    HAS_TK = True
except ImportError:
    HAS_TK = False

CMD_NAMES = {
    P.CMD_READ_CTRLPARAM: "READ_CTRLPARAM",
    P.CMD_WRITE_CTRLPARAM: "WRITE_CTRLPARAM",
    P.CMD_READ_RUNPARAM: "READ_RUNPARAM",
    P.CMD_READ_R_W_DATA: "READ_R_W_DATA",
    P.CMD_WRITE_R_W_DATA: "WRITE_R_W_DATA",
    P.CMD_READ_R_DATA: "READ_R_DATA",
    P.CMD_CTRLCMD: "CTRLCMD",
    P.CMD_READ_TEST_RUNPARAM: "READ_TEST_RUNPARAM",
}

# ======================================================================
# 控制参数名 (CollectorParamAddr.h 索引, 84 个 u16)
# ======================================================================
CTRL_NAMES = [
    "OutputPriority", "MaxChargeCurrentAll", "ACInVoltRange", "BatCapacity",
    "BatteryType", "OverloadRestart", "HighTempRestart", "OutputVoltage",
    "OutputFrequency", "MasterSlave", "MaxChargeCityCurrent",
    "LowVoltReturnCity", "HighVoltReturnBat", "BatChargePriority",
    "AlarmControl", "BacklightCtrl", "PowerShutdownAlarm",
    "OverloadUseCityPower", "ParaMaxDischgCurr", "ParaMaxChgCurr",
    "RecoverThresholdVolt", "SolarPowerBalance", "ACOutputMode",
    "LiBatMaterial", "CellSerialLifePO4", "CellSerialLiNMC", "EQEnable",
    "EQVoltageSet", "EQTimeSet", "EQTimeoutSet", "EQIntervalSet",
    "EQActivateSet", "ChargeTimeSet", "CloseChargeTimeSet",
    "LVoltOpenGen", "HVoltCloseGen", "SOCBackUTL", "SOCBackBat",
    "SOCBackGen", "SOCCloseGen", "SOCCutoff", "BuzzerEn", "SGradeSOC",
    "AGradeSOC", "BGradeSOC", "ManualSocket", "ControlSocket",
    "RecoverThresholdSOC", "LiBatProtocolType", "GenRateWatt",
    "GEBalanceEn", "GenWorkStart1_L", "GenWorkStart1_H", "GenWorkEnd1_L",
    "GenWorkEnd1_H", "GenWorkStart2_L", "GenWorkStart2_H", "GenWorkEnd2_L",
    "GenWorkEnd2_H", "UTIOutputStart1_L", "UTIOutputStart1_H",
    "UTIOutputEnd1_L", "UTIOutputEnd1_H", "UTIOutputStart2_L",
    "UTIOutputStart2_H", "UTIOutputEnd2_L", "UTIOutputEnd2_H",
    "VBatAGM", "VBatFlood", "VBatUserPB", "VBatAGMFloat", "VBatFloodFloat",
    "VBatLiFloat", "VBatUserPBFloat", "VBatUserLifeFloat",
    "VBatUserLiNMCFloat", "VBatAGMCutoff", "VBatFloodCutoff",
    "VBatLiCutoff", "VBatUserPBCutoff", "VBatUserLifeCutoff",
    "VBatUserLiNMCCutoff", "SOCMaxUTLChg", "VMaxUTLChg",
]

# ======================================================================
# 运行参数字段 (RunParamDef 176B, 布局与 arm_param.h 一致)
# ======================================================================
RUN_FIELDS = [
    ("SysStatus", 0, 2, "u16", ""),
    ("Vpv1", 2, 2, "u16", "0.1V"),
    ("Vpv2", 4, 2, "u16", "0.1V"),
    ("Ppv", 6, 4, "u32", "0.1W"),
    ("Buck1Curr", 10, 2, "u16", "0.1A"),
    ("Buck2Curr", 12, 2, "u16", "0.1A"),
    ("BoostTemp", 14, 2, "u16", "0.1C"),
    ("InvertTemp", 16, 2, "u16", "0.1C"),
    ("OutputWatt", 18, 4, "u32", "0.1W"),
    ("OutputVA", 22, 4, "u32", "0.1VA"),
    ("OutputCurr", 26, 2, "u16", "0.1A"),
    ("ACChrWatt", 28, 4, "u32", "0.1W"),
    ("ACChrVA", 32, 4, "u32", "0.1VA"),
    ("GridVolt", 36, 2, "u16", "0.1V"),
    ("GridFreq", 38, 2, "u16", "0.01Hz"),
    ("ACOutputVolt", 40, 2, "u16", "0.1V"),
    ("ACOutputFreq", 42, 2, "u16", "0.01Hz"),
    ("ACInWatt", 44, 4, "u32", "0.1W"),
    ("ACInVA", 48, 4, "u32", "0.1VA"),
    ("ACDisChrWatt", 52, 4, "u32", "0.1W"),
    ("ACDisChrVA", 56, 4, "u32", "0.1VA"),
    ("ACChrCurr", 60, 2, "u16", "0.1A"),
    ("BatVolt", 62, 2, "u16", "0.01V"),
    ("BatterySOC", 64, 2, "u16", "%"),
    ("BatDisChrWatt", 66, 4, "u32", "0.1W"),
    ("BatChrWatt", 70, 4, "u32", "0.1W"),
    ("BatChgCurr", 74, 2, "u16", "0.1A"),
    ("BatDischgCurr", 76, 2, "u16", "0.1A"),
    ("BatOverCharge", 78, 2, "u16", ""),
    ("BusVolt", 80, 2, "u16", "0.01V"),
    ("PvTemp", 82, 2, "u16", "0.1C"),
    ("InvCurr", 84, 2, "u16", "0.1A"),
    ("TransformerTemp", 86, 2, "u16", "0.1C"),
    ("LoadPercent", 88, 2, "u16", "0.1%"),
    ("ParaChgCurr", 90, 2, "u16", "1A"),
    ("WorkTimeTotal", 92, 4, "u32", "s"),
    ("MpptFanSpeed", 96, 2, "u16", "%"),
    ("InvFanSpeed", 98, 2, "u16", "%"),
    ("PairedSocket", 100, 2, "u16", ""),
    ("OnlineSocket", 102, 2, "u16", ""),
    ("OnSocket", 104, 2, "u16", ""),
    ("Warning", 106, 8, "u64", ""),
    ("BmsWarning", 114, 2, "u16", ""),
    ("FaultValue", 116, 4, "u32", ""),
    ("EGen_today", 120, 4, "u32", "0.1kWh"),
    ("EGen_total", 124, 4, "u32", "0.1kWh"),
    ("Epv_today", 128, 4, "u32", "0.1kWh"),
    ("Epv_total", 132, 4, "u32", "0.1kWh"),
    ("Eac_chrToday", 136, 4, "u32", "0.1kWh"),
    ("Eac_chrTotal", 140, 4, "u32", "0.1kWh"),
    ("Ebat_dischrToday", 144, 4, "u32", "0.1kWh"),
    ("Ebat_dischrTotal", 148, 4, "u32", "0.1kWh"),
    ("Ebat_chrToday", 152, 4, "u32", "0.1kWh"),
    ("Ebat_chrTotal", 156, 4, "u32", "0.1kWh"),
    ("Eac_dischrToday", 160, 4, "u32", "0.1kWh"),
    ("Eac_dischrTotal", 164, 4, "u32", "0.1kWh"),
    ("Eop_dischrToday", 168, 4, "u32", "0.1kWh"),
    ("Eop_dischrTotal", 172, 4, "u32", "0.1kWh"),
]

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


# ======================================================================
# 工具函数
# ======================================================================
def get_u16(buf, off):
    return struct.unpack_from('<H', buf, off)[0]


def get_u32(buf, off):
    return struct.unpack_from('<I', buf, off)[0]


def get_u64(buf, off):
    return struct.unpack_from('<Q', buf, off)[0]


def set_u16(buf, off, v):
    struct.pack_into('<H', buf, off, v & 0xFFFF)


def set_u32(buf, off, v):
    struct.pack_into('<I', buf, off, v & 0xFFFFFFFF)


def set_u64(buf, off, v):
    struct.pack_into('<Q', buf, off, v & 0xFFFFFFFFFFFFFFFF)


# ======================================================================
# 参数模型
# ======================================================================
class ParamModel:
    """ARM 端参数模型: 控制参数 / 可读写 / 只读 / 运行参数 + 响应逻辑"""

    RUN_PARAM_START = 0x03E8
    RUN_PARAM_SIZE = 176
    ONLYREAD_START = 0x0190
    ONLYREAD_SIZE = 52      # 0x0190-0x01A9 (SN 16B: 4×u32)
    READWITE_START = 0x00C8
    UTC_ADDR = 0x00CA

    def __init__(self, sn=None):
        # 控制参数: CollectorParamAddr.h Index_ 0x0000-0x0053 = 84 个 u16 (168B)
        self.ctrl = [0] * 84
        self._init_ctrl_defaults()
        # 可读写数据 (4B CtrlParamAlterTime) + UTC (8B)
        self.ctrl_alter_time = 0
        self.utc = 0
        # 只读信息 (52B)
        self.only_read = bytearray(self.ONLYREAD_SIZE)
        self._init_only_read(sn)
        # 运行参数 (176B)
        self.run = bytearray(self.RUN_PARAM_SIZE)
        self._init_run()
        # 动态模拟
        self.t0 = time.time()
        self.work_time = 3600.0
        self.last_frame_time = [0.0] * 8

    # ---------------- 初始化 ----------------
    def _init_ctrl_defaults(self):
        """初始化控制参数默认值 (原始值, 工程单位 = raw / scale)

        scale 对照 (与 esp32 telemetry.c s_cfg_keys 一致):
          scale=1:   整数 (枚举/开关/百分比/串数)
          scale=10:  0.1V / 0.1A
          scale=100: 0.01Hz
        """
        defaults = {
            # ---- 基本设置 ----
            0x0000: 1,     # OutputPriority: 1=太阳能优先
            0x0001: 200,   # MaxChargeCurrentAll: 20.0A (scale=10)
            0x0002: 1,     # ACInVoltRange: 1=宽电压范围
            0x0003: 100,   # BatCapacity: 100Ah
            0x0004: 1,     # BatteryType: 1=LiFePO4
            0x0005: 1,     # OverloadRestart: 1=启用
            0x0006: 1,     # HighTempRestart: 1=启用
            0x0007: 2200,  # OutputVoltage: 220.0V (scale=10)
            0x0008: 5000,  # OutputFrequency: 50.00Hz (scale=100)
            0x0009: 0,     # MasterSlave: 0=独立
            0x000A: 100,   # MaxChargeCityCurrent: 10.0A (scale=10)
            # ---- 电压阈值 (min 40V → raw 400) ----
            0x000B: 480,   # LowVoltReturnCity: 48.0V (scale=10, min 40V)
            0x000C: 560,   # HighVoltReturnBat: 56.0V (scale=10, min 40V)
            # ---- 充放电设置 ----
            0x000D: 0,     # BatChargePriority: 0=PV优先
            0x000E: 1,     # AlarmControl: 1=启用
            0x000F: 1,     # BacklightCtrl: 1=启用
            0x0010: 1,     # PowerShutdownAlarm: 1=启用
            0x0011: 1,     # OverloadUseCityPower: 1=启用
            0x0012: 1000,  # ParaMaxDischgCurr: 100.0A (scale=10)
            0x0013: 1000,  # ParaMaxChgCurr: 100.0A (scale=10)
            0x0014: 480,   # RecoverThresholdVolt: 48.0V (scale=10, min 40V)
            0x0015: 1,     # SolarPowerBalance: 1=启用
            0x0016: 0,     # ACOutputMode: 0=单相
            0x0017: 0,     # LiBatMaterial: 0=LiFePO4
            # ---- 电池串数 (min 4) ----
            0x0018: 16,    # CellSerialLifePO4: 16串 (min 4)
            0x0019: 14,    # CellSerialLiNMC: 14串 (min 4)
            # ---- 均衡设置 ----
            0x001A: 0,     # EQEnable: 0=禁用
            0x001B: 560,   # EQVoltageSet: 56.0V (scale=10, min 40V)
            0x001C: 120,   # EQTimeSet: 120分钟
            0x001D: 60,    # EQTimeoutSet: 60分钟
            0x001E: 30,    # EQIntervalSet: 30天
            0x001F: 0,     # EQActivateSet: 0=未激活
            # ---- 充电时间 ----
            0x0020: 0,     # ChargeTimeSet: 0=禁用
            0x0021: 0,     # CloseChargeTimeSet: 0=禁用
            # ---- 发电机电压阈值 (min 40V → raw 400) ----
            0x0022: 480,   # LVoltOpenGen: 48.0V (scale=10, min 40V)
            0x0023: 560,   # HVoltCloseGen: 56.0V (scale=10, min 40V)
            # ---- SOC 设置 (互斥约束: SOCCutoff ≤ SOCBackUTL) ----
            0x0024: 50,    # SOCBackUTL: 50% (必须 ≥ SOCCutoff=20)
            0x0025: 30,    # SOCBackBat: 30%
            0x0026: 20,    # SOCBackGen: 20%
            0x0027: 80,    # SOCCloseGen: 80%
            0x0028: 20,    # SOCCutoff: 20% (≤ SOCBackUTL=50)
            0x0029: 1,     # BuzzerEn: 1=启用
            # ---- 其他 ----
            0x002A: 0,     # SGradeSOC
            0x002B: 0,     # AGradeSOC
            0x002C: 0,     # BGradeSOC
            0x002D: 0,     # ManualSocket
            0x002E: 0,     # ControlSocket
            0x002F: 30,    # RecoverThresholdSOC: 30%
            0x0030: 0,     # LiBatProtocolType: 0=默认
            0x0031: 0,     # GenRateWatt
            0x0032: 0,     # GEBalanceEn
            0x0033: 0,     # GenWorkStart1_L
            0x0034: 0,     # GenWorkStart1_H
            0x0035: 0,     # GenWorkEnd1_L
            0x0036: 0,     # GenWorkEnd1_H
            0x0037: 0,     # GenWorkStart2_L
            0x0038: 0,     # GenWorkStart2_H
            0x0039: 0,     # GenWorkEnd2_L
            0x003A: 0,     # GenWorkEnd2_H
            0x003B: 0,     # UTIOutputStart1_L
            0x003C: 0,     # UTIOutputStart1_H
            0x003D: 0,     # UTIOutputEnd1_L
            0x003E: 0,     # UTIOutputEnd1_H
            0x003F: 0,     # UTIOutputStart2_L
            0x0040: 0,     # UTIOutputStart2_H
            0x0041: 0,     # UTIOutputEnd2_L
            0x0042: 0,     # UTIOutputEnd2_H
            # ---- 电池电压参数 (scale=100, 0.01V) ----
            0x0043: 2700,  # VBatAGM: 27.00V
            0x0044: 2800,  # VBatFlood: 28.00V
            0x0045: 2700,  # VBatUserPB: 27.00V
            0x0046: 2700,  # VBatAGMFloat: 27.00V
            0x0047: 2700,  # VBatFloodFloat: 27.00V
            0x0048: 2700,  # VBatLiFloat: 27.00V
            0x0049: 2700,  # VBatUserPBFloat: 27.00V
            0x004A: 2700,  # VBatUserLifeFloat: 27.00V
            0x004B: 2700,  # VBatUserLiNMCFloat: 27.00V
            0x004C: 2100,  # VBatAGMCutoff: 21.00V
            0x004D: 2100,  # VBatFloodCutoff: 21.00V
            0x004E: 2100,  # VBatLiCutoff: 21.00V
            0x004F: 2100,  # VBatUserPBCutoff: 21.00V
            0x0050: 2100,  # VBatUserLifeCutoff: 21.00V
            0x0051: 2100,  # VBatUserLiNMCCutoff: 21.00V
            0x0052: 0,     # SOCMaxUTLChg
            0x0053: 0,     # VMaxUTLChg
        }
        for addr, v in defaults.items():
            if 0 <= addr < len(self.ctrl):
                self.ctrl[addr] = v

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

    def _init_run(self):
        t = time.time()
        set_u16(self.run, 0, 0x0004)                 # SysStatus: Charge
        set_u16(self.run, 2, 3600)                   # Vpv1 360.0V
        set_u16(self.run, 4, 3580)                   # Vpv2 358.0V
        set_u32(self.run, 6, 30000)                  # Ppv 3000.0W
        set_u16(self.run, 14, 450)                   # BoostTemp 45.0C
        set_u16(self.run, 16, 420)                   # InvertTemp 42.0C
        set_u32(self.run, 18, 25000)                 # OutputWatt 2500.0W
        set_u32(self.run, 22, 26000)                 # OutputVA
        set_u16(self.run, 26, 113)                   # OutputCurr 11.3A
        set_u32(self.run, 28, 5000)                  # ACChrWatt 500.0W
        set_u32(self.run, 32, 5200)                  # ACChrVA
        set_u16(self.run, 36, 2205)                  # GridVolt 220.5V
        set_u16(self.run, 38, 5000)                  # GridFreq 50.00Hz
        set_u16(self.run, 40, 2205)                  # ACOutputVolt
        set_u16(self.run, 42, 5000)                  # ACOutputFreq
        set_u32(self.run, 44, 5000)                  # ACInWatt
        set_u32(self.run, 48, 5200)                  # ACInVA
        set_u32(self.run, 52, 5000)                  # ACDisChrWatt
        set_u32(self.run, 56, 5200)                  # ACDisChrVA
        set_u16(self.run, 60, 23)                    # ACChrCurr 2.3A
        set_u16(self.run, 62, 5120)                  # BatVolt 51.20V
        set_u16(self.run, 64, 80)                    # BatterySOC 80%
        set_u32(self.run, 66, 3000)                  # BatDisChrWatt
        set_u32(self.run, 70, 3000)                  # BatChrWatt
        set_u16(self.run, 74, 58)                    # BatChgCurr 5.8A
        set_u16(self.run, 76, 0)                     # BatDischgCurr
        set_u16(self.run, 80, 41000)                 # BusVolt 410.00V
        set_u16(self.run, 88, 500)                   # LoadPercent 50.0%
        set_u32(self.run, 92, 0)                     # WorkTimeTotal
        set_u64(self.run, 106, 0)                    # Warning
        set_u32(self.run, 116, 0)                    # FaultValue
        set_u32(self.run, 120, 1200)                 # EGen_today 120.0kWh
        set_u32(self.run, 124, 12000)                # EGen_total
        set_u32(self.run, 128, 1000)                 # Epv_today
        set_u32(self.run, 132, 9000)                 # Epv_total
        set_u32(self.run, 136, 200)                  # Eac_chrToday
        set_u32(self.run, 140, 3000)                 # Eac_chrTotal
        set_u32(self.run, 144, 300)                  # Ebat_dischrToday
        set_u32(self.run, 148, 4000)                 # Ebat_dischrTotal
        set_u32(self.run, 152, 200)                  # Ebat_chrToday
        set_u32(self.run, 156, 2000)                 # Ebat_chrTotal
        set_u32(self.run, 160, 50)                   # Eac_dischrToday
        set_u32(self.run, 164, 500)                  # Eac_dischrTotal
        set_u32(self.run, 168, 30)                   # Eop_dischrToday
        set_u32(self.run, 172, 400)                  # Eop_dischrTotal

    # ---------------- 动态模拟 ----------------
    def tick(self):
        """每次读运行参数前更新 (正弦波动)"""
        t = time.time() - self.t0
        s1 = 0.5 + 0.5 * math.sin(t * 0.15)
        s2 = 0.5 + 0.5 * math.sin(t * 0.11 + 1.7)
        s3 = 0.5 + 0.5 * math.sin(t * 0.19 + 3.1)

        set_u16(self.run, 2, int(3600 + 200 * s1))       # Vpv1
        set_u16(self.run, 4, int(3580 + 150 * s2))       # Vpv2
        set_u32(self.run, 6, int(30000 + 8000 * s1))     # Ppv
        set_u16(self.run, 14, int(450 + 30 * s1))        # BoostTemp
        set_u16(self.run, 16, int(420 + 25 * s2))        # InvertTemp
        set_u32(self.run, 18, int(25000 + 5000 * s1))    # OutputWatt
        set_u32(self.run, 22, int(26000 + 5200 * s1))    # OutputVA
        set_u16(self.run, 26, int(113 + 18 * s1))        # OutputCurr
        set_u16(self.run, 36, int(2205 + 6 * s1))        # GridVolt
        set_u16(self.run, 40, int(2205 + 6 * s2))        # ACOutputVolt
        set_u16(self.run, 62, int(5120 + 8 * s1))        # BatVolt
        set_u16(self.run, 64, int(80 + 1.5 * s3))        # SOC
        set_u32(self.run, 66, int(3000 + 400 * s1))      # BatDisChrWatt
        set_u32(self.run, 70, int(3000 + 400 * s1))      # BatChrWatt
        set_u16(self.run, 74, int(58 + 6 * s1))          # BatChgCurr
        set_u16(self.run, 88, int(500 + 80 * s1))        # LoadPercent
        self.work_time += 1.0
        set_u32(self.run, 92, int(self.work_time))       # WorkTimeTotal
        # 能量缓慢增长
        for off in (120, 128, 136):
            set_u32(self.run, off, get_u32(self.run, off) + 1)
        for off in (124, 132, 140):
            set_u32(self.run, off, get_u32(self.run, off) + 1)

    # ---------------- 响应逻辑 ----------------
    def handle(self, cmd, param1, param2, data):
        """处理一帧, 返回响应帧字节 (已加密+PPP 封装) 或 None (无响应)"""
        resp = self._handle(cmd, param1, param2, data)
        if resp is None:
            return None
        r_cmd, r_p1, r_p2, r_data = resp
        return P.send_frame(r_cmd, r_p1, r_p2, r_data,
                            rand=random.getrandbits(32))

    def _handle(self, cmd, param1, param2, data):
        now = time.time()
        # 保活/帧统计
        self.last_frame_time[cmd & 7] = now

        if cmd == P.CMD_READ_CTRLPARAM:      # 0x01 读控制参数
            if param1 % 2 == 1 or param2 == 0 or (param1 + param2) > 84:
                return None
            n = param2
            buf = bytearray(n)
            for i in range(n):
                buf[i] = (self.ctrl[(param1 + i) // 2] >> (8 * ((param1 + i) % 2))) & 0xFF
            return (cmd, param1, param2, bytes(buf))

        if cmd == P.CMD_WRITE_CTRLPARAM:     # 0x02 写控制参数
            n = param2 >> 1
            if n == 0 or param1 + n > len(self.ctrl) or len(data) < n * 2:
                return None
            for i in range(n):
                self.ctrl[param1 + i] = struct.unpack_from('<H', data, i * 2)[0]
            self.ctrl_alter_time = int(time.time()) & 0xFFFFFFFF
            return (cmd, 0, 1, bytes([0]))

        if cmd == P.CMD_READ_RUNPARAM:       # 0x03 读运行参数
            self.tick()
            off = param1 - self.RUN_PARAM_START
            if off < 0 or off + param2 > self.RUN_PARAM_SIZE:
                return None
            return (cmd, param1, param2, bytes(self.run[off:off + param2]))

        if cmd == P.CMD_READ_R_W_DATA:       # 0x04 读可读写数据
            if param1 == self.READWITE_START:
                return (cmd, param1, 4, struct.pack('<I', self.ctrl_alter_time))
            if param1 == self.UTC_ADDR:
                return (cmd, param1, 8, struct.pack('<II', self.utc & 0xFFFFFFFF,
                                                    (self.utc >> 32) & 0xFFFFFFFF))
            return None

        if cmd == P.CMD_WRITE_R_W_DATA:      # 0x05 写可读写数据 (UTC 校时)
            if param1 == self.UTC_ADDR and len(data) >= 4:
                old = self.utc
                self.utc = struct.unpack('<I', data[:4])[0]
                if self.utc > 1000000000:
                    drift = int(time.time()) - self.utc
                    print("[UTC] ARM 校时: 写入 %s, 与主机差 %+ds"
                          % (datetime.utcfromtimestamp(self.utc).strftime('%Y-%m-%d %H:%M:%S'),
                             drift))
                else:
                    print("[UTC] 写入异常值: %u (old=%u)" % (self.utc, old))
            return (cmd, param1, param2, bytes([0]))

        if cmd == P.CMD_READ_R_DATA:         # 0x06 读只读数据
            off = param1 - self.ONLYREAD_START
            if off < 0 or off + param2 > self.ONLYREAD_SIZE:
                return None
            return (cmd, param1, param2, bytes(self.only_read[off:off + param2]))

        if cmd == P.CMD_CTRLCMD:             # 0x07 CTRLCMD — ARM 端空实现, 无响应
            print("[WARN] CTRLCMD 空实现 (ARM 端无响应), 忽略")
            return None

        if cmd == P.CMD_READ_TEST_RUNPARAM:  # 0x14 测试参数
            return (cmd, param1, 0, b'')

        print("[WARN] 未知命令 0x%02X" % cmd)
        return None

    # ---------------- 供 GUI 使用 ----------------
    def run_field_values(self):
        out = []
        for name, off, size, typ, unit in RUN_FIELDS:
            if typ == "u16":
                v = get_u16(self.run, off)
            elif typ == "u32":
                v = get_u32(self.run, off)
            else:
                v = get_u64(self.run, off)
            out.append((name, v, unit))
        return out

    def device_info_values(self):
        """返回设备信息 tab 所有字段的 (名称, 原始值字符串, 解析值, 单位, 说明)"""
        out = []
        for name, off, size, typ, unit, desc in DEVICE_INFO_FIELDS:
            if typ == "ver":
                v = get_u16(self.only_read, off)
                raw = "0x%04X" % v
                disp = "V%d.%d" % ((v >> 8) & 0xFF, v & 0xFF)
            elif typ == "str4":
                raw = bytes(self.only_read[off:off+size]).rstrip(b'\x00').decode('ascii', errors='replace')
                disp = raw
            elif typ == "sn":
                raw = onlyread_sn_to_str(self.only_read)
                disp = raw if "?" not in raw else raw + " (非法字符)"
            elif typ == "hex4":
                v = get_u32(self.only_read, off)
                raw = "0x%08X" % v
                disp = raw
            else:  # u16
                v = get_u16(self.only_read, off)
                raw = "%d" % v
                disp = raw
            out.append((name, raw, disp, unit, desc))
        return out


# ======================================================================
# 串口线程
# ======================================================================
class SerialWorker:
    """串口接收线程: PPP 解封装 → 解密 → 解析 → 参数模型处理 → 响应发送"""

    def __init__(self, model, log_cb):
        self.model = model
        self.log_cb = log_cb
        self.ser = None
        self.running = False
        self.rx = P.PPPReceiver()
        self.rx_count = 0
        self.tx_count = 0
        self.err_count = 0
        self.thread = None

    def open(self, port, baud):
        if not HAS_SERIAL:
            raise RuntimeError("pyserial 未安装: pip install pyserial")
        self.ser = serial.Serial(port, baud, timeout=0.1)
        self.rx.reset()
        self.rx_count = 0
        self.tx_count = 0
        self.err_count = 0

    def close(self):
        self.running = False
        if self.ser:
            try:
                self.ser.close()
            except Exception:
                pass
            self.ser = None

    def start(self):
        self.running = True
        self.thread = threading.Thread(target=self._loop, daemon=True)
        self.thread.start()

    def _loop(self):
        while self.running and self.ser:
            try:
                n = self.ser.in_waiting
                if n <= 0:
                    time.sleep(0.005)
                    continue
                buf = self.ser.read(n)
                for b in buf:
                    if self.rx.feed(b):
                        raw = bytes(self.rx.buf)
                        self.rx.reset()
                        self._process(raw)
            except Exception as e:
                self.err_count += 1
                self.log_cb("ERR: %s" % e, "error")
                time.sleep(0.1)

    def _process(self, raw):
        try:
            plain = P.decrypt_rx(raw)
            cmd, p1, p2, data = P.parse_frame(plain)
            self.rx_count += 1
            self.log_cb("RX %-18s addr=0x%04X len=%d" % (CMD_NAMES.get(cmd, "0x%02X" % cmd), p1, p2), "rx")
            resp = self.model.handle(cmd, p1, p2, data)
            if resp:
                self.ser.write(resp)
                self.tx_count += 1
                self.log_cb("TX 响应 %-14s addr=0x%04X len=%d" % (CMD_NAMES.get(cmd, "0x%02X" % cmd), p1, p2), "tx")
        except Exception as e:
            self.err_count += 1
            self.log_cb("RX ERR: %s" % e, "error")


# ======================================================================
# GUI
# ======================================================================
class SimulatorApp:
    def __init__(self, root):
        self.root = root
        self.root.title("CS-L10-6K2 ARM 模拟器")
        self.root.geometry("1020x680")

        self.model = ParamModel()
        self.worker = SerialWorker(self.model, self.log)

        self._build_toolbar()
        self._build_body()
        self._build_statusbar()

        # 周期刷新
        self.refresh_timer = None
        self._schedule_refresh()

        self.root.protocol("WM_DELETE_WINDOW", self._on_close)

    # ---------------- 界面构建 ----------------
    def _build_toolbar(self):
        frm = ttk.Frame(self.root, padding=4)
        frm.pack(fill="x")

        ttk.Label(frm, text="端口:").pack(side="left")
        self.port_var = tk.StringVar()
        self.port_cb = ttk.Combobox(frm, textvariable=self.port_var, width=14,
                                    state="readonly")
        self.port_cb.pack(side="left", padx=4)
        self._refresh_ports()

        ttk.Label(frm, text="波特率:").pack(side="left")
        self.baud_var = tk.StringVar(value="115200")
        baud_cb = ttk.Combobox(frm, textvariable=self.baud_var, width=8,
                               state="readonly",
                               values=["9600", "115200", "230400"])
        baud_cb.pack(side="left", padx=4)

        self.open_btn = ttk.Button(frm, text="打开串口", command=self._toggle_serial)
        self.open_btn.pack(side="left", padx=8)

        ttk.Button(frm, text="刷新端口", command=self._refresh_ports).pack(side="left")
        ttk.Button(frm, text="告警测试", command=self._alarm_test).pack(side="left", padx=8)

    def _build_body(self):
        nb = ttk.Notebook(self.root)
        nb.pack(fill="both", expand=True, padx=4, pady=4)

        # ---- 运行参数 ----
        f1 = ttk.Frame(nb)
        nb.add(f1, text="运行参数 (176B)")
        self.run_tree = ttk.Treeview(f1, columns=("value", "unit"), show="tree",
                                     height=20)
        self.run_tree.heading("#0", text="字段")
        self.run_tree.heading("value", text="原始值")
        self.run_tree.heading("unit", text="单位")
        self.run_tree.column("#0", width=180)
        self.run_tree.column("value", width=120, anchor="e")
        self.run_tree.column("unit", width=100)
        sb1 = ttk.Scrollbar(f1, orient="vertical", command=self.run_tree.yview)
        self.run_tree.configure(yscrollcommand=sb1.set)
        self.run_tree.pack(side="left", fill="both", expand=True)
        sb1.pack(side="right", fill="y")

        # ---- 控制参数 ----
        f2 = ttk.Frame(nb)
        nb.add(f2, text="控制参数 (84B)")
        top = ttk.Frame(f2)
        top.pack(fill="x", padx=4, pady=4)
        ttk.Label(top, text="命令: set_charge_limit / set_soc_low / battery_type / output_voltage ..."
                  ).pack(side="left")
        self.ctrl_tree = ttk.Treeview(f2, columns=("addr", "value"), show="tree")
        self.ctrl_tree.heading("#0", text="参数名")
        self.ctrl_tree.heading("addr", text="地址")
        self.ctrl_tree.heading("value", text="当前值")
        self.ctrl_tree.column("#0", width=220)
        self.ctrl_tree.column("addr", width=80, anchor="e")
        self.ctrl_tree.column("value", width=100, anchor="e")
        self.ctrl_tree.pack(fill="both", expand=True, padx=4, pady=4)
        for i, name in enumerate(CTRL_NAMES):
            self.ctrl_tree.insert("", "end", text=name,
                                  values=("0x%04X" % i, self.model.ctrl[i]))

        edit = ttk.Frame(f2)
        edit.pack(fill="x", padx=4, pady=4)
        ttk.Label(edit, text="选择参数 → 输入新值:").pack(side="left")
        self.ctrl_val = tk.StringVar()
        ttk.Entry(edit, textvariable=self.ctrl_val, width=12).pack(side="left", padx=4)
        ttk.Button(edit, text="写入 (模拟 ARM 生效)",
                   command=self._write_ctrl).pack(side="left")

        # ---- 设备信息 ----
        f4 = ttk.Frame(nb)
        nb.add(f4, text="设备信息")

        # 左侧: 只读字段表格
        left = ttk.Frame(f4)
        left.pack(side="left", fill="both", expand=True, padx=4, pady=4)
        ttk.Label(left, text="只读设备信息 (52B OnlyReadDataDef — 蓝色字段可编辑)",
                  font=("", 9, "bold")).pack(anchor="w")

        self.info_tree = ttk.Treeview(left, columns=("raw", "disp", "unit", "desc"),
                                      height=14)
        self.info_tree.heading("#0", text="字段名")
        self.info_tree.heading("raw", text="原始值")
        self.info_tree.heading("disp", text="解析值")
        self.info_tree.heading("unit", text="单位")
        self.info_tree.heading("desc", text="说明")
        self.info_tree.column("#0", width=150)
        self.info_tree.column("raw", width=110, anchor="e")
        self.info_tree.column("disp", width=140, anchor="e")
        self.info_tree.column("unit", width=60)
        self.info_tree.column("desc", width=220)
        sb4 = ttk.Scrollbar(left, orient="vertical", command=self.info_tree.yview)
        self.info_tree.configure(yscrollcommand=sb4.set)
        self.info_tree.pack(side="left", fill="both", expand=True)
        sb4.pack(side="right", fill="y")

        # 右侧: SN 编辑面板
        right = ttk.LabelFrame(f4, text="序列号 (SN) 快速编辑", padding=10)
        right.pack(side="right", fill="y", padx=8, pady=8)

        ttk.Label(right, text="设备序列号 (16位):").pack(anchor="w")
        self.sn_var = tk.StringVar()
        sn_entry = ttk.Entry(right, textvariable=self.sn_var, width=20, font=("", 10))
        sn_entry.pack(fill="x", pady=(4, 2))
        sn_entry.bind("<Return>", lambda e: self._write_sn())

        ttk.Label(right, text="数字: 2024010123456789\n字母: H1CNA00135000014").pack(anchor="w", pady=(0, 8))
        ttk.Separator(right, orient="horizontal").pack(fill="x", pady=4)

        ttk.Label(right, text="内部 u32 值预览:").pack(anchor="w")
        self.sn_bcd_label = ttk.Label(right, text="", font=("Consolas", 9),
                                       foreground="#006600", background="#f0fff0")
        self.sn_bcd_label.pack(fill="x", pady=2)

        ttk.Separator(right, orient="horizontal").pack(fill="x", pady=6)

        ttk.Button(right, text="写入 SN (模拟 ARM 生效)",
                   command=self._write_sn).pack(fill="x", pady=(0, 4))

        ttk.Button(right, text="刷新列表",
                   command=self._refresh_info_tab).pack(fill="x")

        ttk.Separator(right, orient="horizontal").pack(fill="x", pady=10)

        ttk.Label(right, text="SN 校验状态:").pack(anchor="w")
        self.sn_status_label = ttk.Label(right, text="", font=("", 9, "bold"))
        self.sn_status_label.pack(anchor="w")

        # ---- 日志 ----
        f3 = ttk.Frame(nb)
        nb.add(f3, text="日志")
        self.log_txt = scrolledtext.ScrolledText(f3, state="disabled", height=24)
        self.log_txt.pack(fill="both", expand=True, padx=4, pady=4)

        self.log("模拟器就绪. 打开串口后等待 ESP32 轮询.", "info")

        # 初始化设备信息 tab
        self._refresh_info_tab()

    def _build_statusbar(self):
        self.status_var = tk.StringVar(value="未连接")
        bar = ttk.Label(self.root, textvariable=self.status_var,
                        relief="sunken", anchor="w", padding=2)
        bar.pack(fill="x")

    # ---------------- 动作 ----------------
    def _refresh_ports(self):
        if HAS_SERIAL:
            ports = [p.device for p in serial.tools.list_ports.comports()]
        else:
            ports = []
            self.log("pyserial 未安装, 串口不可用: pip install pyserial", "error")
        self.port_cb["values"] = ports
        if ports and not self.port_var.get():
            self.port_var.set(ports[0])

    def _toggle_serial(self):
        if self.worker.ser:
            self.worker.close()
            self.open_btn.config(text="打开串口")
            self.status_var.set("未连接")
            self.log("串口已关闭", "info")
            return
        port = self.port_var.get()
        baud = int(self.baud_var.get())
        try:
            self.worker.open(port, baud)
        except Exception as e:
            messagebox.showerror("串口错误", str(e))
            return
        self.worker.start()
        self.open_btn.config(text="关闭串口")
        self.status_var.set("%s @ %d" % (port, baud))
        self.log("串口已打开: %s @ %d" % (port, baud), "info")

    def _write_ctrl(self):
        sel = self.ctrl_tree.selection()
        if not sel:
            messagebox.showinfo("提示", "先在列表中选择一个控制参数")
            return
        idx = self.ctrl_tree.index(sel[0])
        try:
            v = int(self.ctrl_val.get())
        except ValueError:
            messagebox.showerror("错误", "值必须是整数 (原始值)")
            return
        if not (0 <= v <= 0xFFFF):
            messagebox.showerror("错误", "值范围 0..65535")
            return
        self.model.ctrl[idx] = v
        self.ctrl_tree.set(sel[0], "value", v)
        self.model.ctrl_alter_time = int(time.time()) & 0xFFFFFFFF
        self.log("控制参数 [0x%04X] %s = %d (模拟 ARM 已生效)"
                 % (idx, CTRL_NAMES[idx], v), "info")

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

    def _refresh_info_tab(self):
        """刷新设备信息 tab 的表格和 SN 控件"""
        # 刷新表格
        for item in self.info_tree.get_children():
            self.info_tree.delete(item)
        for name, raw, disp, unit, desc in self.model.device_info_values():
            self.info_tree.insert("", "end", text=name,
                                  values=(raw, disp, unit, desc))

        # 刷新 SN 输入框和预览
        sn_str = onlyread_sn_to_str(self.model.only_read)
        sn_vals = [get_u32(self.model.only_read, 10 + i * 4) for i in range(4)]
        sn_preview = "SN1-4(u32): 0x%08X 0x%08X 0x%08X 0x%08X" % tuple(sn_vals)
        self.sn_bcd_label.config(text=sn_preview)
        if "?" not in sn_str:
            self.sn_var.set(sn_str)
            self.sn_status_label.config(text="SN 合法 ✓", foreground="green")
        else:
            self.sn_var.set("")
            self.sn_status_label.config(text="含非法字符，无法解析", foreground="red")

    def _alarm_test(self):
        """切换 Warning 位模拟告警变化"""
        cur = get_u64(self.model.run, 106)
        new = 0 if cur else (1 << 5)
        set_u64(self.model.run, 106, new)
        self.log("告警测试: Warning 0x%X -> 0x%X (变化后 ESP32 应发 alarm)"
                 % (cur, new), "info")

    # ---------------- 日志/刷新 ----------------
    def log(self, msg, kind="info"):
        ts = datetime.now().strftime("%H:%M:%S")
        self.log_txt.configure(state="normal")
        self.log_txt.insert("end", "[%s] %s\n" % (ts, msg))
        self.log_txt.configure(state="disabled")
        self.log_txt.see("end")

    def _schedule_refresh(self):
        try:
            for item in self.run_tree.get_children():
                self.run_tree.delete(item)
            for name, v, unit in self.model.run_field_values():
                self.run_tree.insert("", "end", text=name, values=(v, unit))
            self.status_var.set(
                self.status_var.get().split(" | ")[0] +
                " | RX=%d TX=%d ERR=%d" % (self.worker.rx_count,
                                           self.worker.tx_count,
                                           self.worker.err_count))
        except Exception:
            pass
        self.refresh_timer = self.root.after(1000, self._schedule_refresh)

    def _on_close(self):
        if self.refresh_timer:
            self.root.after_cancel(self.refresh_timer)
        self.worker.close()
        self.root.destroy()


# ======================================================================
# headless 模式
# ======================================================================
def headless_run(port, baud, sn=None):
    if not HAS_SERIAL:
        print("错误: pyserial 未安装 (pip install pyserial)")
        return 1
    model = ParamModel(sn)

    def log_cb(msg, kind=""):
        print("[%s] %s" % (datetime.now().strftime("%H:%M:%S"), msg))

    worker = SerialWorker(model, log_cb)
    print("打开 %s @ %d ..." % (port, baud))
    worker.open(port, baud)
    worker.start()
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        print("\n退出")
    finally:
        worker.close()
    return 0


def parse_sn(s):
    """--sn: 1..16 位 ASCII SN 字符串, 如 '2024010123456789'"""
    s = s.strip()
    if not s or len(s) > 16:
        raise argparse.ArgumentTypeError("--sn 需要 1..16 个字符")
    return s


def main():
    ap = argparse.ArgumentParser(description="CS-L10-6K2 ARM 端模拟器")
    ap.add_argument("--headless", action="store_true", help="命令行模式")
    ap.add_argument("--port", default=None, help="串口 (headless 必填)")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--sn", type=parse_sn, default=None,
                    help="覆盖只读信息 SN (1..16 位 ASCII), 如 2024010123456789")
    args = ap.parse_args()

    if args.headless:
        if not args.port:
            print("headless 模式需要 --port COMx")
            return 2
        if args.sn:
            print("SN 覆盖: %s" % args.sn)
        return headless_run(args.port, args.baud, args.sn)

    if not HAS_TK:
        print("tkinter 不可用, 请使用 --headless 模式")
        return 2

    root = tk.Tk()
    SimulatorApp(root)
    root.mainloop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
