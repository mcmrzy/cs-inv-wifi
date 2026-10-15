#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
SN + PIN 铭牌生产 GUI 工具 — CS-L10-6K2 逆变器
===============================================
结构化编辑序列号各字段，批量生成 + 预览 + 导出铭牌（SN + 二维码 + PIN）

PIN 算法: HMAC-SHA256(PRODUCT_SECRET, SN) mod 1000000
校验位算法: CRC-16 MODBUS
"""

import sys, os, csv, threading, hmac, hashlib
from datetime import datetime

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sn_encoder import *

try:
    import customtkinter as ctk
    from customtkinter import *
    HAS_CTK = True
except ImportError:
    HAS_CTK = False
    print("请安装 customtkinter: pip install customtkinter")

try:
    from PIL import Image, ImageDraw, ImageFont, ImageTk
    import qrcode
    HAS_QR = True
except ImportError:
    HAS_QR = False
    print("[警告] 缺少二维码库: pip install pillow qrcode")

# ==================== PIN 算法 ====================
S1 = "CS_INV_L10"
S2 = "_2026_"
S3 = "SECRET"

def get_product_secret() -> bytes:
    return (S1 + S2 + S3).encode('utf-8')

def compute_pin(sn: str) -> int:
    secret = get_product_secret()
    sn_bytes = sn.encode('utf-8')
    digest = hmac.new(secret, sn_bytes, hashlib.sha256).digest()
    val = (digest[0] << 16) | (digest[1] << 8) | digest[2]
    return val % 1000000

def pin_to_str(pin: int) -> str:
    return f"{pin:06d}"

# ==================== 主题配色 ====================
C = {
    'bg': '#1a1b26',
    'card': '#24283b',
    'card2': '#1f2335',
    'accent': '#3b4261',
    'highlight': '#7aa2f7',
    't': '#c0caf5',
    't2': '#a9b1d6',
    't3': '#565f89',
    'border': '#3b4261',
    'success': '#9ece6a',
    'warning': '#e0af68',
    'error': '#f7768e',
    'info': '#7dcfff',
    'pin': '#f7768e',  # PIN 红色
}

def FF(family='Microsoft YaHei', size=12, weight='normal'):
    return CTkFont(family=family, size=size, weight=weight)


class NameplateGUI:
    def __init__(self):
        ctk.set_appearance_mode("dark")
        ctk.set_default_color_theme("dark-blue")

        self.root = CTk()
        self.root.title("SN + PIN 铭牌生产工具 — 辰烁科技 CS-L10-6K2")
        self.root.geometry("1200x800")
        self.root.configure(fg_color=C['bg'])

        self.sn_list = []
        self.preview_img = None
        self._build_ui()

    def _build_ui(self):
        # 顶部标题栏
        header = CTkFrame(self.root, fg_color=C['card'], height=55, corner_radius=0)
        header.pack(fill='x')
        header.pack_propagate(False)
        CTkLabel(header, text="🏭 SN + PIN 铭牌生产工具", font=FF(size=18, weight='bold'),
                 text_color=C['t']).pack(side='left', padx=20, pady=10)
        CTkLabel(header, text="CS-L10-6K2  ·  HMAC-SHA256 PIN  ·  CRC-16 MODBUS 校验",
                 text_color=C['t3'], font=FF(size=11)).pack(side='left', padx=(0, 20))

        # 主内容区
        main = CTkFrame(self.root, fg_color=C['bg'])
        main.pack(fill='both', expand=True, padx=15, pady=10)

        # ---- 左侧: 参数输入 ----
        left = CTkFrame(main, fg_color=C['card'], corner_radius=12, width=380)
        left.pack(side='left', fill='both', padx=(0, 12))
        left.pack_propagate(False)

        CTkLabel(left, text="生产参数", font=FF(size=14, weight='bold'),
                 text_color=C['t']).pack(anchor='w', padx=15, pady=(12, 8))

        self.entries = {}

        # 厂商码
        self._add_field(left, "厂商码 (第1-2位)", "manufacturer", "H1",
                        tooltip="1位厂商类型(H/O/S)+1位厂区序号")

        # 国家码
        row = CTkFrame(left, fg_color='transparent')
        row.pack(fill='x', padx=15, pady=3)
        CTkLabel(row, text="国家/地区 (第3-4位)", width=150, anchor='w',
                 text_color=C['t2'], font=FF(size=11)).pack(side='left')
        self.country_combo = CTkComboBox(
            row, values=[f"{c[0]} - {c[1]}" for c in COMMON_COUNTRIES],
            width=190, fg_color=C['accent'], border_color=C['border'],
            button_color=C['highlight'], font=FF(family='Consolas', size=11))
        self.country_combo.set("DE - 德国")
        self.country_combo.pack(side='left')

        # 客户等级 + 流水号
        row2 = CTkFrame(left, fg_color='transparent')
        row2.pack(fill='x', padx=15, pady=3)
        CTkLabel(row2, text="客户等级 (第5位)", width=150, anchor='w',
                 text_color=C['t2'], font=FF(size=11)).pack(side='left')
        grade_vals = [f"{g} - {n}" for g, n in [
            ('A', '战略大客户'), ('B', '核心分销商'), ('C', '安装商'),
            ('D', '授权EPC'), ('E', '二级代理'), ('P', '项目'), ('X', '零售')
        ]]
        self.grade_combo = CTkComboBox(row2, values=grade_vals, width=190,
                                       fg_color=C['accent'], border_color=C['border'],
                                       button_color=C['highlight'], font=FF(family='Consolas', size=11))
        self.grade_combo.set("A - 战略大客户")
        self.grade_combo.pack(side='left')

        self._add_field(left, "客户流水起始 (第6-8位)", "cust_start", "1")

        # 生产年份 + 月份
        row3 = CTkFrame(left, fg_color='transparent')
        row3.pack(fill='x', padx=15, pady=3)
        CTkLabel(row3, text="生产年份", width=60, anchor='w',
                 text_color=C['t2'], font=FF(size=11)).pack(side='left')
        self.year_entry = CTkEntry(row3, width=65, height=28, fg_color=C['accent'],
                                   border_color=C['border'], text_color=C['t'],
                                   font=FF(family='Consolas', size=12))
        self.year_entry.insert(0, "2025")
        self.year_entry.pack(side='left', padx=(0, 12))

        CTkLabel(row3, text="月份", width=30, anchor='w',
                 text_color=C['t2'], font=FF(size=11)).pack(side='left')
        self.month_combo = CTkComboBox(row3, values=[str(m) for m in range(1, 13)],
                                       width=60, fg_color=C['accent'], border_color=C['border'],
                                       button_color=C['highlight'], font=FF(family='Consolas', size=11))
        self.month_combo.set("10")
        self.month_combo.pack(side='left')

        self._add_field(left, "生产序列起始 (第11-15位)", "seq_start", "1")
        self._add_field(left, "生产数量 (台)", "batch_size", "100")

        # 二维码网址
        CTkFrame(left, fg_color=C['border'], height=1).pack(fill='x', padx=10, pady=8)
        CTkLabel(left, text="二维码网址", font=FF(size=12, weight='bold'),
                 text_color=C['t']).pack(anchor='w', padx=15, pady=(0, 4))
        CTkLabel(left, text="用 {sn} 和 {pin} 作占位符",
                 text_color=C['t3'], font=FF(size=10)).pack(anchor='w', padx=15)
        self.qr_url_entry = CTkEntry(left, width=340, height=28, fg_color=C['accent'],
                                     border_color=C['border'], text_color=C['t'],
                                     font=FF(family='Consolas', size=10))
        self.qr_url_entry.insert(0, "https://jiuxiaoyw.online/bind?sn={sn}&pin={pin}")
        self.qr_url_entry.pack(padx=15, pady=4)

        # 预览
        self.date_preview = CTkLabel(left, text="年份码: ?  月份码: ?",
                                     text_color=C['t3'], font=FF(family='Consolas', size=11))
        self.date_preview.pack(anchor='w', padx=15, pady=(2, 0))

        self.range_preview = CTkLabel(left, text="",
                                      text_color=C['t2'], font=FF(family='Consolas', size=10))
        self.range_preview.pack(anchor='w', padx=15, pady=(2, 0))

        # PIN 算法说明
        CTkFrame(left, fg_color=C['border'], height=1).pack(fill='x', padx=10, pady=8)
        CTkLabel(left, text="PIN 算法", font=FF(size=12, weight='bold'),
                 text_color=C['t']).pack(anchor='w', padx=15, pady=(0, 4))
        CTkLabel(left, text="HMAC-SHA256(PRODUCT_SECRET, SN)\n取前3字节 mod 1000000",
                 text_color=C['t3'], font=FF(family='Consolas', size=10)).pack(anchor='w', padx=15)

        # 按钮
        CTkFrame(left, fg_color=C['border'], height=1).pack(fill='x', padx=10, pady=10)

        self.gen_btn = CTkButton(left, text="🔄 生成并预览",
                                 fg_color=C['highlight'], hover_color=C['accent'],
                                 height=38, font=FF(size=13, weight='bold'),
                                 command=self._generate)
        self.gen_btn.pack(fill='x', padx=15, pady=5)

        self.export_btn = CTkButton(left, text="📦 导出铭牌 + CSV",
                                    fg_color=C['success'], hover_color=C['accent'],
                                    height=38, font=FF(size=13, weight='bold'),
                                    command=self._export)
        self.export_btn.pack(fill='x', padx=15, pady=5)
        self.export_btn.configure(state='disabled')

        # 进度条
        self.progress = CTkProgressBar(left, height=8, progress_color=C['success'],
                                       fg_color=C['accent'])
        self.progress.pack(fill='x', padx=15, pady=10)
        self.progress.set(0)

        self.status_lbl = CTkLabel(left, text="就绪", text_color=C['t3'], font=FF(size=11))
        self.status_lbl.pack(padx=15, pady=(0, 10))

        # ---- 右侧: 列表 + 预览 ----
        right = CTkFrame(main, fg_color=C['card'], corner_radius=12)
        right.pack(side='left', fill='both', expand=True)

        CTkLabel(right, text="序列号 + PIN 列表", font=FF(size=13, weight='bold'),
                 text_color=C['t']).pack(anchor='w', padx=15, pady=(12, 5))

        table_frame = CTkFrame(right, fg_color=C['card2'], corner_radius=8)
        table_frame.pack(fill='both', expand=True, padx=10, pady=(0, 8))

        self.sn_text = CTkTextbox(table_frame, fg_color='transparent',
                                  border_color=C['border'], text_color=C['t'],
                                  font=FF(family='Consolas', size=12),
                                  spacing3=2, wrap='none')
        self.sn_text.pack(fill='both', expand=True, padx=2, pady=2)
        self.sn_text.configure(state='disabled')

        # 统计行
        stats_frame = CTkFrame(right, fg_color='transparent')
        stats_frame.pack(fill='x', padx=15, pady=(0, 10))
        self.count_lbl = CTkLabel(stats_frame, text="共 0 台",
                                  text_color=C['t2'], font=FF(size=11))
        self.count_lbl.pack(side='left')

    def _add_field(self, parent, label, key, default, tooltip=""):
        row = CTkFrame(parent, fg_color='transparent')
        row.pack(fill='x', padx=15, pady=3)
        CTkLabel(row, text=label, width=150, anchor='w',
                 text_color=C['t2'], font=FF(size=11)).pack(side='left')
        e = CTkEntry(row, width=190, height=28, fg_color=C['accent'],
                     border_color=C['border'], text_color=C['t'],
                     font=FF(family='Consolas', size=12))
        e.insert(0, default)
        e.pack(side='left')
        e.bind('<KeyRelease>', lambda ev: self._update_preview())
        self.entries[key] = e

    def _update_preview(self):
        try:
            year = int(self.year_entry.get())
            month = int(self.month_combo.get())
            yc = year_encode(year)
            mc = month_encode(month)
            self.date_preview.configure(text=f"年份码: {yc}  月份码: {mc}")

            mfr = self.entries['manufacturer'].get().strip().upper()
            country_raw = self.country_combo.get().strip()
            country = country_raw[:2].upper() if country_raw else "??"
            grade_raw = self.grade_combo.get().strip()
            grade = grade_raw[0] if grade_raw else 'A'
            c1 = self.entries['cust_start'].get().strip()
            c1 = ''.join(c for c in c1 if c.isdigit())
            s1 = self.entries['seq_start'].get().strip()
            s1 = ''.join(c for c in s1 if c.isdigit())
            n_str = self.entries['batch_size'].get().strip()
            n = int(''.join(c for c in n_str if c.isdigit()) or '0')

            if mfr and country and c1 and s1 and n > 0:
                first = generate(mfr, country, grade + c1.zfill(3), yc + mc, s1.zfill(5))
                last = generate(mfr, country, grade + c1.zfill(3),
                                yc + mc, str(int(s1)+n-1).zfill(5))
                if first and last:
                    pin_first = pin_to_str(compute_pin(first))
                    pin_last = pin_to_str(compute_pin(last))
                    self.range_preview.configure(
                        text=f"范围: {first} (PIN:{pin_first}) ~ {last} (PIN:{pin_last})")
        except:
            pass

    def _generate(self):
        try:
            mfr = self.entries['manufacturer'].get().strip().upper()
            country_raw = self.country_combo.get().strip()
            country = country_raw[:2].upper() if country_raw else "??"
            grade_raw = self.grade_combo.get().strip()
            grade = grade_raw[0] if grade_raw else 'A'
            c1 = self.entries['cust_start'].get().strip()
            c1 = ''.join(c for c in c1 if c.isdigit())
            s1 = self.entries['seq_start'].get().strip()
            s1 = ''.join(c for c in s1 if c.isdigit())
            n_str = self.entries['batch_size'].get().strip()
            n = int(''.join(c for c in n_str if c.isdigit()) or '0')
            year = int(self.year_entry.get())
            month = int(self.month_combo.get())
            yc = year_encode(year)
            mc = month_encode(month)

            if not mfr or len(mfr) < 2:
                self._set_status("厂商码至少2位", C['error']); return
            if n <= 0 or n > 9999:
                self._set_status("数量需在 1-9999 之间", C['error']); return

            self.sn_list = []
            for i in range(n):
                seq_str = str(int(s1) + i).zfill(5)
                cust_str = c1.zfill(3)  # 客户流水号固定，不递增
                customer = grade + cust_str
                sn = generate(mfr, country, customer, yc + mc, seq_str)
                if not sn:
                    self._set_status(f"第{i+1}台生成失败", C['error']); return
                pf = parse(sn)
                pin = compute_pin(sn)
                self.sn_list.append({
                    'index': i + 1,
                    'sn': sn,
                    'readable': printable(sn),
                    'pin': pin_to_str(pin),
                    'mfr': mfr, 'country': country, 'customer': customer,
                    'grade': grade, 'grade_name': GRADE_NAMES.get(grade, ''),
                    'cust_num': int(cust_str),
                    'year': year, 'month': month,
                    'seq_num': int(seq_str),
                    'check': pf.get('check_digit', ''),
                })

            # 显示列表
            self.sn_text.configure(state='normal')
            self.sn_text.delete('1.0', 'end')
            for item in self.sn_list:
                self.sn_text.insert('end',
                    f"{item['index']:4d}  {item['sn']}  PIN={item['pin']}\n")
            self.sn_text.configure(state='disabled')

            self.count_lbl.configure(text=f"共 {len(self.sn_list)} 台")
            self.export_btn.configure(state='normal')
            self._set_status(f"已生成 {len(self.sn_list)} 个序列号+PIN", C['success'])
        except Exception as e:
            self._set_status(f"错误: {e}", C['error'])

    def _make_nameplate_image(self, sn, pin_str):
        """生成铭牌图片：SN + 二维码 + PIN（统一风格，居中布局）"""

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
        qr_url = self.qr_url_entry.get().strip() if hasattr(self, 'qr_url_entry') else "https://jiuxiaoyw.online/bind?sn={sn}&pin={pin}"
        qr_content = qr_url.format(sn=sn, pin=pin_str)
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

        return img

    def _export(self):
        if not self.sn_list:
            return

        out_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'nameplate_out')
        os.makedirs(out_dir, exist_ok=True)

        total = len(self.sn_list)
        self.export_btn.configure(state='disabled')
        self.gen_btn.configure(state='disabled')
        self.progress.set(0)

        def worker():
            # CSV
            csv_path = os.path.join(out_dir, 'manifest.csv')
            with open(csv_path, 'w', newline='', encoding='utf-8-sig') as f:
                w = csv.writer(f)
                w.writerow(["序号", "SN", "PIN", "厂商", "国家", "客户编码",
                            "客户等级", "年份", "月份", "序列号"])
                for item in self.sn_list:
                    w.writerow([item['index'], item['sn'], item['pin'],
                                item['mfr'], item['country'], item['customer'],
                                item['grade_name'], item['year'], item['month'],
                                item['seq_num']])

            # 铭牌图片
            if HAS_QR:
                for i, item in enumerate(self.sn_list):
                    img = self._make_nameplate_image(item['sn'], item['pin'])
                    path = os.path.join(out_dir, f"{item['sn']}.png")
                    img.save(path, dpi=(300, 300), quality=95)

                    pct = (i + 1) / total
                    self.root.after(0, lambda p=pct: self.progress.set(p))
                    self.root.after(0, lambda c=i+1, t=total:
                                   self._set_status(f"导出中... {c}/{t}", C['info']))

            self.root.after(0, lambda: self._on_export_done(out_dir))

        threading.Thread(target=worker, daemon=True).start()

    def _on_export_done(self, out_dir):
        self.export_btn.configure(state='normal')
        self.gen_btn.configure(state='normal')
        self.progress.set(1.0)
        self._set_status(f"已导出到 {out_dir}", C['success'])

    def _set_status(self, msg, color):
        self.status_lbl.configure(text=msg, text_color=color)

    def run(self):
        self.root.mainloop()


def main():
    if not HAS_CTK:
        print("请安装 customtkinter: pip install customtkinter")
        return
    NameplateGUI().run()


if __name__ == '__main__':
    main()
