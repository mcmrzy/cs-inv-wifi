#!/usr/bin/env python3
# -*- coding: utf-8 -*-
import re

# Fix arm_inverter_simulator_part1.py
part1_path = r'd:\trae_project\CS_INV_Inverter_Project\arduino_inverter_wifi\tools\arm_inverter_simulator_part1.py'

with open(part1_path, 'r', encoding='utf-8') as f:
    content = f.read()

# 1. Fix efficiency calculation (line 281)
old_efficiency = "p['efficiency'] = round(self.total_active_power/max(1,self.total_dc_power)*100,1) if self.total_dc_power>10 else 95.0"
new_efficiency = "p['efficiency'] = min(98.0, max(90.0, round(self.total_active_power/max(1,self.total_dc_power)*100,1))) if self.total_dc_power>10 else 95.0"
content = content.replace(old_efficiency, new_efficiency)

# 2. Remove total_running_time increment (line 324)
old_running_time = "        self.total_running_time+=int(dt/3600) if random.random()<0.1 else 0\n"
content = content.replace(old_running_time, "")

# 3. Fix active power unit - divide by 1000 (line 308-309)
old_power = """        self.total_active_power=(self.phase_a_voltage*self.phase_a_current+
            self.phase_b_voltage*self.phase_b_current+self.phase_c_voltage*self.phase_c_current)*self.power_factor"""
new_power = """        self.total_active_power=(self.phase_a_voltage*self.phase_a_current+
            self.phase_b_voltage*self.phase_b_current+self.phase_c_voltage*self.phase_c_current)*self.power_factor/1000"""
content = content.replace(old_power, new_power)

# 4. Fix MPPT voltage default value (line 162)
old_mppt = "self.mppt_voltage = [380.0, 0.0, 0.0, 0.0]"
new_mppt = "self.mppt_voltage = [320.0, 0.0, 0.0, 0.0]"
content = content.replace(old_mppt, new_mppt)

with open(part1_path, 'w', encoding='utf-8') as f:
    f.write(content)

print(f"Fixed {part1_path}")

# Fix arm_inverter_simulator_gui.py
gui_path = r'd:\trae_project\CS_INV_Inverter_Project\arduino_inverter_wifi\tools\arm_inverter_simulator_gui.py'

with open(gui_path, 'r', encoding='utf-8') as f:
    content = f.read()

# 5. Fix GUI MPPT1 V default (line 46)
old_m1v = "('m1v','MPPT1 V','V','380')"
new_m1v = "('m1v','MPPT1 V','V','320')"
content = content.replace(old_m1v, new_m1v)

# 6. Fix GUI Hours default (line 59)
old_hr = "('hr','Hours','h','8640')"
new_hr = "('hr','Hours','h','720')"
content = content.replace(old_hr, new_hr)

with open(gui_path, 'w', encoding='utf-8') as f:
    f.write(content)

print(f"Fixed {gui_path}")
print("All fixes applied successfully!")
