#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""测试生成一张示例图片"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sn_encoder import generate, printable
from sn_batch_gen import make_qr

# 生成一个测试 SN
sn = generate("H1", "CN", "A001", "2A", "00001")
readable = printable(sn)

print(f"生成测试 SN: {sn}")
print(f"可读格式: {readable}")

# 生成图片
output_path = os.path.join(os.path.dirname(__file__), "test_qr.png")
make_qr(sn, readable, output_path)
print(f"图片已保存到: {output_path}")
