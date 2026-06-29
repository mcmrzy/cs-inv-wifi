#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ARM 逆变器终端模拟器 — 启动入口
===============================
双击运行，自动启动 GUI 界面。

依赖: pip install pyserial

文件结构:
  arm_inverter_simulator.py       ← 本文件（启动入口）
  arm_inverter_simulator_part1.py ← 协议层 + 参数模型 + 串口管理
  arm_inverter_simulator_gui.py   ← GUI 界面
"""

import sys
import os

# 将当前目录加入搜索路径
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from arm_inverter_simulator_gui import main

if __name__ == '__main__':
    main()
