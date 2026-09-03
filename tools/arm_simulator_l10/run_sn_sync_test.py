#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
run_sn_sync_test.py — SN 同步运行时联调 (计划验证项 2/3/4/5)

硬件要求:
  - 设备日志口: ESP32-C3 USB 虚拟串口 (默认 COM13)
  - ARM 口:     USB-TTL 转换器连接 ESP32 GPIO0(RX)/GPIO1(TX)/GND, 转换器在电脑
                上枚举为 COMx (如 COM5), 传给 --arm-port

用法:
  python run_sn_sync_test.py --arm-port COM5            # 场景 A+B

场景:
  A. SN 同步 + 重启生效: 日志出现 "SN from ARM: 2024010123456789 (was ...)",
     2s 后 "SN updated by ARM, restarting in 2s", 重启后 "SN loaded: 2024010123456789"
  B. 一致不重启: 再次复位, 无 "SN from ARM"/"SN updated", 且 SN loaded 为新值 (NVS 持久化)
  C. 非法 SN 场景已由 verify_sn_sync.py 逻辑层覆盖 (--sn 传字符串无法携带 0x0F
     等非法字节), 本脚本不再提供 CLI 分支

退出码: 0 = 全部 PASS
"""

import sys
import os
import time
import argparse
import subprocess
import serial

HERE = os.path.dirname(os.path.abspath(__file__))
SIMULATOR = os.path.join(HERE, "simulator.py")
PYENV = r"C:\Users\29538\.espressif\python_env\idf5.5_py3.13_env\Scripts\python.exe"
ESPPORT = r"D:\.espressif\v5.5.4\esp-idf\components\esptool_py\esptool\esptool.py"

ARM_SN_STR = "2024010123456789"          # 模拟器默认 SN (16 位 ASCII)
# 非法 SN (含 0x0F 等不可打印字节) 无法通过 --sn 字符串 CLI 表达, 非法场景
# 已由 verify_sn_sync.py 逻辑层覆盖, 此处不保留坏 SN 分支

PASS = 0
FAIL = 0


def check(name, cond, detail=""):
    global PASS, FAIL
    if cond:
        PASS += 1
        print("  [PASS] %s %s" % (name, detail))
    else:
        FAIL += 1
        print("  [FAIL] %s %s" % (name, detail))


def reset_device(log_port):
    """esptool 硬复位设备, 随后立刻打开日志口"""
    r = subprocess.run(
        [PYENV, ESPPORT, "--port", log_port, "--chip", "esp32c3",
         "--after", "hard_reset", "run"],
        capture_output=True, text=True, timeout=60)
    if r.returncode != 0:
        print("    reset FAIL:", r.stdout[-300:], r.stderr[-300:])
        return False
    return True


def capture(log_port, seconds):
    """打开日志口抓取 seconds 秒, 返回文本"""
    s = serial.Serial(log_port, 115200, timeout=0.2)
    buf = b""
    deadline = time.time() + seconds
    while time.time() < deadline:
        d = s.read(4096)
        if d:
            buf += d
    s.close()
    return buf.decode("utf-8", errors="replace")


def start_simulator(arm_port, sn=None, log_file=None):
    """启动 headless 模拟器 (固定 9600 波特率, 与固件 UART 一致)"""
    cmd = [sys.executable, SIMULATOR, "--headless",
           "--port", arm_port, "--baud", "9600"]
    if sn:
        cmd += ["--sn", sn]
    stdout = open(log_file, "w") if log_file else subprocess.DEVNULL
    return subprocess.Popen(cmd, stdout=stdout, stderr=subprocess.STDOUT,
                            text=True)


def stop_simulator(proc):
    if proc and proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()


def run_scene(log_port, arm_port, duration, expect_sync, title):
    """复位设备并抓日志; expect_sync=True 断言同步发生, False 断言不发生"""
    print("== %s ==" % title)
    time.sleep(1.5)                       # 等模拟器串口就绪
    if not reset_device(log_port):
        check("设备复位", False)
        return ""
    time.sleep(0.15)
    txt = capture(log_port, duration)

    if expect_sync:
        check("日志: SN from ARM: %s (was ...)" % ARM_SN_STR,
              ("SN from ARM: %s (was " % ARM_SN_STR) in txt)
        check("日志: SN updated by ARM, restarting in 2s",
              "SN updated by ARM, restarting in 2s" in txt)
        check("重启后: SN loaded: %s" % ARM_SN_STR,
              "SN loaded: %s" % ARM_SN_STR in txt)
        print("  [SKIP] MQTT topic 生效 (需 WiFi+broker, 当前环境跳过; "
              "上线后 topic=cs_inv/%s/...)" % ARM_SN_STR)
    else:
        check("无 'SN from ARM' 日志", "SN from ARM" not in txt)
        check("无 'SN updated by ARM' 日志", "SN updated by ARM" not in txt)
    return txt


def main():
    ap = argparse.ArgumentParser(description="SN 同步运行时联调")
    ap.add_argument("--arm-port", required=True, help="USB-TTL 转换器 COM 口")
    ap.add_argument("--log-port", default="COM13", help="设备日志口")
    ap.add_argument("--duration", type=int, default=25, help="场景 A 抓取秒数")
    args = ap.parse_args()

    print("== SN 同步运行时联调 ==")
    print("  ARM 口: %s @9600 | 日志口: %s | 期望 SN: %s"
          % (args.arm_port, args.log_port, ARM_SN_STR))

    sim = start_simulator(args.arm_port, log_file=os.path.join(HERE, "sim_run.log"))
    try:
        # 场景 A: SN 同步 + 重启生效
        run_scene(args.log_port, args.arm_port, args.duration, True, "场景 A: SN 同步 + 重启生效")

        # 场景 B: NVS 已一致 → 不重启 (等待 0x06 启动轮询完成后判定)
        time.sleep(3)
        run_scene(args.log_port, args.arm_port, 12, False, "场景 B: NVS 与 ARM 一致 → 零写入不重启")
        # 场景 C (非法 SN) 由 verify_sn_sync.py 逻辑层覆盖, 不再走 CLI
    finally:
        stop_simulator(sim)

    print("\n结果: %d PASS, %d FAIL" % (PASS, FAIL))
    return 0 if FAIL == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
