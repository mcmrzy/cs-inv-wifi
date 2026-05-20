#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ARM 逆变器终端模拟器 GUI - 辰烁科技 CS_INV
==========================================
CS-I10-6k2 单相离网48V逆变器 ARM模拟器
支持 AC输出、BMS电池、MPPT光伏、系统状态、能量统计、电芯详情
"""

import sys, os, time, json, threading, random, math
from datetime import datetime
from collections import deque

try:
    import customtkinter as ctk
    from customtkinter import CTk, CTkFrame, CTkLabel, CTkButton, CTkEntry, CTkComboBox
    from customtkinter import CTkCheckBox, CTkSlider, CTkTextbox, CTkScrollableFrame
    from customtkinter import CTkSegmentedButton
    HAS_CTK = True
except ImportError:
    HAS_CTK = False
    print("需要安装 customtkinter: pip install customtkinter")

try:
    import serial
    import serial.tools.list_ports
    HAS_SERIAL = True
except ImportError:
    HAS_SERIAL = False

# 主题配色
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
    return ctk.CTkFont(family=family, size=size, weight=weight)

# ==================== 协议常量 ====================
FRAME_HEADER, FRAME_ESCAPE = 0xAA, 0x55
FRAME_ESC_HEADER, FRAME_ESC_ESCAPE = 0x01, 0x00
FRAME_DATA_MAX = 512

CMD_SET_BROKER, CMD_PUBLISH, CMD_COMMAND_RECV = 0x01, 0x02, 0x03
CMD_COMMAND_SEND, CMD_FACTORY_RESET, CMD_HEARTBEAT = 0x04, 0x05, 0x06
CMD_SET_SN, CMD_SET_KEY, CMD_SET_AP_SSID = 0x08, 0x09, 0x0A
CMD_ACK, CMD_NACK = 0xFE, 0xFF

# 导入 SN 编码模块（仅用于验证，模拟器不生成 SN）
try:
    from sn_encoder import validate as sn_validate
    HAS_SN_ENCODER = True
except ImportError:
    HAS_SN_ENCODER = False

CMD_NAMES = {
    0x01:"SET_BROKER", 0x02:"PUBLISH", 0x03:"COMMAND_RECV",
    0x04:"COMMAND_SEND", 0x05:"FACTORY_RESET", 0x06:"HEARTBEAT",
    0x08:"SET_SN", 0x09:"SET_KEY", 0x0A:"SET_AP_SSID",
    0xFE:"ACK", 0xFF:"NACK",
}

# ==================== 帧编解码 ====================
def calc_xor(cmd, data):
    x = FRAME_HEADER ^ cmd
    dl = len(data)
    x ^= (dl>>8)&0xFF; x ^= dl&0xFF
    for b in data: x ^= b
    return x & 0xFF

def encode_frame(cmd, data):
    xor_val = calc_xor(cmd, data)
    dl = len(data)
    raw = bytearray([FRAME_HEADER, cmd, (dl>>8)&0xFF, dl&0xFF])
    raw.extend(data); raw.append(xor_val)
    escaped = bytearray([FRAME_HEADER])
    for b in raw[1:]:
        if b == FRAME_HEADER: escaped.extend([FRAME_ESCAPE, FRAME_ESC_HEADER])
        elif b == FRAME_ESCAPE: escaped.extend([FRAME_ESCAPE, FRAME_ESC_ESCAPE])
        else: escaped.append(b)
    return bytes(escaped), xor_val

class FrameReceiver:
    RX_WAIT_HEADER, RX_RECV_CMD, RX_RECV_LEN_H, RX_RECV_LEN_L, RX_RECV_DATA, RX_RECV_XOR = range(6)
    def __init__(self): self.reset()
    def reset(self):
        self.state = self.RX_WAIT_HEADER
        self.cmd = self.data_len = self.data_idx = self.rx_xor = 0
        self.data_buf = bytearray(FRAME_DATA_MAX)
        self.esc_next = False
    def feed(self, b):
        if self.state != self.RX_WAIT_HEADER:
            if not self.esc_next and b == FRAME_ESCAPE:
                self.esc_next = True; return None
            if self.esc_next:
                self.esc_next = False
                if b == FRAME_ESC_HEADER: b = FRAME_HEADER
                elif b == FRAME_ESC_ESCAPE: b = FRAME_ESCAPE
        if self.state == self.RX_WAIT_HEADER:
            if b == FRAME_HEADER:
                self.state = self.RX_RECV_CMD
                self.rx_xor = FRAME_HEADER
            return None
        elif self.state == self.RX_RECV_CMD:
            self.cmd = b; self.rx_xor ^= b; self.state = self.RX_RECV_LEN_H; return None
        elif self.state == self.RX_RECV_LEN_H:
            self.data_len = b << 8; self.rx_xor ^= b; self.state = self.RX_RECV_LEN_L; return None
        elif self.state == self.RX_RECV_LEN_L:
            self.data_len |= b; self.rx_xor ^= b; self.data_idx = 0
            if self.data_len == 0: self.state = self.RX_RECV_XOR
            elif self.data_len > FRAME_DATA_MAX: self.state = self.RX_WAIT_HEADER
            else: self.state = self.RX_RECV_DATA
            return None
        elif self.state == self.RX_RECV_DATA:
            self.data_buf[self.data_idx] = b; self.data_idx += 1; self.rx_xor ^= b
            if self.data_idx >= self.data_len: self.state = self.RX_RECV_XOR
            return None
        elif self.state == self.RX_RECV_XOR:
            self.state = self.RX_WAIT_HEADER
            valid = (b == self.rx_xor)
            return {
                'cmd': self.cmd, 'cmd_name': CMD_NAMES.get(self.cmd, f"UNK_{self.cmd:02X}"),
                'length': self.data_len, 'data': bytes(self.data_buf[:self.data_len]),
                'xor': b, 'expected_xor': self.rx_xor, 'valid': valid,
                'timestamp': datetime.now()
            }
        return None

# ==================== 逆变器模型（CS-I10-6k2 单相离网48V）====================
class InverterModel:
    """CS-I10-6k2 单相离网48V逆变器仿真模型"""

    # 工作状态映射
    STATE_MAP = {
        'inverting': '逆变运行',
        'standby': '待机',
        'fault': '故障',
        'shutdown': '关机',
    }

    def __init__(self):
        # ========== 设备信息 ==========
        self.sn = "H1CNA0013500001O"
        self.model = "CS-I10-6k2"
        self.manufacturer = "辰烁科技"
        self.firmware_arm = "V1.2.3.20240510"
        self.firmware_esp = "V1.0.5.20240420"
        self.type = "离网逆变器"
        self.phase = 1  # 单相
        self.rated_power = 6200  # 额定功率 6200W
        self.rated_voltage = 220  # 额定输出电压 220V
        self.rated_freq = 50.0  # 额定频率 50Hz
        self.battery_voltage = 51.2  # 电池额定电压 48V (16S LiFePO4)
        self.battery_types = "LiFePO4"
        self.mppt_count = 1  # 1路 MPPT
        self.pv_max_voltage = 150  # PV 最大电压 150V
        self.pv_max_power = 2000  # PV 最大功率 2000W
        self.bms_count = 1
        self.cell_count = 16  # 16节电芯

        # ========== AC 输出（单相） ==========
        self.voltage = 220.0  # 输出电压 220V
        self.current = 10.0  # 输出电流 0~30A
        self.power = 2200.0  # 有功功率 W
        self.apparent = 2200.0  # 视在功率 VA
        self.reactive = 0.0  # 无功功率 Var
        self.frequency = 50.00  # 频率 Hz（离网自生成）
        self.pf = 0.99  # 功率因数
        self.load_percent = 35.5  # 负载百分比
        self.thd_v = 2.0  # 电压 THD %
        self.thd_i = 3.0  # 电流 THD %
        self.dc_injection = 15  # 直流注入 mA

        # ========== BMS 电池参数 ==========
        self.soc = 75.0  # 荷电状态 %
        self.soh = 96.0  # 健康状态 %
        self.battery_v = 51.2  # 电池总电压 V
        self.battery_i = -15.0  # 电池电流 A（负=放电，正=充电）
        self.battery_power = -768.0  # 电池功率 W
        self.capacity_remain = 75.0  # 剩余容量 Ah
        self.capacity_total = 100.0  # 总容量 Ah
        self.cycle_count = 152  # 循环次数
        self.temp_max = 28.0  # 电池最高温度
        self.temp_min = 25.0  # 电池最低温度
        self.cell_volt_max = 3.35  # 单体最高电压
        self.cell_volt_min = 3.28  # 单体最低电压
        self.cell_volt_diff = 0.07  # 压差 V
        self.charge_state = "discharging"  # 充放电状态
        self.battery_type = "LiFePO4"
        self.protect_status1 = 0  # 保护状态1（正常）
        self.protect_status2 = 0  # 保护状态2（正常）

        # ========== 电芯详细数据（16节） ==========
        self.cell_voltages = [3.30 + random.uniform(-0.02, 0.02) for _ in range(16)]
        self.cell_temps = [26.0 + random.uniform(-1.0, 2.0) for _ in range(16)]
        self.charge_ah_total = 12580.3  # 累计充电 Ah
        self.discharge_ah_total = 12450.6  # 累计放电 Ah

        # ========== MPPT 参数 ==========
        self.pv_voltage = 85.0  # PV 电压 V（范围 60~150V）
        self.pv_current = 12.0  # PV 电流 A
        self.pv_power = 1020.0  # PV 功率 W
        self.mppt_state = "tracking"  # MPPT 状态

        # ========== 系统状态 ==========
        self.state = "inverting"  # 工作状态
        self.fault_code = 0  # 故障代码
        self.alarm_code = 0  # 告警代码
        self.temp_inv = 48.0  # 逆变器温度
        self.temp_mos = 55.0  # MOS管温度
        self.temp_ambient = 32.0  # 环境温度
        self.dc_bus_voltage = 380.0  # 直流母线电压
        self.fan_speed = 60  # 风扇转速 %
        self.efficiency = 90.0  # 效率 %

        # ========== 能量统计 ==========
        self.daily_pv = 8.56  # 当日光伏发电 kWh
        self.total_pv = 1250.3  # 累计光伏发电 kWh
        self.daily_charge = 7.80  # 当日充电 kWh
        self.total_charge = 1100.5  # 累计充电 kWh
        self.daily_discharge = 6.20  # 当日放电 kWh
        self.total_discharge = 980.8  # 累计放电 kWh
        self.daily_load = 6.00  # 当日负载用电 kWh
        self.total_load = 950.2  # 累计负载用电 kWh
        self.runtime_hours = 8640  # 累计运行时间 h

        self._last_update = time.time()

    def tick(self):
        """仿真一步，更新所有参数"""
        now = time.time()
        dt = min(now - self._last_update, 5)
        if dt <= 0:
            return
        self._last_update = now

        # ========== AC 输出波动 ==========
        # 电压在 220V 附近随机波动
        self.voltage += random.gauss(0, 0.5) * dt
        self.voltage = max(200, min(240, self.voltage))

        # 电流在 0~30A 范围内随机波动
        self.current += random.gauss(0, 0.3) * dt
        self.current = max(0, min(30, self.current))

        # 功率因数在 0.95~1.0 之间波动
        self.pf += random.gauss(0, 0.002) * dt
        self.pf = max(0.95, min(1.0, self.pf))

        # 频率微小波动（离网自生成）
        self.frequency += random.gauss(0, 0.005) * dt
        self.frequency = max(49.90, min(50.10, self.frequency))

        # THD 波动
        self.thd_v += random.gauss(0, 0.02) * dt
        self.thd_v = max(1.5, min(3.0, self.thd_v))
        self.thd_i += random.gauss(0, 0.02) * dt
        self.thd_i = max(2.0, min(4.0, self.thd_i))

        # 直流注入波动
        self.dc_injection += random.gauss(0, 0.3) * dt
        self.dc_injection = max(10, min(20, self.dc_injection))

        # 计算功率参数
        self.apparent = self.voltage * self.current
        self.power = self.apparent * self.pf
        self.reactive = math.sqrt(max(0, self.apparent ** 2 - self.power ** 2))
        self.load_percent = self.power / self.rated_power * 100

        # ========== MPPT 波动（模拟云遮挡） ==========
        self.pv_voltage += random.gauss(0, 1.0) * dt
        self.pv_voltage = max(60, min(150, self.pv_voltage))
        self.pv_current += random.gauss(0, 0.2) * dt
        self.pv_current = max(0, min(20, self.pv_current))
        self.pv_power = self.pv_voltage * self.pv_current
        # MPPT 状态偶尔切换
        if random.random() < 0.01:
            self.mppt_state = "scanning"
        elif random.random() < 0.05:
            self.mppt_state = "tracking"

        # ========== BMS 电池波动 ==========
        # 电池电压在 51.2V 附近波动（16S LiFePO4 范围 40~58.4V）
        self.battery_v += random.gauss(0, 0.05) * dt
        self.battery_v = max(40.0, min(58.4, self.battery_v))

        # 电池电流波动（充电为正，放电为负）
        self.battery_i += random.gauss(0, 0.5) * dt
        self.battery_i = max(-60, min(60, self.battery_i))
        self.battery_power = self.battery_v * self.battery_i

        # SOC 缓慢变化
        # 放电时 SOC 下降，充电时 SOC 上升
        # 电池容量 100Ah，电流单位 A，时间单位秒
        soc_delta = -(self.battery_i / 3600.0) * dt / self.capacity_total * 100
        self.soc += soc_delta
        self.soc = max(40, min(95, self.soc))

        # SOH 缓慢衰减
        self.soh -= 0.000001 * dt
        self.soh = max(80, min(100, self.soh))

        # 剩余容量
        self.capacity_remain = self.soc / 100.0 * self.capacity_total

        # 电池温度波动
        self.temp_max += random.gauss(0, 0.05) * dt
        self.temp_max = max(25, min(40, self.temp_max))
        self.temp_min += random.gauss(0, 0.05) * dt
        self.temp_min = max(20, min(35, self.temp_min))
        if self.temp_min > self.temp_max:
            self.temp_min = self.temp_max - 1

        # 单体电压
        self.cell_volt_max += random.gauss(0, 0.005) * dt
        self.cell_volt_max = max(3.20, min(3.65, self.cell_volt_max))
        self.cell_volt_min += random.gauss(0, 0.005) * dt
        self.cell_volt_min = max(3.00, min(3.50, self.cell_volt_min))
        self.cell_volt_diff = self.cell_volt_max - self.cell_volt_min
        self.cell_volt_diff = max(0.05, min(0.10, self.cell_volt_diff))

        # 充放电状态判断
        if self.battery_i > 0.5:
            self.charge_state = "charging"
        elif self.battery_i < -0.5:
            self.charge_state = "discharging"
        else:
            self.charge_state = "idle"

        # ========== 电芯详细数据波动 ==========
        for i in range(self.cell_count):
            self.cell_voltages[i] += random.gauss(0, 0.003) * dt
            self.cell_voltages[i] = max(3.00, min(3.65, self.cell_voltages[i]))
            self.cell_temps[i] += random.gauss(0, 0.02) * dt
            self.cell_temps[i] = max(20, min(40, self.cell_temps[i]))

        # 累计充放电 Ah
        if self.charge_state == "charging":
            self.charge_ah_total += abs(self.battery_i) * dt / 3600.0
        elif self.charge_state == "discharging":
            self.discharge_ah_total += abs(self.battery_i) * dt / 3600.0

        # ========== 系统状态波动 ==========
        self.temp_inv += random.gauss(0, 0.1) * dt
        self.temp_inv = max(30, min(75, self.temp_inv))
        self.temp_mos += random.gauss(0, 0.1) * dt
        self.temp_mos = max(35, min(85, self.temp_mos))
        self.temp_ambient += random.gauss(0, 0.02) * dt
        self.temp_ambient = max(20, min(45, self.temp_ambient))

        # 直流母线电压波动
        self.dc_bus_voltage += random.gauss(0, 0.5) * dt
        self.dc_bus_voltage = max(350, min(410, self.dc_bus_voltage))

        # 风扇转速根据温度调整
        if self.temp_inv > 60:
            self.fan_speed = min(100, self.fan_speed + 1)
        elif self.temp_inv < 40:
            self.fan_speed = max(20, self.fan_speed - 1)

        # 效率计算：AC功率 / (PV功率 + 电池放电功率) * 100
        total_input = self.pv_power + max(0, -self.battery_power)
        if total_input > 0:
            self.efficiency = self.power / total_input * 100
            self.efficiency = max(70, min(96, self.efficiency))

        # ========== 能量累计 ==========
        # 根据功率和时间积分（dt 单位秒，能量单位 kWh）
        pv_energy_inc = self.pv_power * dt / 3600000.0
        self.daily_pv += pv_energy_inc
        self.total_pv += pv_energy_inc

        if self.charge_state == "charging":
            charge_energy_inc = abs(self.battery_power) * dt / 3600000.0
            self.daily_charge += charge_energy_inc
            self.total_charge += charge_energy_inc
        elif self.charge_state == "discharging":
            discharge_energy_inc = abs(self.battery_power) * dt / 3600000.0
            self.daily_discharge += discharge_energy_inc
            self.total_discharge += discharge_energy_inc

        load_energy_inc = self.power * dt / 3600000.0
        self.daily_load += load_energy_inc
        self.total_load += load_energy_inc

        # 运行时间累计
        self.runtime_hours += dt / 3600.0

    def get_param_groups(self):
        """返回参数分组，用于侧边栏切换显示"""
        return {
            "概览": [
                ("工作状态", lambda: self.STATE_MAP.get(self.state, self.state), ""),
                ("设备型号", lambda: self.model, ""),
                ("序列号", lambda: self.sn, ""),
                ("AC输出功率", lambda: f"{self.power:.1f}", "W"),
                ("电池SOC", lambda: f"{self.soc:.1f}", "%"),
                ("PV功率", lambda: f"{self.pv_power:.1f}", "W"),
                ("效率", lambda: f"{self.efficiency:.1f}", "%"),
                ("逆变器温度", lambda: f"{self.temp_inv:.1f}", "°C"),
            ],
            "交流输出": [
                ("输出电压", lambda: f"{self.voltage:.1f}", "V"),
                ("输出电流", lambda: f"{self.current:.2f}", "A"),
                ("有功功率", lambda: f"{self.power:.1f}", "W"),
                ("视在功率", lambda: f"{self.apparent:.1f}", "VA"),
                ("无功功率", lambda: f"{self.reactive:.1f}", "Var"),
                ("输出频率", lambda: f"{self.frequency:.2f}", "Hz"),
                ("功率因数", lambda: f"{self.pf:.3f}", ""),
                ("负载百分比", lambda: f"{self.load_percent:.1f}", "%"),
                ("电压THD", lambda: f"{self.thd_v:.2f}", "%"),
                ("电流THD", lambda: f"{self.thd_i:.2f}", "%"),
                ("直流注入", lambda: f"{self.dc_injection:.0f}", "mA"),
            ],
            "电池BMS": [
                ("SOC", lambda: f"{self.soc:.1f}", "%"),
                ("SOH", lambda: f"{self.soh:.1f}", "%"),
                ("电池电压", lambda: f"{self.battery_v:.2f}", "V"),
                ("电池电流", lambda: f"{self.battery_i:.2f}", "A"),
                ("电池功率", lambda: f"{self.battery_power:.1f}", "W"),
                ("剩余容量", lambda: f"{self.capacity_remain:.2f}", "Ah"),
                ("总容量", lambda: f"{self.capacity_total:.1f}", "Ah"),
                ("循环次数", lambda: f"{self.cycle_count}", "次"),
                ("最高温度", lambda: f"{self.temp_max:.1f}", "°C"),
                ("最低温度", lambda: f"{self.temp_min:.1f}", "°C"),
                ("单体最高电压", lambda: f"{self.cell_volt_max:.3f}", "V"),
                ("单体最低电压", lambda: f"{self.cell_volt_min:.3f}", "V"),
                ("压差", lambda: f"{self.cell_volt_diff:.3f}", "V"),
                ("充放电状态", lambda: {"charging": "充电中", "discharging": "放电中", "idle": "空闲"}.get(self.charge_state, self.charge_state), ""),
                ("电池类型", lambda: self.battery_type, ""),
                ("保护状态1", lambda: f"{self.protect_status1}", ""),
                ("保护状态2", lambda: f"{self.protect_status2}", ""),
            ],
            "光伏MPPT": [
                ("PV电压", lambda: f"{self.pv_voltage:.1f}", "V"),
                ("PV电流", lambda: f"{self.pv_current:.2f}", "A"),
                ("PV功率", lambda: f"{self.pv_power:.1f}", "W"),
                ("MPPT状态", lambda: {"tracking": "跟踪中", "scanning": "扫描中"}.get(self.mppt_state, self.mppt_state), ""),
            ],
            "系统状态": [
                ("工作状态", lambda: self.STATE_MAP.get(self.state, self.state), ""),
                ("故障代码", lambda: f"{self.fault_code}", ""),
                ("告警代码", lambda: f"{self.alarm_code}", ""),
                ("逆变器温度", lambda: f"{self.temp_inv:.1f}", "°C"),
                ("MOS管温度", lambda: f"{self.temp_mos:.1f}", "°C"),
                ("环境温度", lambda: f"{self.temp_ambient:.1f}", "°C"),
                ("直流母线电压", lambda: f"{self.dc_bus_voltage:.1f}", "V"),
                ("风扇转速", lambda: f"{self.fan_speed:.0f}", "%"),
                ("效率", lambda: f"{self.efficiency:.1f}", "%"),
            ],
            "能量统计": [
                ("当日光伏发电", lambda: f"{self.daily_pv:.2f}", "kWh"),
                ("累计光伏发电", lambda: f"{self.total_pv:.1f}", "kWh"),
                ("当日充电", lambda: f"{self.daily_charge:.2f}", "kWh"),
                ("累计充电", lambda: f"{self.total_charge:.1f}", "kWh"),
                ("当日放电", lambda: f"{self.daily_discharge:.2f}", "kWh"),
                ("累计放电", lambda: f"{self.total_discharge:.1f}", "kWh"),
                ("当日负载用电", lambda: f"{self.daily_load:.2f}", "kWh"),
                ("累计负载用电", lambda: f"{self.total_load:.1f}", "kWh"),
                ("累计运行时间", lambda: f"{self.runtime_hours:.1f}", "h"),
            ],
            "电芯详情": [
                ("电芯数量", lambda: f"{self.cell_count}", "节"),
            ] + [
                (f"电芯{i+1}电压", lambda i=i: f"{self.cell_voltages[i]:.3f}", "V")
                for i in range(self.cell_count)
            ] + [
                (f"电芯{i+1}温度", lambda i=i: f"{self.cell_temps[i]:.1f}", "°C")
                for i in range(self.cell_count)
            ] + [
                ("累计充电Ah", lambda: f"{self.charge_ah_total:.1f}", "Ah"),
                ("累计放电Ah", lambda: f"{self.discharge_ah_total:.1f}", "Ah"),
            ],
        }

    def to_json_by_topic(self, topic):
        """按主题生成对应的 JSON payload"""
        if topic == 'info':
            return json.dumps({
                'sn': self.sn,
                'model': self.model,
                'manufacturer': self.manufacturer,
                'firmware_arm': self.firmware_arm,
                'firmware_esp': self.firmware_esp,
                'type': self.type,
                'phase': self.phase,
                'rated_power': self.rated_power,
                'rated_voltage': self.rated_voltage,
                'rated_freq': self.rated_freq,
                'battery_voltage': self.battery_voltage,
                'battery_types': self.battery_types,
                'mppt_count': self.mppt_count,
                'pv_max_voltage': self.pv_max_voltage,
                'pv_max_power': self.pv_max_power,
                'bms_count': self.bms_count,
                'cell_count': self.cell_count,
            }, ensure_ascii=False)
        elif topic == 'ac':
            return json.dumps({
                'voltage': round(self.voltage, 1),
                'current': round(self.current, 2),
                'power': round(self.power, 1),
                'apparent': round(self.apparent, 1),
                'reactive': round(self.reactive, 1),
                'frequency': round(self.frequency, 2),
                'pf': round(self.pf, 3),
                'load_percent': round(self.load_percent, 1),
                'thd_v': round(self.thd_v, 2),
                'thd_i': round(self.thd_i, 2),
                'dc_injection': round(self.dc_injection, 0),
            }, ensure_ascii=False)
        elif topic == 'battery':
            return json.dumps({
                'soc': round(self.soc, 1),
                'soh': round(self.soh, 1),
                'voltage': round(self.battery_v, 2),
                'current': round(self.battery_i, 2),
                'power': round(self.battery_power, 1),
                'capacity_remain': round(self.capacity_remain, 2),
                'capacity_total': self.capacity_total,
                'cycle_count': self.cycle_count,
                'temp_max': round(self.temp_max, 1),
                'temp_min': round(self.temp_min, 1),
                'cell_volt_max': round(self.cell_volt_max, 3),
                'cell_volt_min': round(self.cell_volt_min, 3),
                'cell_volt_diff': round(self.cell_volt_diff, 3),
                'charge_state': self.charge_state,
                'battery_type': self.battery_type,
                'protect_status1': self.protect_status1,
                'protect_status2': self.protect_status2,
            }, ensure_ascii=False)
        elif topic == 'pv':
            return json.dumps({
                'pv_voltage': round(self.pv_voltage, 1),
                'pv_current': round(self.pv_current, 2),
                'pv_power': round(self.pv_power, 1),
                'mppt_state': self.mppt_state,
            }, ensure_ascii=False)
        elif topic == 'status':
            return json.dumps({
                'state': self.state,
                'fault_code': self.fault_code,
                'alarm_code': self.alarm_code,
                'temp_inv': round(self.temp_inv, 1),
                'temp_mos': round(self.temp_mos, 1),
                'temp_ambient': round(self.temp_ambient, 1),
                'dc_bus_voltage': round(self.dc_bus_voltage, 1),
                'fan_speed': self.fan_speed,
                'efficiency': round(self.efficiency, 1),
            }, ensure_ascii=False)
        elif topic == 'energy':
            return json.dumps({
                'daily_pv': round(self.daily_pv, 2),
                'total_pv': round(self.total_pv, 1),
                'daily_charge': round(self.daily_charge, 2),
                'total_charge': round(self.total_charge, 1),
                'daily_discharge': round(self.daily_discharge, 2),
                'total_discharge': round(self.total_discharge, 1),
                'daily_load': round(self.daily_load, 2),
                'total_load': round(self.total_load, 1),
                'runtime_hours': round(self.runtime_hours, 1),
            }, ensure_ascii=False)
        elif topic == 'cells':
            return json.dumps({
                'cell_count': self.cell_count,
                'voltages': [round(v, 3) for v in self.cell_voltages],
                'temps': [round(t, 1) for t in self.cell_temps],
                'charge_ah_total': round(self.charge_ah_total, 1),
                'discharge_ah_total': round(self.discharge_ah_total, 1),
            }, ensure_ascii=False)
        else:
            # 未知主题返回空 JSON
            return json.dumps({}, ensure_ascii=False)

# ==================== 串口管理 ====================
class SerialManager:
    def __init__(self):
        self.port = None
        self.is_open = False
        self._stop = threading.Event()
        self._rx_thread = None
        self._lock = threading.Lock()
        self._rx_queue = deque(maxlen=500)
        self._receiver = FrameReceiver()
        self.on_frame = None
        self.on_status = None

    def list_ports(self):
        if not HAS_SERIAL: return []
        return [{'device': p.device, 'desc': p.description}
                for p in serial.tools.list_ports.comports()]

    def open(self, port, baud=115200):
        if not HAS_SERIAL: return False
        if self.is_open: self.close()
        try:
            self.port = serial.Serial(port, baud, timeout=0.05, write_timeout=1.0)
            self.is_open = True
            self._stop.clear()
            self._receiver.reset()
            self._rx_thread = threading.Thread(target=self._rx_loop, daemon=True)
            self._rx_thread.start()
            if self.on_status: self.on_status(True, port)
            return True
        except Exception as e:
            print(f"打开串口失败: {e}")
            return False

    def close(self):
        if self.is_open:
            self._stop.set()
            if self._rx_thread: self._rx_thread.join(timeout=1)
            try: self.port.close()
            except: pass
            self.port = None
            self.is_open = False
            if self.on_status: self.on_status(False, "")

    def send(self, cmd, data):
        if not self.is_open: return False, "串口未打开"
        try:
            frame, xor = encode_frame(cmd, data)
            with self._lock: self.port.write(frame)
            return True, frame.hex(' ').upper()
        except Exception as e:
            return False, str(e)

    def _rx_loop(self):
        while not self._stop.is_set():
            try:
                if not self.port or not self.port.is_open:
                    time.sleep(0.1); continue
                w = self.port.in_waiting
                if w > 0:
                    for b in self.port.read(min(w, 256)):
                        r = self._receiver.feed(b)
                        if r and self.on_frame:
                            self.on_frame(r)
                else:
                    time.sleep(0.01)
            except:
                time.sleep(0.1)

# ==================== 应用控制器 ====================
class AppController:
    def __init__(self):
        self.serial = SerialManager()
        self.inverter = InverterModel()
        self.auto_sim = True
        self.auto_pub = True
        self.pub_interval = 5.0
        self._last_pub = 0
        self._pub_topic_index = 0
        # 上报主题列表：info 只在首次上报，其余轮询
        self._pub_topics = ['ac', 'battery', 'pv', 'status', 'energy', 'cells']
        self._first_pub_done = False
        self.device_sn = "H1CNA0013500001O"
        self.device_key = "a1b2c3d4e5f6a1b2c3d4e5f6"
        self.mqtt_broker = "jiuxiaoyw.online:1883"
        self.log = deque(maxlen=500)
        self.serial.on_frame = self._on_frame

    def update(self):
        if self.auto_sim: self.inverter.tick()
        if self.auto_pub and self.serial.is_open:
            now = time.time()
            if now - self._last_pub >= self.pub_interval:
                self._last_pub = now
                # 首次上报时先发 info
                if not self._first_pub_done:
                    self._first_pub_done = True
                    self.publish_topic('info')
                # 轮询上报其他主题
                topic = self._pub_topics[self._pub_topic_index % len(self._pub_topics)]
                self._pub_topic_index += 1
                self.publish_topic(topic)

    def publish_topic(self, topic):
        """按主题发布数据"""
        payload = self.inverter.to_json_by_topic(topic)
        obj = {"sn": self.device_sn, "topic": topic, "payload": payload}
        data = json.dumps(obj, ensure_ascii=False).encode('utf-8')
        ok, info = self.serial.send(CMD_PUBLISH, data)
        if ok:
            self.log.append(f"[TX] topic={topic} 已发布")
        else:
            self.log.append(f"[ERR] topic={topic} 发送失败: {info}")
        return ok

    def set_sn(self, sn):
        self.device_sn = sn
        self.inverter.sn = sn
        return self.serial.send(CMD_SET_SN, sn.encode('ascii'))

    def set_key(self, key):
        self.device_key = key
        return self.serial.send(CMD_SET_KEY, key.encode('ascii'))

    def set_broker(self, broker):
        self.mqtt_broker = broker
        return self.serial.send(CMD_SET_BROKER, broker.encode('ascii'))

    def factory_reset(self):
        return self.serial.send(CMD_FACTORY_RESET, b'\x00')

    def _on_frame(self, frame):
        name = frame.get('cmd_name', '')
        valid = frame.get('valid', False)
        data = frame.get('data', b'')
        msg = f"[RX] {name}"
        if not valid: msg += " (校验失败!)"
        if data:
            try:
                s = data.decode('utf-8', 'replace')[:80]
                msg += f" | {s}"
            except:
                msg += f" | {data.hex()[:40]}"
        self.log.append(msg)

# ==================== GUI ====================
class SimulatorGUI:
    def __init__(self, ctrl):
        self.ctrl = ctrl
        ctk.set_appearance_mode("dark")
        ctk.set_default_color_theme("dark-blue")

        self.root = CTk()
        self.root.title("CS-I10-6k2 单相离网48V逆变器 ARM模拟器")
        self.root.geometry("1400x900")
        self.root.configure(fg_color=C['bg'])

        self.current_group = "概览"
        self.param_labels = {}

        self._build_ui()
        self._update_loop()

    def _build_ui(self):
        # 顶部标题栏
        header = CTkFrame(self.root, fg_color=C['card'], height=60, corner_radius=0)
        header.pack(fill='x')
        header.pack_propagate(False)

        CTkLabel(header, text="⚡ CS-I10-6k2 单相离网48V逆变器 ARM模拟器", font=FF(size=18, weight='bold'),
                 text_color=C['t']).pack(side='left', padx=20, pady=10)

        self.status_lbl = CTkLabel(header, text="● 未连接", font=FF(size=12),
                                   text_color=C['t2'])
        self.status_lbl.pack(side='right', padx=20)

        # 主内容区
        main = CTkFrame(self.root, fg_color=C['bg'])
        main.pack(fill='both', expand=True, padx=15, pady=15)

        # 左侧边栏 - 参数分类切换
        sidebar = CTkFrame(main, fg_color=C['card'], corner_radius=12, width=200)
        sidebar.pack(side='left', fill='y', padx=(0, 15))
        sidebar.pack_propagate(False)

        CTkLabel(sidebar, text="参数分类", font=FF(size=15, weight='bold'),
                 text_color=C['t']).pack(anchor='w', padx=15, pady=(15, 10))

        # 分类按钮
        groups = list(self.ctrl.inverter.get_param_groups().keys())
        for grp in groups:
            btn = CTkButton(sidebar, text=grp, height=32, fg_color='transparent',
                           hover_color=C['accent'], text_color=C['t2'],
                           font=FF(size=13),
                           command=lambda g=grp: self._switch_group(g))
            btn.pack(fill='x', padx=10, pady=2)

        # 串口连接区域
        CTkFrame(sidebar, fg_color=C['border'], height=1).pack(fill='x', padx=10, pady=15)

        CTkLabel(sidebar, text="串口连接", font=FF(size=14, weight='bold'),
                 text_color=C['t']).pack(anchor='w', padx=15, pady=(0, 10))

        self.port_combo = CTkComboBox(sidebar, values=[], width=170, fg_color=C['accent'],
                                      border_color=C['border'], button_color=C['highlight'])
        self.port_combo.pack(padx=15, pady=5)
        self._refresh_ports()

        self.conn_btn = CTkButton(sidebar, text="连接", width=170,
                                  fg_color=C['success'], hover_color=C['highlight'],
                                  command=self._on_connect)
        self.conn_btn.pack(padx=15, pady=5)

        CTkButton(sidebar, text="刷新端口", width=170, fg_color=C['accent'],
                  hover_color=C['highlight'], command=self._refresh_ports).pack(padx=15, pady=5)

        # 中间内容区 - 参数显示
        self.content_frame = CTkFrame(main, fg_color=C['card'], corner_radius=12)
        self.content_frame.pack(side='left', fill='both', expand=True, padx=(0, 15))

        # 参数标题
        self.group_title = CTkLabel(self.content_frame, text="概览",
                                    font=FF(size=16, weight='bold'), text_color=C['t'])
        self.group_title.pack(anchor='w', padx=20, pady=(15, 10))

        # 参数列表容器
        self.params_container = CTkScrollableFrame(self.content_frame, fg_color='transparent')
        self.params_container.pack(fill='both', expand=True, padx=15, pady=5)

        # 初始化显示概览
        self._switch_group("概览")

        # 右侧面板 - 设置和日志
        right = CTkFrame(main, fg_color=C['card'], corner_radius=12, width=350)
        right.pack(side='right', fill='both', expand=True)

        # 日志区域放在最上面
        self._build_log_panel(right)

        # 设置和命令放在可滚动区域
        self._build_settings_panel(right)

    def _build_settings_panel(self, parent):
        # 设置和命令区域占据剩余空间，可滚动
        scroll = CTkScrollableFrame(parent, fg_color='transparent')
        scroll.pack(fill='both', expand=True, padx=15, pady=(5, 15))

        frm = CTkFrame(scroll, fg_color='transparent')
        frm.pack(fill='x')

        CTkLabel(frm, text="设备参数", font=FF(size=14, weight='bold'),
                 text_color=C['t']).pack(anchor='w', pady=(0, 10))

        self.entries = {}
        fields = [
            ('设备 SN:', 'sn', self.ctrl.device_sn),
            ('密钥 PIN:', 'key', self.ctrl.device_key),
            ('Broker:',   'broker', self.ctrl.mqtt_broker),
        ]
        for lb, key, dv in fields:
            row = CTkFrame(frm, fg_color='transparent')
            row.pack(fill='x', pady=5)
            CTkLabel(row, text=lb, width=80, anchor='e',
                     text_color=C['t2'], font=FF(size=11)).pack(side='left', padx=(0, 8))
            e = CTkEntry(row, height=34, fg_color=C['accent'], border_color=C['border'],
                         text_color=C['t'], font=FF(family='Consolas', size=12))
            e.insert(0, dv)
            e.pack(side='left', fill='x', expand=True)
            self.entries[key] = e

        CTkButton(frm, text="应用设置", fg_color=C['accent'],
                  hover_color=C['highlight'], command=self._apply_settings).pack(fill='x', pady=10)

        # 仿真选项
        opt_frm = CTkFrame(frm, fg_color='transparent')
        opt_frm.pack(fill='x', pady=5)
        self.sim_chk = ctk.CTkCheckBox(opt_frm, text="自动仿真", font=FF(size=12),
                                       checkbox_width=20, checkbox_height=20,
                                       fg_color=C['success'])
        self.sim_chk.pack(side='left', padx=5)
        self.sim_chk.select()
        self.pub_chk = ctk.CTkCheckBox(opt_frm, text="自动上报", font=FF(size=12),
                                       checkbox_width=20, checkbox_height=20,
                                       fg_color=C['success'])
        self.pub_chk.pack(side='left', padx=20)
        self.pub_chk.select()

        # 快捷命令
        CTkLabel(frm, text="快捷命令", font=FF(size=14, weight='bold'),
                 text_color=C['t']).pack(anchor='w', pady=(15, 10))

        cmds = [
            ("发布设备信息 (info)", lambda: self._pub_topic('info')),
            ("交流输出 (ac)", lambda: self._pub_topic('ac')),
            ("电池数据 (battery)", lambda: self._pub_topic('battery')),
            ("光伏数据 (pv)", lambda: self._pub_topic('pv')),
            ("系统状态 (status)", lambda: self._pub_topic('status')),
            ("能量统计 (energy)", lambda: self._pub_topic('energy')),
            ("电芯详情 (cells)", lambda: self._pub_topic('cells')),
            ("恢复出厂", self._factory_reset),
        ]
        for txt, cmd in cmds:
            CTkButton(frm, text=txt, height=30, fg_color=C['accent'],
                      hover_color=C['highlight'], font=FF(size=12),
                      command=cmd).pack(fill='x', pady=2)

    def _build_log_panel(self, parent):
        # 日志区域固定高度
        frm = CTkFrame(parent, fg_color='transparent', height=220)
        frm.pack(fill='x', padx=15, pady=(15, 10))
        frm.pack_propagate(False)

        log_header = CTkFrame(frm, fg_color='transparent')
        log_header.pack(fill='x', pady=(0, 8))
        CTkLabel(log_header, text="通信日志", font=FF(size=14, weight='bold'),
                 text_color=C['t']).pack(side='left')
        CTkButton(log_header, text="清空", width=50, height=26,
                  fg_color=C['accent'], hover_color=C['highlight'],
                  font=FF(size=11), command=self._clear_log).pack(side='right')

        self.log_txt = CTkTextbox(frm, height=180, fg_color=C['card2'],
                                  border_color=C['border'], text_color=C['t'],
                                  font=FF(family='Consolas', size=13),
                                  spacing3=2)
        self.log_txt.pack(fill='both', expand=True)
        self.log_txt.configure(state='disabled')

    def _clear_log(self):
        self.log_txt.configure(state='normal')
        self.log_txt.delete('1.0', 'end')
        self.log_txt.configure(state='disabled')

    def _switch_group(self, group_name):
        """切换参数分组显示"""
        self.current_group = group_name
        self.group_title.configure(text=group_name)

        # 清空当前显示
        for widget in self.params_container.winfo_children():
            widget.destroy()
        self.param_labels.clear()

        # 获取参数组
        groups = self.ctrl.inverter.get_param_groups()
        if group_name not in groups:
            return

        params = groups[group_name]

        # 创建参数显示行
        for name, getter, unit in params:
            row = CTkFrame(self.params_container, fg_color=C['card2'], corner_radius=6, height=36)
            row.pack(fill='x', pady=3, padx=5)
            row.pack_propagate(False)

            CTkLabel(row, text=name, width=160, anchor='w', padx=10,
                     text_color=C['t2'], font=FF(size=13)).pack(side='left', pady=6)

            val_lbl = CTkLabel(row, text="--", width=140, anchor='e',
                               text_color=C['success'], font=FF(family='Consolas', size=14, weight='bold'))
            val_lbl.pack(side='left', padx=(0, 5), pady=6)

            if unit:
                CTkLabel(row, text=unit, width=50, anchor='w',
                         text_color=C['t3'], font=FF(size=12)).pack(side='left', pady=6)

            self.param_labels[name] = (val_lbl, getter)

    def _refresh_ports(self):
        ports = self.ctrl.serial.list_ports()
        vals = [p['device'] for p in ports]
        self.port_combo.configure(values=vals)
        if vals:
            self.port_combo.set(vals[0])

    def _on_connect(self):
        if self.ctrl.serial.is_open:
            self.ctrl.serial.close()
            self.conn_btn.configure(text="连接", fg_color=C['success'])
            self.status_lbl.configure(text="● 未连接", text_color=C['t3'])
            self._add_log("[INFO] 串口已断开")
        else:
            port = self.port_combo.get()
            if port and self.ctrl.serial.open(port):
                self.conn_btn.configure(text="断开", fg_color=C['warning'])
                self.status_lbl.configure(text=f"● 已连接 {port}", text_color=C['success'])
                self._add_log(f"[INFO] 串口已连接: {port}")
                self._apply_settings()
            elif port:
                self._add_log(f"[ERR] 无法打开串口: {port}")

    def _apply_settings(self):
        sn = self.entries['sn'].get()
        key = self.entries['key'].get()
        broker = self.entries['broker'].get()

        self.ctrl.device_sn = sn
        self.ctrl.device_key = key
        self.ctrl.mqtt_broker = broker
        self.ctrl.inverter.sn = sn

        if self.ctrl.serial.is_open:
            self.ctrl.set_sn(sn)
            self.ctrl.set_key(key)
            self.ctrl.set_broker(broker)
            self._add_log(f"设置已应用: SN={sn}")

    def _pub_topic(self, topic):
        if self.ctrl.serial.is_open:
            self.ctrl.publish_topic(topic)
        else:
            self._add_log(f"[WARN] 请先连接串口，无法发送 {topic}")

    def _factory_reset(self):
        if self.ctrl.serial.is_open:
            self.ctrl.factory_reset()
            self._add_log("已发送恢复出厂命令")

    def _add_log(self, msg):
        try:
            ts = datetime.now().strftime('%H:%M:%S')
            self.log_txt.configure(state='normal')
            self.log_txt.insert('end', f"[{ts}] {msg}\n")
            self.log_txt.see('end')
            self.log_txt.configure(state='disabled')
        except Exception as e:
            print(f"日志写入失败: {e}")

    def _update_loop(self):
        # 更新仿真开关
        self.ctrl.auto_sim = self.sim_chk.get()
        self.ctrl.auto_pub = self.pub_chk.get()

        self.ctrl.update()

        # 更新当前显示的参数
        for name, (lbl, getter) in self.param_labels.items():
            try:
                val = getter()
                lbl.configure(text=val)
            except:
                pass

        # 更新日志
        while self.ctrl.log:
            self._add_log(self.ctrl.log.popleft())

        self.root.after(200, self._update_loop)

    def run(self):
        self.root.mainloop()

# ==================== 入口 ====================
def main():
    if not HAS_CTK:
        print("请先安装 customtkinter: pip install customtkinter")
        return
    ctrl = AppController()
    app = SimulatorGUI(ctrl)
    app.run()

if __name__ == '__main__':
    main()
