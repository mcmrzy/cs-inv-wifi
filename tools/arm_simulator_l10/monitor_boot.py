#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
monitor_boot.py — 复位设备并抓取启动日志 (绕过 idf.py monitor 的 MSys 检查)

用法: python monitor_boot.py [秒数]
  1. esptool --after hard_reset run  复位设备
  2. 打开 COM13 (USB-Serial-JTAG, 115200) 抓日志, 默认 25 秒
"""

import sys
import time
import subprocess
import serial

PORT = "COM13"
BAUD = 115200
PYENV = r"C:\Users\29538\.espressif\python_env\idf5.5_py3.13_env\Scripts\python.exe"
ESPPORT = r"D:\.espressif\v5.5.4\esp-idf\components\esptool_py\esptool\esptool.py"

DURATION = int(sys.argv[1]) if len(sys.argv) > 1 else 25

# 1. 复位设备 (USB-Serial-JTAG 硬复位)
print(f"== reset device via esptool (port {PORT}) ==", flush=True)
r = subprocess.run(
    [PYENV, ESPPORT, "--port", PORT, "--chip", "esp32c3",
     "--after", "hard_reset", "run"],
    capture_output=True, text=True, timeout=60)
print("reset:", "OK" if r.returncode == 0 else f"FAIL rc={r.returncode}")
if r.returncode != 0:
    print(r.stdout[-500:], r.stderr[-500:])

time.sleep(0.5)

# 2. 抓日志
print(f"== capture {DURATION}s from {PORT} ==", flush=True)
log = []
s = serial.Serial(PORT, BAUD, timeout=0.3)
deadline = time.time() + DURATION
while time.time() < deadline:
    data = s.read(4096)
    if data:
        try:
            text = data.decode("utf-8", errors="replace")
        except Exception:
            text = repr(data)
        log.append(text)
        print(text, end="", flush=True)
s.close()
print(f"\n== captured {sum(len(x) for x in log)} bytes ==")
