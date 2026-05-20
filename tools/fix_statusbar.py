#!/usr/bin/env python3
# -*- coding: utf-8 -*-
import sys

file_path = r'd:\trae_project\CS_INV_Inverter_Project\arduino_inverter_wifi\tools\arm_inverter_simulator_gui.py'

with open(file_path, 'r', encoding='utf-8') as f:
    lines = f.readlines()

# 查找 save() 函数并修改
modified = False
for i, line in enumerate(lines):
    if 'def save():' in line and i > 400:  # 在 _open_settings 中的 save
        # 找到 self.l_br.configure 这一行，在其后添加 update_idletasks
        for j in range(i, min(i+10, len(lines))):
            if 'self.l_br.configure' in lines[j]:
                # 检查下一行是否已经有 update_idletasks
                if j+1 < len(lines) and 'update_idletasks' not in lines[j+1]:
                    # 在 self.l_br.configure 行后插入 update_idletasks
                    indent = '            '
                    lines.insert(j+1, indent + "self.update_idletasks()  # 强制刷新界面\n")
                    print(f'已在第 {j+2} 行添加 update_idletasks')
                    modified = True
                break
        break

if modified:
    with open(file_path, 'w', encoding='utf-8') as f:
        f.writelines(lines)
    print('修复完成')
else:
    print('未找到需要修改的位置')
