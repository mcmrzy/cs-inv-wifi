#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
SN 批量生产 GUI 工具 — CS-SN-STD-001 V1.1
==========================================
结构化编辑序列号各字段，批量生成 + 预览 + 导出二维码 & CSV

校验位算法: CRC-16 MODBUS (Poly=0x8005, Init=0xFFFF, RefIn=true, RefOut=true)
"""

import sys, os, csv, threading
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
}

def FF(family='Microsoft YaHei', size=12, weight='normal'):
    return CTkFont(family=family, size=size, weight=weight)


class SNBatchGUI:
    def __init__(self):
        ctk.set_appearance_mode("dark")
        ctk.set_default_color_theme("dark-blue")

        self.root = CTk()
        self.root.title("SN 批量生产工具 — 辰烁科技 CS-SN-STD-001 V1.1")
        self.root.geometry("1050x750")
        self.root.configure(fg_color=C['bg'])

        self.sn_list = []
        self.preview_img = None
        self._build_ui()

    def _build_ui(self):
        # 顶部标题栏
        header = CTkFrame(self.root, fg_color=C['card'], height=55, corner_radius=0)
        header.pack(fill='x')
        header.pack_propagate(False)
        CTkLabel(header, text="🏭 SN 批量生产工具", font=FF(size=18, weight='bold'),
                 text_color=C['t']).pack(side='left', padx=20, pady=10)
        CTkLabel(header, text="CS-SN-STD-001 V1.1  ·  CRC-16 MODBUS 校验",
                 text_color=C['t3'], font=FF(size=11)).pack(side='left', padx=(0, 20))

        # 主内容区
        main = CTkFrame(self.root, fg_color=C['bg'])
        main.pack(fill='both', expand=True, padx=15, pady=10)

        # ---- 左侧: 参数输入 ----
        left = CTkFrame(main, fg_color=C['card'], corner_radius=12, width=360)
        left.pack(side='left', fill='both', padx=(0, 12))
        left.pack_propagate(False)

        CTkLabel(left, text="生产参数", font=FF(size=14, weight='bold'),
                 text_color=C['t']).pack(anchor='w', padx=15, pady=(12, 8))

        self.entries = {}

        # 厂商码
        self._add_field(left, "厂商码 (第1-2位)", "manufacturer", "H1",
                        tooltip="1位厂商类型(H/O/S)+1位厂区序号, 如 H1=总部1号工厂")

        # 国家码
        row = CTkFrame(left, fg_color='transparent')
        row.pack(fill='x', padx=15, pady=3)
        CTkLabel(row, text="国家/地区 (第3-4位)", width=140, anchor='w',
                 text_color=C['t2'], font=FF(size=11)).pack(side='left')
        self.country_combo = CTkComboBox(
            row, values=[f"{c[0]} - {c[1]}" for c in COMMON_COUNTRIES],
            width=180, fg_color=C['accent'], border_color=C['border'],
            button_color=C['highlight'], font=FF(family='Consolas', size=11))
        self.country_combo.set("DE - 德国")
        self.country_combo.pack(side='left')

        # 客户等级 + 流水号
        row2 = CTkFrame(left, fg_color='transparent')
        row2.pack(fill='x', padx=15, pady=3)
        CTkLabel(row2, text="客户等级 (第5位)", width=140, anchor='w',
                 text_color=C['t2'], font=FF(size=11)).pack(side='left')
        grade_vals = [f"{g} - {n}" for g, n in [
            ('A', '战略大客户'), ('B', '核心分销商'), ('C', '安装商'),
            ('D', '授权EPC'), ('E', '二级代理'), ('P', '项目'), ('X', '零售')
        ]]
        self.grade_combo = CTkComboBox(row2, values=grade_vals, width=180,
                                       fg_color=C['accent'], border_color=C['border'],
                                       button_color=C['highlight'], font=FF(family='Consolas', size=11))
        self.grade_combo.set("A - 战略大客户")
        self.grade_combo.pack(side='left')

        self._add_field(left, "客户流水起始 (第6-8位)", "cust_start", "1",
                        tooltip="3位数字, 不足补零, 如 001")

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

        self._add_field(left, "生产序列起始 (第11-15位)", "seq_start", "1",
                        tooltip="5位数字, 不足补零, 如 00001")

        self._add_field(left, "生产数量 (台)", "batch_size", "100",
                        tooltip="本批次生成数量")

        # 年份码/月份码预览
        self.date_preview = CTkLabel(left, text="年份码: ?  月份码: ?",
                                     text_color=C['t3'], font=FF(family='Consolas', size=11))
        self.date_preview.pack(anchor='w', padx=15, pady=(2, 0))

        # SN 范围预览
        self.range_preview = CTkLabel(left, text="",
                                      text_color=C['t2'], font=FF(family='Consolas', size=10))
        self.range_preview.pack(anchor='w', padx=15, pady=(2, 0))

        # 生成按钮
        CTkFrame(left, fg_color=C['border'], height=1).pack(fill='x', padx=10, pady=10)

        self.gen_btn = CTkButton(left, text="🔄 生成并预览",
                                 fg_color=C['highlight'], hover_color=C['accent'],
                                 height=38, font=FF(size=13, weight='bold'),
                                 command=self._generate)
        self.gen_btn.pack(fill='x', padx=15, pady=5)

        self.export_btn = CTkButton(left, text="📦 导出二维码 + CSV",
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

        # ---- 右侧: 预览区 ----
        right = CTkFrame(main, fg_color=C['card'], corner_radius=12)
        right.pack(side='left', fill='both', expand=True)

        # 右上: 列表
        CTkLabel(right, text="序列号列表", font=FF(size=13, weight='bold'),
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
        if HAS_QR:
            self.qr_preview_lbl = CTkLabel(stats_frame, text="",
                                           text_color=C['info'], font=FF(size=11))
            self.qr_preview_lbl.pack(side='right')

    def _add_field(self, parent, label, key, default, tooltip=""):
        row = CTkFrame(parent, fg_color='transparent')
        row.pack(fill='x', padx=15, pady=3)
        CTkLabel(row, text=label, width=140, anchor='w',
                 text_color=C['t2'], font=FF(size=11)).pack(side='left')
        e = CTkEntry(row, width=180, height=28, fg_color=C['accent'],
                     border_color=C['border'], text_color=C['t'],
                     font=FF(family='Consolas', size=12))
        e.insert(0, default)
        e.pack(side='left')
        e.bind('<KeyRelease>', lambda ev: self._update_preview())
        self.entries[key] = e

    def _update_preview(self):
        """实时更新日期码和 SN 范围预览"""
        try:
            year = int(self.year_entry.get())
            month = int(self.month_combo.get())
            yc = year_encode(year)
            mc = month_encode(month)
            self.date_preview.configure(text=f"年份码: {yc}  月份码: {mc}")

            # SN 范围
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
                first = generate(mfr, country, grade + c1.zfill(3),
                                 yc + mc, s1.zfill(5))
                last = generate(mfr, country, grade + str(int(c1)+n-1).zfill(3),
                                yc + mc, str(int(s1)+n-1).zfill(5))
                if first and last:
                    self.range_preview.configure(
                        text=f"范围: {printable(first)}  ~  {printable(last)}")
        except:
            pass

    def _generate(self):
        """生成 SN 列表"""
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
                cust_str = str(int(c1) + i).zfill(3)
                customer = grade + cust_str
                sn = generate(mfr, country, customer, yc + mc, seq_str)
                if not sn:
                    self._set_status(f"第{i+1}台生成失败", C['error']); return
                pf = parse(sn)
                self.sn_list.append({
                    'index': i + 1,
                    'sn': sn,
                    'readable': printable(sn),
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
                self.sn_text.insert('end', f"{item['index']:4d}  {item['readable']}\n")
            self.sn_text.configure(state='disabled')

            self.count_lbl.configure(text=f"共 {len(self.sn_list)} 台")
            self.export_btn.configure(state='normal')
            if HAS_QR and self.sn_list:
                self.qr_preview_lbl.configure(
                    text=f"首台: {self.sn_list[0]['sn']}")

            self._set_status(f"已生成 {len(self.sn_list)} 个序列号", C['success'])
        except Exception as e:
            self._set_status(f"错误: {e}", C['error'])

    def _make_qr_image(self, sn, readable):
        """生成 SN 二维码铭牌图（用于预览）"""

        def _load_font(sizes, prefer=("msyh.ttc", "msyhbd.ttc")):
            for sz in sizes:
                for name in prefer + ("simhei.ttf", "consola.ttf", "cour.ttf"):
                    try: return ImageFont.truetype(name, sz)
                    except: pass
            return ImageFont.load_default()

        font_sn_big   = _load_font((48, 44, 40, 36), prefer=("consola.ttf", "cour.ttf"))
        font_sn_small = _load_font((32, 28, 24), prefer=("consola.ttf", "cour.ttf"))
        font_label    = _load_font((18, 16))

        dummy = Image.new("RGB", (1, 1))
        d = ImageDraw.Draw(dummy)
        bbox = d.textbbox((0, 0), readable, font=font_sn_big)
        font_sn = font_sn_small if bbox[2] > 280 else font_sn_big

        bbox = d.textbbox((0, 0), readable, font=font_sn)
        sn_w = bbox[2]
        sn_h = bbox[3]

        qr_content = f"SN:{sn}"
        qr = qrcode.QRCode(
            version=2,
            error_correction=qrcode.constants.ERROR_CORRECT_H,
            box_size=8,
            border=2,
        )
        qr.add_data(qr_content)
        qr.make(fit=True)
        qr_img = qr.make_image(fill_color="black", back_color="white").convert("RGB")
        qr_size = 280

        text_area_w = sn_w + 20
        text_area_h = sn_h + 20 + 30
        qr_area_w = qr_size + 40

        pad = 24
        img_w = max(pad * 2 + text_area_w + 50 + qr_area_w, 600)
        img_h = pad * 2 + max(text_area_h, qr_size + 20)

        BORDER_COLOR = (45, 55, 72)
        BG_COLOR = (255, 255, 255)
        TEXT_COLOR = (26, 32, 44)
        LABEL_COLOR = (113, 128, 150)
        QR_BG_COLOR = (248, 250, 252)

        img = Image.new("RGB", (img_w, img_h), BG_COLOR)
        draw = ImageDraw.Draw(img)

        draw.rectangle([0, 0, img_w - 1, 6], fill=BORDER_COLOR)

        left_x = pad
        left_y = (img_h - text_area_h) // 2

        draw.text((left_x, left_y), "SN:", fill=LABEL_COLOR, font=font_label)
        sn_y = left_y + 18 + 10
        draw.text((left_x, sn_y), readable, fill=TEXT_COLOR, font=font_sn)
        hint_y = sn_y + sn_h + 15
        draw.text((left_x, hint_y), "SCAN WITH CAMERA", fill=LABEL_COLOR, font=font_label)

        line_x = pad + sn_w + 30
        line_y1 = pad + 20
        line_y2 = img_h - pad - 20
        draw.line([(line_x, line_y1), (line_x, line_y2)], fill=(226, 232, 240), width=1)

        qr_x = line_x + 25
        qr_y = (img_h - qr_size) // 2

        draw.rounded_rectangle(
            [qr_x - 6, qr_y - 6, qr_x + qr_size + 6, qr_y + qr_size + 6],
            radius=12, fill=(220, 228, 236)
        )
        draw.rounded_rectangle(
            [qr_x - 3, qr_y - 3, qr_x + qr_size + 3, qr_y + qr_size + 3],
            radius=10, fill=QR_BG_COLOR
        )

        qr_resized = qr_img.resize((qr_size, qr_size), Image.LANCZOS)
        img.paste(qr_resized, (qr_x, qr_y))

        draw.rectangle([0, img_h - 6, img_w - 1, img_h - 1], fill=BORDER_COLOR)

        return ImageTk.PhotoImage(img)

    def _export(self):
        """导出二维码 + CSV"""
        if not self.sn_list:
            return

        out_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'qr_out')
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
                w.writerow(["序号", "序列号", "可读格式", "厂商", "国家", "客户编码",
                            "客户等级", "客户流水", "年份", "月份", "生产序列", "校验位"])
                for item in self.sn_list:
                    w.writerow([item['index'], item['sn'], item['readable'],
                                item['mfr'], item['country'], item['customer'],
                                item['grade_name'], item['cust_num'],
                                item['year'], item['month'], item['seq_num'],
                                item['check']])

            # 二维码
            if HAS_QR:
                def _load_font(sizes, prefer=("msyh.ttc", "msyhbd.ttc")):
                    for sz in sizes:
                        for name in prefer + ("simhei.ttf", "consola.ttf", "cour.ttf"):
                            try: return ImageFont.truetype(name, sz)
                            except: pass
                    return ImageFont.load_default()

                font_sn_big   = _load_font((48, 44, 40, 36), prefer=("consola.ttf", "cour.ttf"))
                font_sn_small = _load_font((32, 28, 24), prefer=("consola.ttf", "cour.ttf"))
                font_label    = _load_font((18, 16))

                for i, item in enumerate(self.sn_list):
                    readable = item['readable']

                    dummy = Image.new('RGB', (1, 1))
                    d = ImageDraw.Draw(dummy)
                    bbox = d.textbbox((0, 0), readable, font=font_sn_big)
                    font_sn = font_sn_small if bbox[2] > 280 else font_sn_big

                    bbox = d.textbbox((0, 0), readable, font=font_sn)
                    sn_w = bbox[2]
                    sn_h = bbox[3]

                    qr_content = f"SN:{item['sn']}"
                    qr = qrcode.QRCode(
                        version=2,
                        error_correction=qrcode.constants.ERROR_CORRECT_H,
                        box_size=8,
                        border=2,
                    )
                    qr.add_data(qr_content)
                    qr.make(fit=True)
                    qr_img = qr.make_image(fill_color="black", back_color="white").convert('RGB')
                    qr_size = 280

                    text_area_w = sn_w + 20
                    text_area_h = sn_h + 20 + 30
                    qr_area_w = qr_size + 40

                    pad = 24
                    img_w = max(pad * 2 + text_area_w + 50 + qr_area_w, 600)
                    img_h = pad * 2 + max(text_area_h, qr_size + 20)

                    BORDER_COLOR = (45, 55, 72)
                    BG_COLOR = (255, 255, 255)
                    TEXT_COLOR = (26, 32, 44)
                    LABEL_COLOR = (113, 128, 150)
                    QR_BG_COLOR = (248, 250, 252)

                    img = Image.new('RGB', (img_w, img_h), BG_COLOR)
                    draw = ImageDraw.Draw(img)

                    draw.rectangle([0, 0, img_w - 1, 6], fill=BORDER_COLOR)

                    left_x = pad
                    left_y = (img_h - text_area_h) // 2

                    draw.text((left_x, left_y), 'SN:', fill=LABEL_COLOR, font=font_label)
                    sn_y = left_y + 18 + 10
                    draw.text((left_x, sn_y), readable, fill=TEXT_COLOR, font=font_sn)
                    hint_y = sn_y + sn_h + 15
                    draw.text((left_x, hint_y), 'SCAN WITH CAMERA', fill=LABEL_COLOR, font=font_label)

                    line_x = pad + sn_w + 30
                    line_y1 = pad + 20
                    line_y2 = img_h - pad - 20
                    draw.line([(line_x, line_y1), (line_x, line_y2)], fill=(226, 232, 240), width=1)

                    qr_x = line_x + 25
                    qr_y = (img_h - qr_size) // 2

                    draw.rounded_rectangle(
                        [qr_x - 6, qr_y - 6, qr_x + qr_size + 6, qr_y + qr_size + 6],
                        radius=12, fill=(220, 228, 236)
                    )
                    draw.rounded_rectangle(
                        [qr_x - 3, qr_y - 3, qr_x + qr_size + 3, qr_y + qr_size + 3],
                        radius=10, fill=QR_BG_COLOR
                    )

                    qr_resized = qr_img.resize((qr_size, qr_size), Image.LANCZOS)
                    img.paste(qr_resized, (qr_x, qr_y))

                    draw.rectangle([0, img_h - 6, img_w - 1, img_h - 1], fill=BORDER_COLOR)

                    path = os.path.join(out_dir, f"{item['sn']}.png")
                    img.save(path, dpi=(300, 300), quality=95)

                    # 更新进度
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
    SNBatchGUI().run()


if __name__ == '__main__':
    main()
