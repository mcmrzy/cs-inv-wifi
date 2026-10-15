#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
辰烁铭牌生成工具 — CS-L10-6K2 逆变器
=====================================
生成铭牌图片：SN + 二维码 + PIN 码

PIN 算法: HMAC-SHA256(PRODUCT_SECRET, SN) mod 1000000
  PRODUCT_SECRET = "CS_INV_L10" + "_2026_" + "SECRET"
  PIN 为 6 位十进制数（前导零保留）

用法:
    python nameplate_gen.py                    # 交互式输入
    python nameplate_gen.py --sn H1CNA00135000014  # 单个 SN
    python nameplate_gen.py --batch 100        # 批量生成

输出:
    nameplate_out/H1CNA0013500001O.png   ← 铭牌图片
    nameplate_out/manifest.csv           ← CSV 清单
"""

import argparse
import csv
import hmac
import hashlib
import os
import sys
from datetime import datetime

# 确保能找到同目录的 sn_encoder
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sn_encoder import *

# 尝试导入图像库
try:
    from PIL import Image, ImageDraw, ImageFont
    import qrcode
    HAS_QR = True
except ImportError:
    HAS_QR = False
    print("[警告] 缺少图像库，将只生成 CSV 清单。")
    print("       安装: pip install pillow qrcode")
    print()


# ==================== PIN 算法 ====================
# PRODUCT_SECRET 分片拼接（与固件一致）
S1 = "CS_INV_L10"
S2 = "_2026_"
S3 = "SECRET"

def get_product_secret() -> bytes:
    """获取产品线密钥"""
    return (S1 + S2 + S3).encode('utf-8')

def compute_pin(sn: str) -> int:
    """
    计算 PIN 码
    PIN = HMAC-SHA256(PRODUCT_SECRET, SN) mod 1000000
    返回 0~999999
    """
    secret = get_product_secret()
    sn_bytes = sn.encode('utf-8')
    digest = hmac.new(secret, sn_bytes, hashlib.sha256).digest()
    # 取前 3 字节 → 24 位整数 → mod 1000000
    val = (digest[0] << 16) | (digest[1] << 8) | digest[2]
    return val % 1000000

def pin_to_str(pin: int) -> str:
    """PIN → 6 位字符串（前导零保留）"""
    return f"{pin:06d}"

def verify_pin(sn: str, pin_str: str) -> bool:
    """验证 PIN 是否正确"""
    expected = compute_pin(sn)
    return pin_to_str(expected) == pin_str


# ==================== 铭牌图片生成 ====================
# 默认二维码网址（可通过 --url 参数覆盖）
DEFAULT_QR_URL = "http://192.168.8.57/bind?sn={sn}&pin={pin}"  # 本地联调：局域网 docker nginx；上线后改回 https://jiuxiaoyw.online/bind

def make_nameplate(sn: str, pin_str: str, path: str, qr_url: str = None):
    """生成铭牌图片：SN + 二维码 + PIN（统一风格，居中布局）"""

    # 二维码内容
    if qr_url is None:
        qr_url = DEFAULT_QR_URL
    qr_content = qr_url.format(sn=sn, pin=pin_str)

    def _load_font(sizes, prefer=("msyh.ttc", "msyhbd.ttc")):
        for sz in sizes:
            for name in prefer + ("simhei.ttf", "consola.ttf", "cour.ttf"):
                try: return ImageFont.truetype(name, sz)
                except: pass
        return ImageFont.load_default()

    # SN 和 PIN 使用相同字号
    font_val_big   = _load_font((48, 44, 40, 36), prefer=("consola.ttf", "cour.ttf"))
    font_val_small = _load_font((32, 28, 24), prefer=("consola.ttf", "cour.ttf"))
    font_label     = _load_font((18, 16))

    readable = printable(sn)

    dummy = Image.new("RGB", (1, 1))
    d = ImageDraw.Draw(dummy)

    # SN 字体选择
    bbox = d.textbbox((0, 0), readable, font=font_val_big)
    font_val = font_val_small if bbox[2] > 280 else font_val_big

    # SN 尺寸
    bbox = d.textbbox((0, 0), readable, font=font_val)
    sn_w = bbox[2]
    sn_h = bbox[3]

    # PIN 尺寸（用同一字体）
    bbox = d.textbbox((0, 0), pin_str, font=font_val)
    pin_w = bbox[2]
    pin_h = bbox[3]

    # 二维码（智能链接：已安装 App 则跳转 App，否则跳转下载页）
    qr = qrcode.QRCode(
        version=4,  # URL 较长，需要更大版本
        error_correction=qrcode.constants.ERROR_CORRECT_H,
        box_size=8, border=2,
    )
    qr.add_data(qr_content)
    qr.make(fit=True)
    qr_img = qr.make_image(fill_color="black", back_color="white").convert("RGB")
    qr_size = 280

    # 固定画布尺寸（确保内容居中）
    img_w = 800
    img_h = 400

    # 颜色（SN 和 PIN 统一）
    BORDER_COLOR = (45, 55, 72)
    BG_COLOR = (255, 255, 255)
    TEXT_COLOR = (26, 32, 44)
    LABEL_COLOR = (113, 128, 150)
    QR_BG_COLOR = (248, 250, 252)

    img = Image.new("RGB", (img_w, img_h), BG_COLOR)
    draw = ImageDraw.Draw(img)

    # 顶部装饰条
    draw.rectangle([0, 0, img_w - 1, 6], fill=BORDER_COLOR)

    # 计算标签宽度（用实际文字宽度）
    label_sn = "SN:"
    label_pin = "PIN:"
    bbox_sn = d.textbbox((0, 0), label_sn, font=font_label)
    bbox_pin = d.textbbox((0, 0), label_pin, font=font_label)
    label_w = max(bbox_sn[2], bbox_pin[2]) + 12  # 实际宽度 + 间距

    # 文字区域宽度
    val_w = max(sn_w, pin_w)
    text_area_w = label_w + val_w

    # 计算各区域尺寸
    gap = 40  # 文字区与二维码之间间距
    qr_area_w = qr_size + 20  # 二维码 + 边距

    # 总内容宽度
    total_w = text_area_w + gap + qr_area_w

    # 计算起始 X（居中）
    start_x = (img_w - total_w) // 2

    # 文字区域（左侧）
    text_x = start_x
    text_area_h = sn_h + 24 + pin_h + 36 + 20  # SN + 间距 + PIN + 间距 + 提示
    text_y = (img_h - text_area_h) // 2

    # SN
    draw.text((text_x, text_y), label_sn, fill=LABEL_COLOR, font=font_label)
    sn_x = text_x + label_w
    sn_y = text_y + 20  # 标签下方 20px
    draw.text((sn_x, sn_y), readable, fill=TEXT_COLOR, font=font_val)

    # PIN（与 SN 同样风格）
    pin_y = sn_y + sn_h + 24  # SN 下方 24px
    draw.text((text_x, pin_y), label_pin, fill=LABEL_COLOR, font=font_label)
    pin_x = text_x + label_w
    pin_val_y = pin_y + 20  # 标签下方 20px
    draw.text((pin_x, pin_val_y), pin_str, fill=TEXT_COLOR, font=font_val)

    # 提示（PIN 值下方 36px）
    hint_y = pin_val_y + pin_h + 36
    draw.text((text_x, hint_y), "SCAN WITH CAMERA", fill=LABEL_COLOR, font=font_label)

    # 分隔线
    sep_x = text_x + text_area_w + gap // 2
    sep_y1 = 30
    sep_y2 = img_h - 30
    draw.line([(sep_x, sep_y1), (sep_x, sep_y2)], fill=(226, 232, 240), width=1)

    # 二维码（右侧，居中）
    qr_x = sep_x + gap // 2 + 10
    qr_y = (img_h - qr_size) // 2

    draw.rounded_rectangle(
        [qr_x - 6, qr_y - 6, qr_x + qr_size + 6, qr_y + qr_size + 6],
        radius=12, fill=(220, 228, 236))
    draw.rounded_rectangle(
        [qr_x - 3, qr_y - 3, qr_x + qr_size + 3, qr_y + qr_size + 3],
        radius=10, fill=QR_BG_COLOR)

    qr_resized = qr_img.resize((qr_size, qr_size), Image.LANCZOS)
    img.paste(qr_resized, (qr_x, qr_y))

    # 底部装饰条
    draw.rectangle([0, img_h - 6, img_w - 1, img_h - 1], fill=BORDER_COLOR)

    # 保存
    img.save(path, dpi=(300, 300), quality=95)
    print(f"  [{sn}] PIN={pin_str} → {path}")


# ==================== 主程序 ====================
def main():
    parser = argparse.ArgumentParser(description="辰烁铭牌生成工具")
    parser.add_argument("--sn", type=str, help="单个 SN 号")
    parser.add_argument("--batch", type=int, help="批量生成数量")
    parser.add_argument("--output", type=str, default="nameplate_out", help="输出目录")
    parser.add_argument("--verify", type=str, nargs=2, metavar=("SN", "PIN"), help="验证 PIN")
    parser.add_argument("--url", type=str, default=None,
                        help=f"二维码网址模板，用 {{sn}} 和 {{pin}} 占位（默认: {DEFAULT_QR_URL}）")
    args = parser.parse_args()

    # 验证模式
    if args.verify:
        sn, pin = args.verify
        ok = verify_pin(sn, pin)
        print(f"SN={sn} PIN={pin} → {'✓ 正确' if ok else '✗ 错误'}")
        return

    os.makedirs(args.output, exist_ok=True)
    qr_url = args.url  # None 时使用默认值

    # 单个 SN
    if args.sn:
        sn = args.sn.upper()
        if not validate(sn):
            print(f"✗ SN 格式错误: {sn}")
            return
        pin = compute_pin(sn)
        pin_str = pin_to_str(pin)
        if HAS_QR:
            path = os.path.join(args.output, f"{sn}.png")
            make_nameplate(sn, pin_str, path, qr_url)
        print(f"\n  SN:  {sn}")
        print(f"  PIN: {pin_str}")
        return

    # 批量生成
    if args.batch:
        # 使用默认配置
        cfg = {
            "manufacturer": "H1",
            "country": "DE",
            "grade": "A",
            "customer_start": 1,
            "year": 2025,
            "month": 10,
            "batch_size": args.batch,
            "seq_start": 1,
        }

        grade = cfg["grade"].upper()
        country = cfg["country"].upper()
        mfr = cfg["manufacturer"].upper()
        year_code = year_encode(cfg["year"])
        month_code = month_encode(cfg["month"])
        date_code = year_code + month_code

        csv_path = os.path.join(args.output, "manifest.csv")
        csv_f = open(csv_path, "w", newline="", encoding="utf-8-sig")
        writer = csv.writer(csv_f)
        writer.writerow(["序号", "SN", "PIN", "厂商", "国家", "客户等级", "年份", "月份", "序列号"])

        for i in range(cfg["batch_size"]):
            seq_num = cfg["seq_start"] + i
            cust_num = cfg["customer_start"]  # 客户流水号固定，不递增
            seq_str = str(seq_num).zfill(5)
            cust_str = str(cust_num).zfill(3)
            customer = grade + cust_str

            sn = generate(mfr, country, customer, date_code, seq_str)
            if not sn:
                continue

            pin = compute_pin(sn)
            pin_str = pin_to_str(pin)

            pf = parse(sn)
            writer.writerow([
                i + 1, sn, pin_str,
                pf.get("manufacturer", ""), pf.get("country", ""),
                pf.get("grade_name", ""), pf.get("year", ""),
                pf.get("month", ""), seq_num,
            ])

            if HAS_QR:
                path = os.path.join(args.output, f"{sn}.png")
                make_nameplate(sn, pin_str, path, qr_url)

        csv_f.close()
        print(f"\n  批量生成完成: {cfg['batch_size']} 台")
        print(f"  输出目录: {os.path.abspath(args.output)}")
        return

    # 交互式
    print("=" * 55)
    print("  辰烁铭牌生成工具 — CS-L10-6K2")
    print("=" * 55)
    print()
    print("  PIN 算法: HMAC-SHA256(PRODUCT_SECRET, SN) mod 1000000")
    print("  PRODUCT_SECRET: CS_INV_L10_2026_SECRET")
    print()

    while True:
        sn = input("  输入 SN 号 (q 退出): ").strip().upper()
        if sn == 'Q':
            break
        if not sn:
            continue

        if not validate(sn):
            print(f"  ✗ SN 格式错误: {sn}")
            print()
            continue

        pin = compute_pin(sn)
        pin_str = pin_to_str(pin)

        if HAS_QR:
            path = os.path.join(args.output, f"{sn}.png")
            make_nameplate(sn, pin_str, path, qr_url)

        print(f"  SN:  {sn}")
        print(f"  PIN: {pin_str}")
        print()


# ==================== 测试向量 ====================
def print_test_vectors():
    """打印测试向量（供固件验证）"""
    print("\n  测试向量（固件 ble_ct_compute_pin 验证用）:")
    print("  " + "-" * 50)
    test_sns = [
        "H1CNA00135000014",
        "H1CNA00135000015",
        "H1CNA00135000016",
        "H1DEA0012A00088X",  # 注意：这个 SN 可能格式不对
    ]
    for sn in test_sns:
        if validate(sn):
            pin = compute_pin(sn)
            print(f"  SN={sn} → PIN={pin_to_str(pin)}")
        else:
            # 即使格式不对，也计算 PIN（用于测试）
            pin = compute_pin(sn)
            print(f"  SN={sn} → PIN={pin_to_str(pin)} (SN 格式无效，仅测试)")
    print("  " + "-" * 50)


if __name__ == '__main__':
    print_test_vectors()
    main()
