#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
PIN 测试向量生成器 — 供固件 ble_ct_compute_pin() 验证
"""

import hmac
import hashlib

# PRODUCT_SECRET（与固件一致）
S1 = "CS_INV_L10"
S2 = "_2026_"
S3 = "SECRET"
SECRET = (S1 + S2 + S3).encode('utf-8')

def compute_pin(sn: str) -> int:
    sn_bytes = sn.encode('utf-8')
    digest = hmac.new(SECRET, sn_bytes, hashlib.sha256).digest()
    val = (digest[0] << 16) | (digest[1] << 8) | digest[2]
    return val % 1000000

if __name__ == '__main__':
    print("=" * 60)
    print("  固件 PIN 算法测试向量")
    print("  PRODUCT_SECRET = 'CS_INV_L10_2026_SECRET'")
    print("=" * 60)
    print()

    test_sns = [
        "H1CNA00135000014",
        "H1CNA00135000015",
        "H1CNA00135000016",
        "H1DEA0012A00088X",
    ]

    for sn in test_sns:
        pin = compute_pin(sn)
        print(f"  SN = {sn}")
        print(f"  PIN = {pin:06d}")
        print()

    # 固件测试代码示例
    print("  === 固件测试代码 ===")
    print()
    print("  // 在 ble_ct_start() 中添加:")
    print("  ESP_LOGI(TAG, \"PIN test: SN=H1CNA00135000014 → %06u\",")
    print("           ble_ct_compute_pin(\"H1CNA00135000014\"));")
    print()

    # 输出预期结果
    print("  === 预期输出 ===")
    for sn in test_sns:
        pin = compute_pin(sn)
        print(f"  SN={sn} → PIN={pin:06d}")
