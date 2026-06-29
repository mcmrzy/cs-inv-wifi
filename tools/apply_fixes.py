#!/usr/bin/env python3
# -*- coding: utf-8 -*-

# Fix arm_inverter_simulator_part1.py
part1_path = r'd:\trae_project\CS_INV_Inverter_Project\arduino_inverter_wifi\tools\arm_inverter_simulator_part1.py'

with open(part1_path, 'r', encoding='utf-8') as f:
    content = f.read()

# 1. Fix efficiency calculation (line 281)
old_efficiency = "p['efficiency'] = round(self.total_active_power/max(1,self.total_dc_power)*100,1) if self.total_dc_power>10 else 95.0"
new_efficiency = "p['efficiency'] = min(98.0, max(90.0, round(self.total_active_power/max(1,self.total_dc_power)*100,1))) if self.total_dc_power>10 else 95.0"
content = content.replace(old_efficiency, new_efficiency)
print('Fix 1: Efficiency calculation updated')

# 2. Remove total_running_time increment (line 324)
old_running_time = "        self.total_running_time+=int(dt/3600) if random.random()<0.1 else 0\n"
content = content.replace(old_running_time, "")
print('Fix 2: Total running time increment removed')

# 3. Fix active power unit - divide by 1000 (line 308-309)
old_power = """        self.total_active_power=(self.phase_a_voltage*self.phase_a_current+
            self.phase_b_voltage*self.phase_b_current+self.phase_c_voltage*self.phase_c_current)*self.power_factor"""
new_power = """        self.total_active_power=(self.phase_a_voltage*self.phase_a_current+
            self.phase_b_voltage*self.phase_b_current+self.phase_c_voltage*self.phase_c_current)*self.power_factor/1000"""
content = content.replace(old_power, new_power)
print('Fix 3: Active power unit fixed (divided by 1000)')

# 4. Fix MPPT voltage default value (line 162)
old_mppt = "self.mppt_voltage = [380.0, 0,0,0,0,0,0,0,0,0,0,0]"
new_mppt = "self.mppt_voltage = [320.0, 0,0,0,0,0,0,0,0,0,0,0]"
content = content.replace(old_mppt, new_mppt)
print('Fix 4: MPPT voltage default changed from 380V to 320V')

with open(part1_path, 'w', encoding='utf-8') as f:
    f.write(content)

print(f'\nFixed {part1_path}')
print('All fixes applied successfully!')
