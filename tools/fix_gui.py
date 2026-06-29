#!/usr/bin/env python3
# -*- coding: utf-8 -*-
import re

with open('arm_inverter_simulator_gui.py', 'r', encoding='utf-8') as f:
    content = f.read()

# 修复方案：修改 entries 的 key 为短名称
# 1. 先修改 for 循环，添加 key 变量
# 2. 修改 entries[lb]=e 为 entries[key]=e
# 3. 修改 save() 函数中的访问

old_code = """        for lb, dv in [('设备SN:',self.ctrl.device_sn),('设备密钥:',self.ctrl.device_key),
            ('MQTT Broker:',self.ctrl.mqtt_broker),('AP SSID:',self.ctrl.ap_ssid)]:
            r = ctk.CTkFrame(frm, fg_color='transparent'); r.pack(fill='x', pady=7)
            ctk.CTkLabel(r, text=lb, width=80, anchor='e', font=FF(size=13),
                         text_color=C['t2']).pack(side='left', padx=(0,8))
            e = ctk.CTkEntry(r, height=34, fg_color=C['card'], border_color=C['border'],
                text_color=C['t'], font=FF(family='Consolas', size=13))
            e.insert(0,dv); e.pack(side='left',fill='x',expand=True); entries[lb]=e
        def save():
            self.ctrl.device_sn=entries['SN:'].get(); self.ctrl.device_key=entries['Key:'].get()
            self.ctrl.mqtt_broker=entries['Broker:'].get(); self.ctrl.ap_ssid=entries['AP SSID:'].get()"""

new_code = """        fields = [
            ('设备SN:', 'sn', self.ctrl.device_sn),
            ('设备密钥:', 'key', self.ctrl.device_key),
            ('MQTT Broker:', 'broker', self.ctrl.mqtt_broker),
            ('AP SSID:', 'ap', self.ctrl.ap_ssid),
        ]
        for lb, key, dv in fields:
            r = ctk.CTkFrame(frm, fg_color='transparent'); r.pack(fill='x', pady=7)
            ctk.CTkLabel(r, text=lb, width=80, anchor='e', font=FF(size=13),
                         text_color=C['t2']).pack(side='left', padx=(0,8))
            e = ctk.CTkEntry(r, height=34, fg_color=C['card'], border_color=C['border'],
                text_color=C['t'], font=FF(family='Consolas', size=13))
            e.insert(0,dv); e.pack(side='left',fill='x',expand=True); entries[key]=e
        def save():
            self.ctrl.device_sn=entries['sn'].get(); self.ctrl.device_key=entries['key'].get()
            self.ctrl.mqtt_broker=entries['broker'].get(); self.ctrl.ap_ssid=entries['ap'].get()"""

if old_code in content:
    content = content.replace(old_code, new_code)
    with open('arm_inverter_simulator_gui.py', 'w', encoding='utf-8') as f:
        f.write(content)
    print('修复成功！')
else:
    print('未找到匹配代码，尝试查找问题...')
    # 尝试查找实际的代码
    if "entries['SN:']" in content:
        print('找到 entries[\'SN:\']，但格式可能不同')
    if "entries[lb]=e" in content:
        print('找到 entries[lb]=e')
