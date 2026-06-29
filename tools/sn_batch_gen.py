#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
辰烁 SN 批量生产工具 — CS-SN-STD-001 V1.1
===========================================
输入生产参数，批量生成序列号 + 二维码 PNG
每台设备一个 PNG 文件，可直接打印到铭牌。

校验位算法: CRC-16 MODBUS (Poly=0x8005, Init=0xFFFF, RefIn=true, RefOut=true)
  1. 对前15位字符串的ASCII字节做 CRC-16 MODBUS 运算
  2. 取低8位 (crc & 0xFF)
  3. 对36取模 (lo % 36)
  4. 映射为36进制字符 (0-9→'0'-'9', 10-35→'A'-'Z')

用法:
    python sn_batch_gen.py

输出:
    qr_out/H1CNA0013500001O.png   ← 二维码 (标准 QR, 微信/QQ 可扫描)
    qr_out/manifest.csv           ← CSV 清单 (全部 SN 明细)
"""

import csv, os, sys
from datetime import datetime

# 确保能找到同目录的 sn_encoder
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sn_encoder import *

# 尝试导入二维码库
try:
    from PIL import Image, ImageDraw, ImageFont
    import qrcode
    HAS_QR = True
except ImportError:
    HAS_QR = False
    print("[警告] 缺少二维码库，将只生成 CSV 清单。")
    print("       安装: pip install pillow qrcode")
    print()


# ==================== 配置区 ====================
CONFIG = {
    # ---- 序列号参数 ----
    "manufacturer":   "H1",      # 厂商码 (第1-2位): H1=总部1号工厂
    "country":        "DE",      # 国家码 (第3-4位): ISO 3166-1 alpha-2
    "grade":          "A",       # 客户等级 (第5位): A=战略大客户
    "customer_start":  1,        # 客户流水号起始 (第6-8位, 左侧补零到3位)
    "year":           2025,      # 生产年份
    "month":          10,        # 生产月份 (1-12)
    "batch_size":     100,       # 本批次数量
    "seq_start":      1,         # 生产序列起始号 (第11-15位, 左侧补零到5位)

    # ---- 二维码参数 ----
    "output_dir":     "qr_out",  # 输出目录
}


def main():
    cfg = CONFIG

    # 推导字段
    grade     = cfg["grade"].upper()
    country   = cfg["country"].upper()
    mfr       = cfg["manufacturer"].upper()
    year_code = year_encode(cfg["year"])
    month_code = month_encode(cfg["month"])
    date_code = year_code + month_code
    batch_size = cfg["batch_size"]
    seq_start = cfg["seq_start"]
    cust_start = cfg["customer_start"]

    # 创建输出目录
    os.makedirs(cfg["output_dir"], exist_ok=True)

    # 打开 CSV
    csv_path = os.path.join(cfg["output_dir"], "manifest.csv")
    csv_f = open(csv_path, "w", newline="", encoding="utf-8-sig")
    writer = csv.writer(csv_f)
    writer.writerow(["序号", "序列号", "可读格式", "厂商", "国家", "客户编码",
                     "客户等级", "客户流水", "年份", "月份", "生产序列", "校验位"])

    count = 0
    for i in range(batch_size):
        seq_num = seq_start + i
        cust_num = cust_start + i

        seq_str    = str(seq_num).zfill(5)
        cust_str   = str(cust_num).zfill(3)
        customer   = grade + cust_str

        sn = generate(mfr, country, customer, date_code, seq_str)
        if not sn:
            print(f"  ❌ 生成失败: seq={seq_str}")
            continue

        pf = parse(sn)
        readable = printable(sn)

        writer.writerow([
            count + 1,
            sn,
            readable,
            pf.get("manufacturer", ""),
            pf.get("country", ""),
            pf.get("customer", ""),
            pf.get("grade_name", ""),
            cust_num,
            pf.get("year", ""),
            pf.get("month", ""),
            seq_num,
            pf.get("check_digit", ""),
        ])

        # 生成二维码图片
        if HAS_QR:
            img_path = os.path.join(cfg["output_dir"], f"{sn}.png")
            make_qr(sn, readable, img_path)

        count += 1

    csv_f.close()

    print(f"=" * 55)
    print(f"  批次生产完成！")
    print(f"  数量:        {count} 台")
    print(f"  SN 范围:     {generate(mfr, country, grade + str(cust_start).zfill(3), date_code, str(seq_start).zfill(5))}")
    print(f"            ~  {generate(mfr, country, grade + str(cust_start+batch_size-1).zfill(3), date_code, str(seq_start+batch_size-1).zfill(5))}")
    print(f"  厂商:        {mfr}")
    print(f"  国家/地区:   {country}")
    print(f"  客户等级:    {grade} ({GRADE_NAMES.get(grade, '')})")
    print(f"  生产日期:    {cfg['year']}年{cfg['month']}月")
    print(f"  年份码:      {year_code}  月份码: {month_code}")
    print(f"  校验算法:    CRC-16 MODBUS mod 36")
    print(f"  输出目录:    {os.path.abspath(cfg['output_dir'])}")
    if HAS_QR:
        print(f"  二维码:      标准 QR Code (仅 SN)")
    print(f"  清单文件:    {os.path.abspath(csv_path)}")
    print(f"=" * 55)


def make_qr(sn, readable, path):
    """生成简洁美观的 SN 二维码铭牌图"""

    def _load_font(sizes, prefer=("msyh.ttc", "msyhbd.ttc")):
        for sz in sizes:
            for name in prefer + ("simhei.ttf", "consola.ttf", "cour.ttf"):
                try: return ImageFont.truetype(name, sz)
                except: pass
        return ImageFont.load_default()

    # 字号配置
    font_sn_big   = _load_font((48, 44, 40, 36), prefer=("consola.ttf", "cour.ttf"))  # SN 大字
    font_sn_small = _load_font((32, 28, 24), prefer=("consola.ttf", "cour.ttf"))  # SN 小字
    font_label    = _load_font((18, 16))  # 标签小字

    # 选择合适大小的字体
    dummy = Image.new("RGB", (1, 1))
    d = ImageDraw.Draw(dummy)
    bbox = d.textbbox((0, 0), readable, font=font_sn_big)
    if bbox[2] > 280:  # 如果太长，用小字体
        font_sn = font_sn_small
    else:
        font_sn = font_sn_big

    bbox = d.textbbox((0, 0), readable, font=font_sn)
    sn_w = bbox[2]
    sn_h = bbox[3]

    # QR Code — 编码内容仅 SN
    qr_content = f"SN:{sn}"
    qr = qrcode.QRCode(
        version=2,  # 固定版本，保证二维码大小一致
        error_correction=qrcode.constants.ERROR_CORRECT_H,  # 高纠错级别
        box_size=8,
        border=2,
    )
    qr.add_data(qr_content)
    qr.make(fit=True)
    qr_img = qr.make_image(fill_color="black", back_color="white").convert("RGB")

    # 二维码尺寸：280px（保证足够的扫码距离）
    qr_size = 280

    # 画布尺寸
    # 左侧：SN 文字区（根据实际文字宽度）
    text_area_w = sn_w + 20  # 实际文字宽度 + 边距
    text_area_h = sn_h + 20 + 30  # SN + 间距 + "SCAN" 提示

    # 右侧：二维码
    qr_area_w = qr_size + 40  # 二维码 + 边距

    # 总画布
    pad = 24
    img_w = max(pad * 2 + text_area_w + 50 + qr_area_w, 600)  # 最小宽度 600
    img_h = pad * 2 + max(text_area_h, qr_size + 20)

    # 颜色
    BORDER_COLOR = (45, 55, 72)      # 深蓝灰
    BG_COLOR = (255, 255, 255)       # 白色背景
    TEXT_COLOR = (26, 32, 44)       # 深灰文字
    LABEL_COLOR = (113, 128, 150)   # 浅灰标签
    QR_BG_COLOR = (248, 250, 252)   # 二维码背景

    # 创建图像
    img = Image.new("RGB", (img_w, img_h), BG_COLOR)
    draw = ImageDraw.Draw(img)

    # 顶部装饰条（品牌色）
    draw.rectangle([0, 0, img_w - 1, 6], fill=BORDER_COLOR)

    # 左侧：SN 区域
    left_x = pad
    left_y = (img_h - text_area_h) // 2

    # "SN:" 标签
    draw.text((left_x, left_y), "SN:", fill=LABEL_COLOR, font=font_label)

    # SN 序列号（大字）
    sn_y = left_y + 18 + 10
    draw.text((left_x, sn_y), readable, fill=TEXT_COLOR, font=font_sn)

    # "SCAN WITH CAMERA" 提示
    hint_y = sn_y + sn_h + 15
    draw.text((left_x, hint_y), "SCAN WITH CAMERA", fill=LABEL_COLOR, font=font_label)

    # 分隔线（放在二维码左侧，确保不遮挡文字）
    line_x = pad + sn_w + 30  # 根据实际文字宽度定位
    line_y1 = pad + 20
    line_y2 = img_h - pad - 20
    draw.line([(line_x, line_y1), (line_x, line_y2)], fill=(226, 232, 240), width=1)

    # 右侧：二维码区域
    qr_x = line_x + 25
    qr_y = (img_h - qr_size) // 2

    # 二维码白色背景（带轻微阴影效果）
    draw.rounded_rectangle(
        [qr_x - 6, qr_y - 6, qr_x + qr_size + 6, qr_y + qr_size + 6],
        radius=12, fill=(220, 228, 236)
    )
    draw.rounded_rectangle(
        [qr_x - 3, qr_y - 3, qr_x + qr_size + 3, qr_y + qr_size + 3],
        radius=10, fill=QR_BG_COLOR
    )

    # 绘制二维码
    qr_resized = qr_img.resize((qr_size, qr_size), Image.LANCZOS)
    img.paste(qr_resized, (qr_x, qr_y))

    # 底部装饰条
    draw.rectangle([0, img_h - 6, img_w - 1, img_h - 1], fill=BORDER_COLOR)

    # 保存
    img.save(path, dpi=(300, 300), quality=95)
    print(f"  [{sn}] → {path}")


# ==================== 校验位算法说明 ====================
def print_algorithm():
    """打印校验位算法详解 (供铭牌文档使用)"""
    print("""
