#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
uart_loopback_test.py — CS-L10-6K2 协议联调测试脚本

用途:
  1. 无 --port: 自测模式 (send_frame → PPP → recv_frame 往返验证)
  2. 有 --port: 与 arm_simulator_l10 或真实 ARM 串口联调:
     - 5s 周期读取运行参数 (READ_RUNPARAM 0x03E8, 176B) 并打印关键字段
     - 读取只读信息 (READ_R_DATA 0x0190, 52B)
     - 写控制参数测试 (WRITE_CTRLPARAM 0x0001)
     - 写 UTC 校时测试 (WRITE_R_W_DATA 0x00CA)

用法:
  python uart_loopback_test.py                  # 自测
  python uart_loopback_test.py --port COM5 --baud 115200 --count 5
  python uart_loopback_test.py --port COM5 --write-ctrl 0x0001 200
"""

import sys
import os
import time
import struct
import argparse

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import protocol as P

try:
    import serial
    HAS_SERIAL = True
except ImportError:
    HAS_SERIAL = False

# ---- 参数布局 (与 arm_param.h / simulator.py 一致) ----
RUN_PARAM_START = 0x03E8
RUN_PARAM_SIZE = 176
ONLYREAD_START = 0x0190
ONLYREAD_SIZE = 52
UTC_ADDR = 0x00CA

KEY_FIELDS = [
    ("SysStatus", 0, "H"), ("Vpv1", 2, "H"), ("Vpv2", 4, "H"),
    ("Ppv", 6, "I"), ("BoostTemp", 14, "H"), ("InvertTemp", 16, "H"),
    ("OutputWatt", 18, "I"), ("OutputVA", 22, "I"), ("OutputCurr", 26, "H"),
    ("ACChrWatt", 28, "I"), ("GridVolt", 36, "H"), ("GridFreq", 38, "H"),
    ("ACOutputVolt", 40, "H"), ("ACOutputFreq", 42, "H"),
    ("ACChrCurr", 60, "H"), ("BatVolt", 62, "H"), ("BatterySOC", 64, "H"),
    ("BatChrWatt", 70, "I"), ("BatChgCurr", 74, "H"),
    ("BusVolt", 80, "H"), ("LoadPercent", 88, "H"),
    ("Warning", 106, "Q"), ("FaultValue", 116, "I"),
    ("EGen_today", 120, "I"), ("EGen_total", 124, "I"),
    ("Epv_today", 128, "I"), ("Epv_total", 132, "I"),
]


def parse_run_param(data):
    """解析 176B 运行参数 → 关键字段 dict"""
    out = {}
    for name, off, fmt in KEY_FIELDS:
        out[name] = struct.unpack_from("<" + fmt, data, off)[0]
    return out


def print_run_param(p):
    print("  SysStatus=0x%04X  Vpv1=%.1fV  Vpv2=%.1fV  Ppv=%.1fW"
          % (p["SysStatus"], p["Vpv1"] / 10, p["Vpv2"] / 10, p["Ppv"] / 10))
    print("  Output: %.1fW / %.1fVA / %.1fA  Load=%.1f%%"
          % (p["OutputWatt"] / 10, p["OutputVA"] / 10, p["OutputCurr"] / 10,
             p["LoadPercent"] / 10))
    print("  Grid: %.1fV / %.2fHz   AC Out: %.1fV / %.2fHz"
          % (p["GridVolt"] / 10, p["GridFreq"] / 100,
             p["ACOutputVolt"] / 10, p["ACOutputFreq"] / 100))
    print("  Bat: %.2fV  SOC=%d%%  P=%.1fW  ChgCurr=%.1fA  Bus=%.2fV"
          % (p["BatVolt"] / 100, p["BatterySOC"], p["BatChrWatt"] / 10,
             p["BatChgCurr"] / 10, p["BusVolt"] / 100))
    print("  Warning=0x%X  Fault=0x%X  EGen=%.1f/%.1f kWh"
          % (p["Warning"], p["FaultValue"], p["EGen_today"] / 10,
             p["EGen_total"] / 10))


class ArmLink:
    """串口联调客户端 (与 ESP32 端 arm_poll 行为一致)"""

    def __init__(self, port, baud=115200, timeout=1.0):
        if not HAS_SERIAL:
            raise RuntimeError("pyserial 未安装: pip install pyserial")
        self.ser = serial.Serial(port, baud, timeout=timeout)
        self.rx = P.PPPReceiver()
        self.seq = 0

    def close(self):
        self.ser.close()

    def _drain(self):
        """清空接收缓冲"""
        self.rx.reset()
        n = self.ser.in_waiting
        if n:
            self.ser.read(n)

    def exchange(self, cmd, param1, param2, data=b'', wait_ms=1000):
        """发送一帧并等待响应, 返回 (cmd, param1, param2, data)"""
        self._drain()
        frame = P.send_frame(cmd, param1, param2, data,
                             rand=0x12340000 | (self.seq & 0xFFFF))
        self.seq += 1
        self.ser.write(frame)
        deadline = time.time() + wait_ms / 1000.0
        while time.time() < deadline:
            n = self.ser.in_waiting
            if n:
                buf = self.ser.read(n)
                for b in buf:
                    if self.rx.feed(b):
                        raw = bytes(self.rx.buf)
                        self.rx.reset()
                        return P.parse_frame(P.decrypt_rx(raw))
            time.sleep(0.01)
        raise TimeoutError("no response (cmd=0x%02X addr=0x%04X)" % (cmd, param1))

    def read_run_param(self):
        cmd, p1, p2, data = self.exchange(P.CMD_READ_RUNPARAM,
                                          RUN_PARAM_START, RUN_PARAM_SIZE)
        assert p2 == RUN_PARAM_SIZE and len(data) == RUN_PARAM_SIZE
        return parse_run_param(data)

    def read_info(self):
        cmd, p1, p2, data = self.exchange(P.CMD_READ_R_DATA,
                                          ONLYREAD_START, ONLYREAD_SIZE)
        assert p2 == ONLYREAD_SIZE and len(data) == ONLYREAD_SIZE
        sn = bytes(data[10:26]).rstrip(b'\x00').decode('ascii', errors='replace')
        print("  ARM FW=%u DSP FW=%u HW=%u SN=%s Rate=%uW/%uVA"
              % (struct.unpack_from("<H", data, 0)[0],
                 struct.unpack_from("<H", data, 2)[0],
                 struct.unpack_from("<H", data, 8)[0], sn,
                 struct.unpack_from("<H", data, 30)[0],
                 struct.unpack_from("<H", data, 32)[0]))
        return data

    def write_ctrl(self, addr, value):
        """写控制参数: addr=索引, value=原始值"""
        data = struct.pack("<H", value & 0xFFFF)
        cmd, p1, p2, resp = self.exchange(P.CMD_WRITE_CTRLPARAM, addr, 2, data)
        err = resp[0] if resp else -1
        print("  WRITE_CTRLPARAM 0x%04X=%u -> err=%d (期望 err=0)"
              % (addr, value, err))
        assert err == 0
        return err

    def write_utc(self, utc=None):
        if utc is None:
            utc = int(time.time())
        data = struct.pack("<I", utc & 0xFFFFFFFF)
        cmd, p1, p2, resp = self.exchange(P.CMD_WRITE_R_W_DATA,
                                          UTC_ADDR, 4, data)
        err = resp[0] if resp else -1
        print("  WRITE_R_W_DATA 0x%04X=%u -> err=%d" % (UTC_ADDR, utc, err))
        assert err == 0
        return err


def self_test():
    """无串口自测: 协议往返 + 边界"""
    print("=== 自测模式: send_frame → recv_frame 往返 ===")
    passed = 0
    cases = [
        (P.CMD_READ_RUNPARAM, 0x03E8, 176),
        (P.CMD_READ_R_DATA, 0x0190, 52),
        (P.CMD_READ_CTRLPARAM, 0x0000, 84),
        (P.CMD_READ_R_W_DATA, 0x00CA, 8),
        (P.CMD_READ_RUNPARAM, 0x03E8, 2),
        (P.CMD_READ_RUNPARAM, 0x03E8, 6),
        (P.CMD_READ_RUNPARAM, 0x03E8, 8),
    ]
    for cmd, p1, p2 in cases:
        # 数据区长度由 param2 决定: 填充到 param2 字节
        payload = (b"\x01\x02\x03\x04" * ((p2 + 3) // 4))[:p2]
        frame = P.send_frame(cmd, p1, p2, payload, rand=0xA5A55A5A)
        got = P.recv_frame(frame)
        assert got[0] == cmd and got[1] == p1 and got[2] == p2
        assert got[3] == payload
        passed += 1
        print("  [PASS] cmd=0x%02X p1=0x%04X p2=%d" % (cmd, p1, p2))

    # 写命令帧 (含转义字节) 往返
    data = bytes(range(176))
    frame = P.send_frame(P.CMD_WRITE_CTRLPARAM, 0x0000, 176, data,
                         rand=0x11223344)
    got = P.recv_frame(frame)
    assert got[3] == data
    passed += 1
    print("  [PASS] WRITE_CTRLPARAM 176B with escape bytes")

    # PPP 非法转义 (0x7D 后跟非法字节) 应拒绝
    rx = P.PPPReceiver()
    good = P.send_frame(P.CMD_READ_RUNPARAM, 0x03E8, 176, b'', rand=1)
    bad = good.replace(b"\x7D", b"\x7D\x42", 1)  # 0x42 非 5E/5D
    got_any = False
    for b in bad:
        if rx.feed(b):
            got_any = True
    assert not got_any, "非法转义序列不应产生完整帧"
    passed += 1
    print("  [PASS] PPP invalid escape rejected")

    print("=== 自测通过: %d checks ===" % passed)
    return 0


def live_test(args):
    print("=== 联调模式: %s @ %d ===" % (args.port, args.baud))
    link = ArmLink(args.port, args.baud)
    try:
        # 1. 读只读信息
        print("[info] READ_R_DATA 0x%04X:" % ONLYREAD_START)
        link.read_info()

        # 2. 写控制参数测试 (可选)
        if args.write_ctrl is not None:
            print("[ctrl] 写入控制参数 0x%04X = %u" % args.write_ctrl)
            link.write_ctrl(args.write_ctrl[0], args.write_ctrl[1])

        # 3. UTC 校时测试 (可选)
        if args.write_utc:
            print("[utc] 写入 UTC 校时:")
            link.write_utc()

        # 4. 周期读运行参数
        count = args.count or 3
        for i in range(count):
            print("[run #%d] READ_RUNPARAM 0x%03X:" % (i + 1, RUN_PARAM_START))
            p = link.read_run_param()
            print_run_param(p)
            if i < count - 1:
                time.sleep(args.interval)
    finally:
        link.close()
    print("=== 联调完成 ===")
    return 0


def main():
    ap = argparse.ArgumentParser(description="CS-L10-6K2 协议联调脚本")
    ap.add_argument("--port", default=None, help="串口 (缺省=自测模式)")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--count", type=int, default=3, help="运行参数读取次数")
    ap.add_argument("--interval", type=float, default=5.0, help="读取间隔秒")
    ap.add_argument("--write-ctrl", nargs=2, type=int, metavar=("ADDR", "VALUE"),
                    default=None, help="写控制参数测试 (如 1 200 = 0x0001 20.0A)")
    ap.add_argument("--write-utc", action="store_true", help="写 UTC 校时测试")
    args = ap.parse_args()

    if not args.port:
        return self_test()
    return live_test(args)


if __name__ == "__main__":
    sys.exit(main())
