#!/usr/bin/env python3
"""根据 SN 计算设备 PIN 码（HMAC-SHA256 算法，与固件 ble_ct_pin.c 一致）"""
import sys
import hmac
import hashlib

PRODUCT_SECRET = "CS_INV_L10_2026_SECRET"

def compute_pin(sn: str) -> str:
    digest = hmac.new(PRODUCT_SECRET.encode(), sn.encode(), hashlib.sha256).digest()
    pin = ((digest[0] << 16) | (digest[1] << 8) | digest[2]) % 1000000
    return f"{pin:06d}"

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(f"用法: python {sys.argv[0]} <SN> [SN ...]")
        sys.exit(1)
    for sn in sys.argv[1:]:
        print(f"{sn} → {compute_pin(sn)}")