╔══════════════════════════════════════════════════════════╗
║         辰烁 SN 校验位算法 — CRC-16 MODBUS               ║
╠══════════════════════════════════════════════════════════╣
║                                                          ║
║  输入:  SN 前 15 位字符串 (如 "H1CNA0013500001")         ║
║  输出:  1 位校验字符 (0-9 或 A-Z)                        ║
║                                                          ║
║  步骤 1:  CRC-16 MODBUS 计算                             ║
║           多项式: 0x8005                                 ║
║           初始值: 0xFFFF                                 ║
║           RefIn:  true (输入字节反转)                    ║
║           RefOut: true (输出CRC反转)                     ║
║           输入:   15 字节 ASCII 字符串                   ║
║           输出:   16-bit CRC 值 (0x0000-0xFFFF)          ║
║                                                          ║
║  步骤 2:  取低8位 (crc & 0xFF)                           ║
║                                                          ║
║  步骤 3:  对36取模 (lo % 36) → 0~35                     ║
║                                                          ║
║  步骤 4:  映射为字符                                      ║
║            0-9  → '0'-'9'                                ║
║           10-35 → 'A'-'Z'                                ║
║                                                          ║
╚══════════════════════════════════════════════════════════╝
""")


if __name__ == '__main__':
    print_algorithm()
    main()
