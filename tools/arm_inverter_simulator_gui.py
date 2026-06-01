#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
GD32F303 ARM芯片完整模拟器 - 辰烁科技 CS_INV
=============================================
模拟完整的GD32F303微控制器，包括：
- UART通信协议处理 (ESP32连接)
- Modbus RTU主站 (DSP/BMS通信)
- IAP Bootloader OTA升级
- 逆变器控制逻辑
- GUI可视化界面

用法:
  python arm_inverter_simulator_gui.py          # 启动GUI
  python arm_inverter_simulator_gui.py --test   # 运行单元测试
"""

import sys, os, time, json, threading, random, math, struct, queue, hashlib
from datetime import datetime
from collections import deque

# ==================== 协议常量 ====================
FRAME_HEADER, FRAME_ESCAPE = 0xAA, 0x55
FRAME_ESC_HEADER, FRAME_ESC_ESCAPE = 0x01, 0x00
FRAME_DATA_MAX = 512

CMD_SET_BROKER       = 0x01
CMD_PUBLISH          = 0x02
CMD_COMMAND_RECV     = 0x03
CMD_COMMAND_SEND     = 0x04
CMD_FACTORY_RESET    = 0x05
CMD_HEARTBEAT        = 0x06
CMD_SET_SN           = 0x08
CMD_SET_KEY          = 0x09
CMD_SET_AP_SSID      = 0x0A
CMD_ARM_OTA_START    = 0x10
CMD_ARM_OTA_DATA     = 0x11
CMD_ARM_OTA_END      = 0x12
CMD_ARM_OTA_ACK      = 0x13
CMD_ARM_OTA_NACK     = 0x14
CMD_ARM_OTA_INFO     = 0x15
CMD_ACK              = 0xFE
CMD_NACK             = 0xFF

MODBUS_FUNC_READ_HOLDING  = 0x03
MODBUS_FUNC_READ_INPUT    = 0x04
MODBUS_FUNC_WRITE_SINGLE  = 0x06
MODBUS_FUNC_WRITE_MULTIPLE = 0x10

CMD_NAMES = {
    0x01: "SET_BROKER", 0x02: "PUBLISH", 0x03: "COMMAND_RECV",
    0x04: "COMMAND_SEND", 0x05: "FACTORY_RESET", 0x06: "HEARTBEAT",
    0x08: "SET_SN", 0x09: "SET_KEY", 0x0A: "SET_AP_SSID",
    0x10: "ARM_OTA_START", 0x11: "ARM_OTA_DATA", 0x12: "ARM_OTA_END",
    0x13: "ARM_OTA_ACK", 0x14: "ARM_OTA_NACK", 0x15: "ARM_OTA_INFO",
    0xFE: "ACK", 0xFF: "NACK",
}


# ==================== 帧编解码 ====================
def calc_xor(cmd, data):
    x = FRAME_HEADER ^ cmd
    dl = len(data)
    x ^= (dl >> 8) & 0xFF; x ^= dl & 0xFF
    for b in data: x ^= b
    return x & 0xFF


def encode_frame(cmd, data):
    xor_val = calc_xor(cmd, data)
    dl = len(data)
    raw = bytearray([FRAME_HEADER, cmd, (dl >> 8) & 0xFF, dl & 0xFF])
    raw.extend(data); raw.append(xor_val)
    escaped = bytearray([FRAME_HEADER])
    for b in raw[1:]:
        if b == FRAME_HEADER: escaped.extend([FRAME_ESCAPE, FRAME_ESC_HEADER])
        elif b == FRAME_ESCAPE: escaped.extend([FRAME_ESCAPE, FRAME_ESC_ESCAPE])
        else: escaped.append(b)
    return bytes(escaped), xor_val


class FrameReceiver:
    RX_WAIT_HEADER, RX_RECV_CMD, RX_RECV_LEN_H, RX_RECV_LEN_L, \
        RX_RECV_DATA, RX_RECV_XOR = range(6)

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
                self.state = self.RX_RECV_CMD; self.rx_xor = FRAME_HEADER
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
            return {
                'cmd': self.cmd, 'cmd_name': CMD_NAMES.get(self.cmd, f"UNK_{self.cmd:02X}"),
                'length': self.data_len, 'data': bytes(self.data_buf[:self.data_len]),
                'xor': b, 'expected_xor': self.rx_xor, 'valid': (b == self.rx_xor),
                'timestamp': datetime.now()
            }
        return None


# ==================== Modbus RTU 主站 ====================
class ModbusRTUMaster:
    """Modbus RTU 主站模拟 (ARM作为主站查询DSP/BMS从站)"""

    def __init__(self):
        self._lock = threading.Lock()
        self.slaves = {
            1: {'name': 'DSP', 'enabled': True, 'response_delay': 0.01},
            2: {'name': 'BMS', 'enabled': True, 'response_delay': 0.02},
        }
        self._holding_regs = {}
        self._input_regs = {}
        self.poll_interval = 1.0
        self.last_poll_time = 0
        self._init_registers()

    def _init_registers(self):
        # DSP寄存器 (0x0000-0x001F)
        self._holding_regs[0x0000] = 0x0001  # DSP运行状态
        self._holding_regs[0x0001] = 5000    # 输出频率 x100
        self._holding_regs[0x0002] = 2200    # 输出电压 x10
        self._holding_regs[0x0003] = 1000    # 输出电流 x100
        self._holding_regs[0x0004] = 2200    # 输出功率 W
        self._holding_regs[0x0005] = 0       # 故障代码
        self._holding_regs[0x0006] = 3800    # 直流母线电压 x10
        self._holding_regs[0x0007] = 450     # DSP温度 x10
        self._holding_regs[0x0008] = 960     # 效率 x10
        self._holding_regs[0x0009] = 6200    # 额定功率 W
        self._holding_regs[0x000A] = 2200    # 额定电压 x10
        self._holding_regs[0x000B] = 500     # 额定频率 x10
        # BMS寄存器 (0x0100-0x011F)
        self._holding_regs[0x0100] = 0x0002  # BMS运行状态
        self._holding_regs[0x0101] = 750     # SOC x10
        self._holding_regs[0x0102] = 512     # 电池电压 x100
        self._holding_regs[0x0103] = 0       # 电池电流 x100
        self._holding_regs[0x0104] = 0x0000  # 充电状态
        self._holding_regs[0x0105] = 1600    # 电池容量 Ah x10
        self._holding_regs[0x0106] = 1200    # 剩余容量 Ah x10
        self._holding_regs[0x0107] = 280     # 最高温度 x10
        self._holding_regs[0x0108] = 250     # 最低温度 x10
        self._holding_regs[0x0109] = 335     # 单体最高电压 x1000
        self._holding_regs[0x010A] = 328     # 单体最低电压 x1000
        self._holding_regs[0x010B] = 152     # 循环次数
        # MPPT寄存器 (0x0200-0x021F)
        self._holding_regs[0x0200] = 0x0001  # MPPT状态
        self._holding_regs[0x0201] = 8500    # PV电压 x10
        self._holding_regs[0x0202] = 1200    # PV电流 x100
        self._holding_regs[0x0203] = 10200   # PV功率 W
        self._holding_regs[0x0204] = 15000   # PV最大电压 x10
        self._holding_regs[0x0205] = 20000   # PV最大功率 W

    def crc16(self, data):
        crc = 0xFFFF
        for b in data:
            crc ^= b
            for _ in range(8):
                if crc & 0x0001: crc = (crc >> 1) ^ 0xA001
                else: crc >>= 1
        return struct.pack('<H', crc)

    def build_read_holding(self, slave_id, addr, count):
        frame = bytes([slave_id, MODBUS_FUNC_READ_HOLDING]) + struct.pack('>H', addr) + struct.pack('>H', count)
        return frame + self.crc16(frame)

    def build_read_input(self, slave_id, addr, count):
        frame = bytes([slave_id, MODBUS_FUNC_READ_INPUT]) + struct.pack('>H', addr) + struct.pack('>H', count)
        return frame + self.crc16(frame)

    def build_write_single(self, slave_id, addr, value):
        frame = bytes([slave_id, MODBUS_FUNC_WRITE_SINGLE]) + struct.pack('>H', addr) + struct.pack('>H', value)
        return frame + self.crc16(frame)

    def parse_response(self, frame):
        if len(frame) < 5: return None
        data = frame[:-2]
        crc_received = struct.unpack('<H', frame[-2:])[0]
        crc_calc = struct.unpack('<H', self.crc16(data))[0]
        if crc_received != crc_calc: return None
        slave_id, func_code = data[0], data[1]
        if func_code in (MODBUS_FUNC_READ_HOLDING, MODBUS_FUNC_READ_INPUT):
            byte_count = data[2]
            values = [struct.unpack('>H', data[3 + i * 2:5 + i * 2])[0] for i in range(byte_count // 2)]
            return {'slave': slave_id, 'func': func_code, 'values': values}
        elif func_code == MODBUS_FUNC_WRITE_SINGLE:
            return {'slave': slave_id, 'func': func_code,
                    'addr': struct.unpack('>H', data[2:4])[0],
                    'value': struct.unpack('>H', data[4:6])[0]}
        return None

    def tick(self, dt):
        self._holding_regs[0x0003] = int(1000 + random.gauss(0, 50))
        self._holding_regs[0x0004] = int(2200 + random.gauss(0, 100))
        self._holding_regs[0x0102] = int(5120 + random.gauss(0, 20))
        self._holding_regs[0x0103] = int(random.gauss(0, 500))
        self._holding_regs[0x0101] = int(750 + random.gauss(0, 5))
        self._holding_regs[0x0202] = int(1200 + random.gauss(0, 30))
        self._holding_regs[0x0203] = int(self._holding_regs[0x0201] * self._holding_regs[0x0202] / 1000)

    def get_register(self, addr):
        return self._holding_regs.get(addr, 0)

    def get_all_data(self):
        return dict(self._holding_regs)


# ==================== GD32F303 IAP Bootloader ====================
class GD32F303Bootloader:
    STATE_IDLE, STATE_IAP_READY, STATE_RECEIVING, STATE_VERIFYING, STATE_JUMPING, STATE_ERROR = range(6)
    STATE_NAMES = {0: "正常模式", 1: "IAP就绪", 2: "接收中", 3: "校验中", 4: "跳转中", 5: "错误"}
    PACKET_SIZE = 480

    def __init__(self):
        self.state = self.STATE_IDLE
        self.firmware = bytearray()
        self.total_size = 0
        self.total_packets = 0
        self.current_packet = 0
        self.received_packets = set()
        self.md5_expected = "0" * 32
        self.firmware_version = "V1.2.3.20240510"
        self.max_flash_size = 256 * 1024
        self._last_activity = time.time()
        self.on_progress = None

    def reset(self):
        self.state = self.STATE_IDLE
        self.firmware = bytearray()
        self.total_size = self.total_packets = self.current_packet = 0
        self.received_packets = set()

    def handle_start(self, data):
        if len(data) < 5: return False, CMD_ARM_OTA_NACK, b'\x00\x00'
        self.total_size = struct.unpack('<I', data[0:4])[0]
        self.md5_expected = data[4:20].hex()
        version_len = data[20] if len(data) > 20 else 0
        self.firmware_version = data[21:21 + version_len].decode('ascii', errors='ignore') if version_len > 0 and len(data) > 21 else "V1.0.0"
        if self.total_size == 0 or self.total_size > self.max_flash_size:
            return False, CMD_ARM_OTA_NACK, b'\x00\x01'
        self.firmware = bytearray()
        self.current_packet = 0
        self.total_packets = (self.total_size + self.PACKET_SIZE - 1) // self.PACKET_SIZE
        self.received_packets = set()
        self.state = self.STATE_IAP_READY
        self._last_activity = time.time()
        if self.on_progress: self.on_progress(0, "进入IAP模式，等待数据...")
        return True, CMD_ARM_OTA_ACK, b''

    def handle_data(self, data):
        if self.state == self.STATE_IDLE: return False, None, None
        if len(data) < 3: return False, CMD_ARM_OTA_NACK, struct.pack('<H', self.current_packet)
        packet_idx = struct.unpack('<H', data[0:2])[0]
        packet_data = data[2:]
        if packet_idx != self.current_packet:
            return False, CMD_ARM_OTA_NACK, struct.pack('<H', self.current_packet)
        self.firmware.extend(packet_data)
        self.received_packets.add(packet_idx)
        self.current_packet += 1
        self.state = self.STATE_RECEIVING
        self._last_activity = time.time()
        if self.on_progress:
            percent = min(99, int(len(self.firmware) * 100 / self.total_size))
            self.on_progress(percent, f"接收固件 {len(self.firmware)}/{self.total_size} bytes")
        time.sleep(0.005)  # 模拟Flash写入
        return True, CMD_ARM_OTA_ACK, struct.pack('<H', packet_idx)

    def handle_end(self, data):
        if self.state not in (self.STATE_RECEIVING, self.STATE_IAP_READY): return False, None, None
        if len(data) < 22: return False, None, None
        total_packets = struct.unpack('<H', data[0:2])[0]
        total_size = struct.unpack('<I', data[2:6])[0]
        md5_received = data[6:22].hex()
        self.state = self.STATE_VERIFYING
        self._last_activity = time.time()
        time.sleep(0.1)  # 模拟校验
        if total_size != self.total_size:
            self.state = self.STATE_ERROR; return False, CMD_ARM_OTA_NACK, b'\x00\x02'
        if len(self.firmware) != self.total_size:
            self.state = self.STATE_ERROR; return False, CMD_ARM_OTA_NACK, struct.pack('<H', self.current_packet - 1)
        self.state = self.STATE_JUMPING
        time.sleep(0.2)  # 模拟跳转
        self.state = self.STATE_IDLE
        return True, CMD_ARM_OTA_ACK, b''

    def handle_info_request(self, data):
        version_bytes = self.firmware_version.encode('ascii')
        fw_size = len(self.firmware) if self.firmware else 128 * 1024
        md5_sim = hashlib.md5(self.firmware).hexdigest() if self.firmware else "0" * 32
        resp = bytes([len(version_bytes)]) + version_bytes + struct.pack('<I', fw_size) + md5_sim.encode('ascii')
        return True, resp

    def is_in_ota_mode(self): return self.state != self.STATE_IDLE
    def get_state_name(self): return self.STATE_NAMES.get(self.state, "未知")


# ==================== 逆变器控制器 ====================
class InverterController:
    """基于物理模型的逆变器模拟器
    
    能量守恒关系：
    - PV发电 → AC负载 + 电池充电（多余）/ 电池放电（不足）
    - 电池功率 = PV功率 - AC功率（正值=充电，负值=放电）
    - 电池电流 = 电池功率 / 电池电压
    - SOC变化 = -电池电流 * dt / 容量 * 100
    """
    STATE_MAP = {'inverting': '逆变运行', 'standby': '待机', 'fault': '故障', 'shutdown': '关机', 'bypass': '旁路'}

    def __init__(self, modbus_master):
        self.modbus = modbus_master
        self.sn = "H1CNA00135000014"
        self.model = "CS-I10-6k2"
        self.manufacturer = "辰烁科技"
        self.firmware_arm = "V1.2.3.20240510"
        self.firmware_esp = "V1.0.5.20240420"
        self.type = "离网逆变器"
        self.rated_power = 6200  # 额定功率 6.2kW
        self.rated_voltage = 220
        self.rated_freq = 50.0
        self.battery_voltage = 51.2  # 48V 锂电池
        self.battery_capacity = 100.0  # 100Ah
        self.battery_type = "LiFePO4"
        self.cell_count = 16
        self.state = "inverting"
        self.fault_code = self.alarm_code = 0
        
        # ========== 设备参数 ==========
        self.voltage = 220.0
        self.current = 10.0
        self.power = 2200.0
        self.frequency = 50.00
        self.load_percent = 35.5
        
        # ========== 电池参数 ==========
        self.soc = 75.0  # 初始 SOC 75%
        self.soh = 96.0
        self.battery_v = 51.2
        self.battery_i = 0.0
        self.charge_state = "idle"
        
        # ========== PV参数 ==========
        self.pv_voltage = 85.0
        self.pv_current = 12.0
        self.pv_power = 1020.0
        self.mppt_state = "tracking"
        
        # ========== 能量统计 ==========
        self.daily_pv = 8.56; self.total_pv = 1250.3
        self.daily_charge = 7.80; self.total_charge = 1100.5
        self.daily_discharge = 6.20; self.total_discharge = 980.8
        self.runtime_hours = 8640
        
        # ========== 温度 ==========
        self.temp_inv = 48.0; self.temp_mos = 55.0; self.temp_ambient = 32.0
        self.efficiency = 90.0
        
        # ========== 电芯数据 ==========
        self.cell_voltages = [3.30 + random.uniform(-0.02, 0.02) for _ in range(self.cell_count)]
        self.cell_temps = [26.0 + random.uniform(-1.0, 2.0) for _ in range(self.cell_count)]
        
        # ========== 用户可调模拟参数 ==========
        self.sim_params = {
            'target_pv_power': 1500.0,   # PV目标功率 (W)
            'target_ac_power': 2200.0,   # AC目标功率 (W)
            'pv_volatility': 0.02,       # PV波动幅度 (0-0.1)
            'ac_volatility': 0.03,      # AC波动幅度 (0-0.1)
            'ambient_temp': 32.0,        # 环境温度 (°C)
            'inverter_efficiency': 0.92, # 逆变效率 (0-1)
            'initial_soc': 75.0,         # 初始SOC (%)
            'battery_capacity': 100.0,   # 电池容量 (Ah)
        }
        
        self._last_update = time.time()

    def set_sim_param(self, key, value):
        """设置模拟参数"""
        if key in self.sim_params:
            self.sim_params[key] = value
            # 同步更新初始SOC
            if key == 'initial_soc':
                self.soc = value
            # 同步更新电池容量
            if key == 'battery_capacity':
                self.battery_capacity = value

    def get_sim_params(self):
        """获取所有模拟参数"""
        return dict(self.sim_params)

    def _calc_battery_voltage(self, soc):
        """根据SOC计算电池电压 (LiFePO4特性曲线)
        
        LiFePO4 16S 电池电压特性：
        - 100% SOC: ~58.4V
        - 50% SOC: ~51.2V (标称电压)
        - 0% SOC: ~44.8V
        """
        # 简化线性模型（实际是非线性的）
        v_min = 44.8  # 最低电压
        v_max = 58.4  # 最高电压
        return v_min + (v_max - v_min) * (soc / 100.0)

    def _calc_pv_current(self, pv_power, pv_voltage):
        """根据PV功率和电压计算电流"""
        if pv_voltage > 0:
            return pv_power / pv_voltage
        return 0.0

    def tick(self, dt):
        """基于能量守恒的物理模拟"""
        now = time.time()
        dt = min(now - self._last_update, 5)  # 最大5秒步长
        if dt <= 0: return
        self._last_update = now
        
        p = self.sim_params
        
        # ========== 1. 计算目标PV功率（带小幅度波动） ==========
        pv_noise = 1.0 + random.gauss(0, p['pv_volatility'])
        target_pv = p['target_pv_power'] * pv_noise
        target_pv = max(0, min(2000, target_pv))  # 限制范围 0-2000W
        
        # ========== 2. 计算目标AC功率（带小幅度波动） ==========
        ac_noise = 1.0 + random.gauss(0, p['ac_volatility'])
        target_ac = p['target_ac_power'] * ac_noise
        target_ac = max(0, min(self.rated_power, target_ac))  # 限制范围 0-6200W
        
        # ========== 3. 能量守恒计算电池功率 ==========
        # 电池功率 = PV功率 - AC功率
        # 正值 = 充电（多余能量存电池）
        # 负值 = 放电（从电池补充不足）
        battery_power = target_pv - target_ac  # 单位：W
        
        # 考虑逆变效率：AC输出 = 电池输入 * 效率
        if battery_power > 0:
            # 充电：PV多余能量给电池
            battery_power_input = battery_power * p['inverter_efficiency']
        else:
            # 放电：电池输出供AC
            battery_power_input = battery_power / p['inverter_efficiency']
        
        # ========== 4. 计算电池电压和电流 ==========
        self.battery_v = self._calc_battery_voltage(self.soc)
        if self.battery_v > 0:
            self.battery_i = battery_power_input / self.battery_v  # A
        else:
            self.battery_i = 0.0
        
        # 限制电池电流范围 (-60A ~ 60A)
        self.battery_i = max(-60, min(60, self.battery_i))
        
        # ========== 5. 更新SOC ==========
        # SOC变化 = -电池电流 * dt(秒) / 容量(Ah) * 100
        soc_delta = -(self.battery_i * dt / 3600.0) / self.battery_capacity * 100
        self.soc += soc_delta
        self.soc = max(20, min(100, self.soc))  # SOC限制 20%-100%
        
        # 充放电状态
        if self.battery_i > 1.0:
            self.charge_state = "charging"
        elif self.battery_i < -1.0:
            self.charge_state = "discharging"
        else:
            self.charge_state = "idle"
        
        # ========== 6. 计算AC输出 ==========
        # AC功率 = min(目标功率, 受限于电池和PV能力)
        self.power = min(target_ac, self.rated_power)
        # 电压稳定在220V附近
        self.voltage = 220.0 + random.gauss(0, 1.0)
        self.voltage = max(210, min(230, self.voltage))
        # 电流 = 功率 / 电压
        self.current = self.power / self.voltage if self.voltage > 0 else 0
        self.load_percent = self.power / self.rated_power * 100
        self.frequency = 50.0 + random.gauss(0, 0.05)
        self.frequency = max(49.5, min(50.5, self.frequency))
        
        # ========== 7. 计算PV参数 ==========
        # PV电压根据功率动态调整（模拟MPPT追踪）
        if target_pv > 0:
            # 功率一定时，电压高则电流低
            self.pv_voltage = 70.0 + (target_pv / 2000.0) * 50.0  # 70-120V
            self.pv_voltage += random.gauss(0, 2.0)  # 小幅波动
            self.pv_voltage = max(60, min(150, self.pv_voltage))
            self.pv_current = self._calc_pv_current(target_pv, self.pv_voltage)
        else:
            self.pv_voltage = 0
            self.pv_current = 0
        self.pv_power = target_pv
        
        # MPPT状态
        if self.pv_power > 100:
            self.mppt_state = "tracking"
        elif self.pv_power > 10:
            self.mppt_state = "scanning"
        else:
            self.mppt_state = "standby"
        
        # ========== 8. 能量统计 ==========
        pv_energy = self.pv_power * dt / 3600000.0  # kWh
        self.daily_pv += pv_energy; self.total_pv += pv_energy
        
        if self.charge_state == "charging":
            bat_energy = abs(self.battery_i * self.battery_v) * dt / 3600000.0
            self.daily_charge += bat_energy
            self.total_charge += bat_energy
        elif self.charge_state == "discharging":
            bat_energy = abs(self.battery_i * self.battery_v) * dt / 3600000.0
            self.daily_discharge += bat_energy
            self.total_discharge += bat_energy
        
        # ========== 9. 温度计算 ==========
        # 逆变器温度：与环境温度 + 负载发热
        load_factor = self.power / self.rated_power
        heat_generated = load_factor * 20.0  # 负载越大发热越多
        temp_target = p['ambient_temp'] + 15 + heat_generated
        self.temp_inv += (temp_target - self.temp_inv) * dt * 0.1  # 缓慢趋近目标
        self.temp_inv = max(25, min(75, self.temp_inv + random.gauss(0, 0.2)))
        
        # MOS温度比逆变器温度略高
        self.temp_mos = self.temp_inv + random.uniform(3, 8)
        self.temp_mos = max(30, min(85, self.temp_mos))
        
        # 环境温度缓慢变化
        self.temp_ambient = p['ambient_temp'] + random.gauss(0, 0.1)
        
        # ========== 10. 效率计算 ==========
        # 效率 = AC输出功率 / (PV功率 + 电池放电功率)
        total_input = self.pv_power + max(0, -battery_power)
        if total_input > 10:
            self.efficiency = (self.power / total_input) * 100
            self.efficiency = max(70, min(96, self.efficiency))
        else:
            self.efficiency = 90.0
        
        # ========== 11. 更新电芯数据 ==========
        base_volt = 3.30 + (self.soc / 100.0) * 0.15
        for i in range(self.cell_count):
            self.cell_voltages[i] = base_volt + random.gauss(0, 0.01)
            self.cell_voltages[i] = max(3.0, min(3.65, self.cell_voltages[i]))
            self.cell_temps[i] = self.temp_inv / self.cell_count + random.uniform(-1, 2)
            self.cell_temps[i] = max(20, min(45, self.cell_temps[i]))
        
        # ========== 12. 运行时间 ==========
        self.runtime_hours += dt / 3600.0

    def _update_from_modbus(self):
        dsp_state = self.modbus.get_register(0x0000)
        self.state = "inverting" if dsp_state == 1 else "standby"
        self.frequency = self.modbus.get_register(0x0001) / 100.0
        self.voltage = self.modbus.get_register(0x0002) / 10.0
        self.current = self.modbus.get_register(0x0003) / 100.0
        self.power = self.modbus.get_register(0x0004)
        self.soc = self.modbus.get_register(0x0101) / 10.0
        self.battery_v = self.modbus.get_register(0x0102) / 100.0
        self.battery_i = self.modbus.get_register(0x0103) / 100.0
        cs = self.modbus.get_register(0x0104)
        self.charge_state = {1: "charging", 2: "discharging"}.get(cs, "idle")
        self.pv_voltage = self.modbus.get_register(0x0201) / 10.0
        self.pv_current = self.modbus.get_register(0x0202) / 100.0
        self.pv_power = self.modbus.get_register(0x0203)

    def to_json(self, topic):
        if topic == 'info':
            return {'sn': self.sn, 'model': self.model, 'manufacturer': self.manufacturer,
                    'firmware_arm': self.firmware_arm, 'firmware_esp': self.firmware_esp,
                    'type': self.type, 'rated_power': self.rated_power, 'rated_voltage': self.rated_voltage,
                    'rated_freq': self.rated_freq, 'battery_voltage': self.battery_voltage,
                    'battery_type': self.battery_type, 'cell_count': self.cell_count}
        elif topic == 'ac':
            return {'voltage': round(self.voltage, 1), 'current': round(self.current, 2),
                    'power': round(self.power, 1), 'frequency': round(self.frequency, 2),
                    'load_percent': round(self.load_percent, 1)}
        elif topic == 'battery':
            return {'soc': round(self.soc, 1), 'soh': round(self.soh, 1),
                    'voltage': round(self.battery_v, 2), 'current': round(self.battery_i, 2),
                    'charge_state': self.charge_state}
        elif topic == 'pv':
            return {'pv_voltage': round(self.pv_voltage, 1), 'pv_current': round(self.pv_current, 2),
                    'pv_power': round(self.pv_power, 1), 'mppt_state': self.mppt_state}
        elif topic == 'status':
            return {'state': self.state, 'fault_code': self.fault_code, 'alarm_code': self.alarm_code,
                    'temp_inv': round(self.temp_inv, 1), 'temp_mos': round(self.temp_mos, 1),
                    'efficiency': round(self.efficiency, 1)}
        elif topic == 'energy':
            return {'daily_pv': round(self.daily_pv, 2), 'total_pv': round(self.total_pv, 1),
                    'runtime_hours': round(self.runtime_hours, 1)}
        return {}


# ==================== GD32F303 芯片模拟器 ====================
class GD32F303Chip:
    """GD32F303 微控制器完整模拟"""

    def __init__(self):
        self.modbus_master = ModbusRTUMaster()
        self.bootloader = GD32F303Bootloader()
        self.inverter = InverterController(self.modbus_master)
        self.serial_port = None
        self.serial_baud = 115200
        self._serial_lock = threading.Lock()
        self._running = False
        self._rx_thread = None
        self._frame_receiver = FrameReceiver()
        self.connected = False
        self.last_heartbeat = 0
        self.heartbeat_timeout = 30.0
        self.ota_progress = 0
        self.ota_status = "空闲"
        self.ota_target = ""
        self.log_queue = deque(maxlen=500)
        self.bootloader.on_progress = self._on_ota_progress
        self.on_frame = None
        self.on_log = None

    def _log(self, msg):
        ts = datetime.now().strftime('%H:%M:%S')
        log_msg = f"[{ts}] {msg}"
        self.log_queue.append(log_msg)
        if self.on_log: self.on_log(log_msg)

    def _on_ota_progress(self, percent, message):
        self.ota_progress = percent; self.ota_status = message
        self._log(f"[OTA] {message}")

    # ---- 串口接口 ----
    def serial_open(self, port, baud=115200):
        try:
            import serial
            self.serial_port = serial.Serial(port, baud, timeout=0.05, write_timeout=1.0)
            self.serial_baud = baud; self._running = True; self._frame_receiver.reset()
            self._rx_thread = threading.Thread(target=self._serial_rx_loop, daemon=True)
            self._rx_thread.start(); self.connected = True
            self._log(f"串口已打开: {port} @ {baud}bps")
            return True
        except Exception as e:
            self._log(f"串口打开失败: {e}"); return False

    def serial_close(self):
        self._running = False
        if self._rx_thread: self._rx_thread.join(timeout=1)
        if self.serial_port:
            try: self.serial_port.close()
            except: pass
            self.serial_port = None
        self.connected = False; self._log("串口已关闭")

    def serial_send(self, cmd, data):
        if not self.serial_port or not self.serial_port.is_open: return False, "串口未打开"
        try:
            frame, xor = encode_frame(cmd, data)
            with self._serial_lock: self.serial_port.write(frame)
            return True, frame.hex(' ').upper()
        except Exception as e: return False, str(e)

    def _serial_rx_loop(self):
        while self._running:
            try:
                if not self.serial_port or not self.serial_port.is_open:
                    time.sleep(0.1); continue
                w = self.serial_port.in_waiting
                if w > 0:
                    for b in self.serial_port.read(min(w, 256)):
                        frame = self._frame_receiver.feed(b)
                        if frame: self._handle_frame(frame)
                else: time.sleep(0.01)
            except Exception as e:
                self._log(f"串口接收错误: {e}"); time.sleep(0.1)

    # ---- 帧处理 ----
    def _handle_frame(self, frame):
        cmd, data, valid = frame['cmd'], frame['data'], frame['valid']
        name = CMD_NAMES.get(cmd, f"UNK_{cmd:02X}")
        if not valid:
            self._log(f"[RX] {name} (校验失败!)"); self.serial_send(CMD_NACK, b'\x01'); return
        self._log(f"[RX] {name} ({len(data)} bytes)")
        if cmd == CMD_ARM_OTA_START: self._handle_ota_start(data)
        elif cmd == CMD_ARM_OTA_DATA: self._handle_ota_data(data)
        elif cmd == CMD_ARM_OTA_END: self._handle_ota_end(data)
        elif cmd == CMD_ARM_OTA_INFO: self._handle_ota_info(data)
        else: self._handle_normal_command(cmd, data)
        if self.on_frame: self.on_frame(frame)

    def _handle_normal_command(self, cmd, data):
        if cmd == CMD_HEARTBEAT:
            self.last_heartbeat = time.time()
            self.serial_send(CMD_ACK, b''); self._log("[TX] HEARTBEAT ACK")
        elif cmd == CMD_SET_SN:
            try: self.inverter.sn = data.decode('ascii'); self._log(f"设置SN: {self.inverter.sn}"); self.serial_send(CMD_ACK, b'')
            except: self.serial_send(CMD_NACK, b'')
        elif cmd == CMD_SET_KEY:
            self._log(f"设置密钥"); self.serial_send(CMD_ACK, b'')
        elif cmd == CMD_SET_BROKER:
            self._log(f"设置Broker"); self.serial_send(CMD_ACK, b'')
        elif cmd == CMD_PUBLISH: self._handle_publish(data)
        elif cmd == CMD_FACTORY_RESET:
            self.bootloader.reset(); self._log("恢复出厂设置"); self.serial_send(CMD_ACK, b'')
        elif cmd == CMD_COMMAND_RECV:
            self._log(f"收到远程命令"); self.serial_send(CMD_ACK, b'')
        elif cmd == CMD_ACK:
            pass  # 已在 _handle_frame 记录，不回复
        elif cmd == CMD_NACK:
            pass  # 已在 _handle_frame 记录，不回复
        else:
            self._log(f"未知命令: {cmd:02X}"); self.serial_send(CMD_NACK, b'')

    def _handle_publish(self, data):
        try:
            obj = json.loads(data.decode('utf-8'))
            topic = obj.get('topic', '')
            topic_map = {'/info': 'info', '/ac': 'ac', '/battery': 'battery',
                         '/pv': 'pv', '/status': 'status', '/energy': 'energy'}
            for suffix, key in topic_map.items():
                if topic.endswith(suffix):
                    obj['payload'] = json.dumps(self.inverter.to_json(key), ensure_ascii=False); break
            if topic.endswith('/cells'):
                obj['payload'] = json.dumps({'cell_count': self.inverter.cell_count,
                    'voltages': [round(v, 3) for v in self.inverter.cell_voltages],
                    'temps': [round(t, 1) for t in self.inverter.cell_temps]}, ensure_ascii=False)
            self._log(f"ARM上报: {topic}"); self.serial_send(CMD_ACK, b'\x00')
        except Exception as e:
            self._log(f"PUBLISH处理错误: {e}"); self.serial_send(CMD_NACK, b'\x01')

    # ---- OTA处理 ----
    def _handle_ota_start(self, data):
        self.ota_target = "ARM"; self.ota_status = "收到OTA_START"; self.ota_progress = 0
        self._log("[OTA] 收到固件升级请求")
        ok, resp_cmd, resp_data = self.bootloader.handle_start(data)
        if ok:
            self.ota_status = f"IAP就绪 ({self.bootloader.total_packets} packets)"
            self._log(f"[OTA] 进入IAP模式，总大小={self.bootloader.total_size} bytes")
        else:
            self.ota_status = "OTA_START失败"; self._log("[OTA] OTA_START 失败")
        if resp_cmd: self.serial_send(resp_cmd, resp_data)

    def _handle_ota_data(self, data):
        if not self.bootloader.is_in_ota_mode():
            self._log("[OTA] 忽略：未在OTA模式"); return
        self.ota_target = "ARM"
        ok, resp_cmd, resp_data = self.bootloader.handle_data(data)
        if ok:
            self.ota_progress = min(99, int(len(self.bootloader.firmware) * 100 / self.bootloader.total_size))
            self.ota_status = f"接收中 {self.ota_progress}%"
        else:
            self.ota_status = "数据包错误"; self._log("[OTA] DATA 失败")
        if resp_cmd: self.serial_send(resp_cmd, resp_data)

    def _handle_ota_end(self, data):
        if not self.bootloader.is_in_ota_mode():
            self._log("[OTA] 忽略：未在OTA模式"); return
        self.ota_target = "ARM"; self.ota_status = "固件传输完成，校验中..."
        self._log("[OTA] 固件传输完成，校验中...")
        ok, resp_cmd, resp_data = self.bootloader.handle_end(data)
        if ok:
            self.ota_progress = 100; self.ota_status = "升级成功!"
            self._log(f"[OTA] ✅ 固件升级成功! ({len(self.bootloader.firmware)} bytes)")
            if self.bootloader.firmware_version:
                self.inverter.firmware_arm = self.bootloader.firmware_version
                self._log(f"[OTA] ARM固件版本: {self.inverter.firmware_arm}")
        else:
            self.ota_status = "校验失败"; self._log("[OTA] ❌ 固件校验失败")
        if resp_cmd: self.serial_send(resp_cmd, resp_data)

    def _handle_ota_info(self, data):
        ok, resp_data = self.bootloader.handle_info_request(data)
        if ok:
            self.serial_send(CMD_ARM_OTA_INFO, resp_data)
            self._log(f"[OTA] 回复固件信息: {self.bootloader.firmware_version}")

    # ---- 主循环 ----
    def tick(self, dt):
        self.modbus_master.tick(dt); self.inverter.tick(dt)
        if self.connected and time.time() - self.last_heartbeat > self.heartbeat_timeout:
            if self.last_heartbeat > 0: self._log("心跳超时!")
            self.last_heartbeat = time.time()

    def reset_ota(self):
        self.bootloader.reset(); self.ota_progress = 0; self.ota_status = "空闲"; self.ota_target = ""
        self._log("[OTA] OTA状态已重置")

    def factory_reset(self):
        self.bootloader.reset(); self.ota_progress = 0; self.ota_status = "空闲"
        self._log("恢复出厂设置完成")

    def get_status(self):
        return {'connected': self.connected, 'ota_progress': self.ota_progress,
                'ota_status': self.ota_status, 'ota_target': self.ota_target,
                'bootloader_state': self.bootloader.get_state_name(),
                'inverter_state': self.inverter.state,
                'firmware_version': self.inverter.firmware_arm}


# ==================== GUI ====================
try:
    import customtkinter as ctk
    from customtkinter import CTk, CTkFrame, CTkLabel, CTkButton, CTkEntry, CTkComboBox
    from customtkinter import CTkCheckBox, CTkTextbox, CTkScrollableFrame
    HAS_CTK = True
except ImportError:
    HAS_CTK = False

try:
    import serial
    import serial.tools.list_ports
    HAS_SERIAL = True
except ImportError:
    HAS_SERIAL = False

C = {
    'bg': '#1a1b26', 'card': '#24283b', 'card2': '#1f2335', 'accent': '#3b4261',
    'highlight': '#7aa2f7', 't': '#c0caf5', 't2': '#a9b1d6', 't3': '#565f89',
    'border': '#3b4261', 'success': '#9ece6a', 'warning': '#e0af68', 'error': '#f7768e',
    'info': '#7dcfff', 'ota': '#bb9af7',
}

def FF(family='Microsoft YaHei', size=12, weight='normal'):
    return ctk.CTkFont(family=family, size=size, weight=weight)


class SimulatorGUI:
    def __init__(self):
        self.chip = GD32F303Chip()
        ctk.set_appearance_mode("dark"); ctk.set_default_color_theme("dark-blue")
        self.root = CTk()
        self.root.title("CS-I10-6k2 ARM模拟器 (GD32F303 完整芯片仿真)")
        self.root.geometry("1500x920"); self.root.configure(fg_color=C['bg'])
        self.current_group = "概览"; self.param_labels = {}
        self.auto_sim = True; self.auto_pub = True; self.pub_interval = 5.0
        self._last_pub = 0; self._pub_topic_index = 0
        self._pub_topics = ['info', 'ac', 'battery', 'pv', 'status', 'energy']
        self._first_pub_done = False
        self._build_ui(); self._update_loop()

    def _build_ui(self):
        # 顶部标题栏
        header = CTkFrame(self.root, fg_color=C['card'], height=60, corner_radius=0)
        header.pack(fill='x'); header.pack_propagate(False)
        CTkLabel(header, text="⚡ CS-I10-6k2 ARM模拟器 (GD32F303 完整芯片仿真)",
                 font=FF(size=16, weight='bold'), text_color=C['t']).pack(side='left', padx=20, pady=10)
        self.chip_status_lbl = CTkLabel(header, text="● 芯片: 正常", font=FF(size=12), text_color=C['success'])
        self.chip_status_lbl.pack(side='right', padx=(0, 20))
        self.status_lbl = CTkLabel(header, text="● 串口: 未连接", font=FF(size=12), text_color=C['t3'])
        self.status_lbl.pack(side='right', padx=(0, 15))

        # OTA 状态栏
        self.ota_bar = CTkFrame(self.root, fg_color=C['ota'], height=36, corner_radius=0)
        self.ota_bar.pack(fill='x'); self.ota_bar.pack_propagate(False)
        self.ota_lbl = CTkLabel(self.ota_bar, text="🔄 OTA状态: 空闲", font=FF(size=13), text_color='#fff')
        self.ota_lbl.pack(side='left', padx=15, pady=5)
        self.ota_progress_lbl = CTkLabel(self.ota_bar, text="0%", font=FF(size=13, weight='bold'), text_color='#fff')
        self.ota_progress_lbl.pack(side='right', padx=15, pady=5)

        # 主内容区
        main = CTkFrame(self.root, fg_color=C['bg']); main.pack(fill='both', expand=True, padx=15, pady=15)

        # 左侧边栏
        sidebar = CTkFrame(main, fg_color=C['card'], corner_radius=12, width=200)
        sidebar.pack(side='left', fill='y', padx=(0, 15)); sidebar.pack_propagate(False)
        CTkLabel(sidebar, text="参数分类", font=FF(size=15, weight='bold'), text_color=C['t']).pack(anchor='w', padx=15, pady=(15, 10))
        for grp in ["概览", "交流输出", "电池BMS", "光伏MPPT", "系统状态", "能量统计", "模拟参数", "OTA升级", "Modbus寄存器"]:
            if grp == "模拟参数":
                color = C['info']
            elif grp == "OTA升级":
                color = C['ota']
            else:
                color = C['t2']
            CTkButton(sidebar, text=grp, height=32, fg_color='transparent', hover_color=C['accent'],
                      text_color=color, font=FF(size=13),
                      command=lambda g=grp: self._switch_group(g)).pack(fill='x', padx=10, pady=2)

        CTkFrame(sidebar, fg_color=C['border'], height=1).pack(fill='x', padx=10, pady=15)
        CTkLabel(sidebar, text="串口连接 (ESP32)", font=FF(size=14, weight='bold'), text_color=C['t']).pack(anchor='w', padx=15, pady=(0, 10))
        self.port_combo = CTkComboBox(sidebar, values=[], width=170, fg_color=C['accent'], border_color=C['border'], button_color=C['highlight'])
        self.port_combo.pack(padx=15, pady=5); self._refresh_ports()
        self.conn_btn = CTkButton(sidebar, text="连接", width=170, fg_color=C['success'], hover_color=C['highlight'], command=self._on_connect)
        self.conn_btn.pack(padx=15, pady=5)
        CTkButton(sidebar, text="刷新端口", width=170, fg_color=C['accent'], hover_color=C['highlight'], command=self._refresh_ports).pack(padx=15, pady=5)

        # 中间内容区
        self.content_frame = CTkFrame(main, fg_color=C['card'], corner_radius=12)
        self.content_frame.pack(side='left', fill='both', expand=True, padx=(0, 15))
        self.group_title = CTkLabel(self.content_frame, text="概览", font=FF(size=16, weight='bold'), text_color=C['t'])
        self.group_title.pack(anchor='w', padx=20, pady=(15, 10))
        self.params_container = CTkScrollableFrame(self.content_frame, fg_color='transparent')
        self.params_container.pack(fill='both', expand=True, padx=15, pady=5)
        self._switch_group("概览")

        # 右侧面板
        right = CTkFrame(main, fg_color=C['card'], corner_radius=12, width=380)
        right.pack(side='right', fill='both', expand=True)
        self._build_log_panel(right); self._build_settings_panel(right)

    def _build_settings_panel(self, parent):
        scroll = CTkScrollableFrame(parent, fg_color='transparent')
        scroll.pack(fill='both', expand=True, padx=15, pady=(5, 15))
        frm = CTkFrame(scroll, fg_color='transparent'); frm.pack(fill='x')
        CTkLabel(frm, text="设备参数", font=FF(size=14, weight='bold'), text_color=C['t']).pack(anchor='w', pady=(0, 10))
        self.entries = {}
        for lb, key, dv in [('设备 SN:', 'sn', self.chip.inverter.sn),
                             ('密钥 PIN:', 'key', 'a1b2c3d4e5f6a1b2c3d4e5f6'),
                             ('Broker:', 'broker', 'jiuxiaoyw.online:8883')]:
            row = CTkFrame(frm, fg_color='transparent'); row.pack(fill='x', pady=5)
            CTkLabel(row, text=lb, width=80, anchor='e', text_color=C['t2'], font=FF(size=11)).pack(side='left', padx=(0, 8))
            e = CTkEntry(row, height=34, fg_color=C['accent'], border_color=C['border'], text_color=C['t'], font=FF(family='Consolas', size=12))
            e.insert(0, dv); e.pack(side='left', fill='x', expand=True); self.entries[key] = e
        CTkButton(frm, text="应用设置", fg_color=C['accent'], hover_color=C['highlight'], command=self._apply_settings).pack(fill='x', pady=10)

        opt_frm = CTkFrame(frm, fg_color='transparent'); opt_frm.pack(fill='x', pady=5)
        self.sim_chk = ctk.CTkCheckBox(opt_frm, text="自动仿真", font=FF(size=12), checkbox_width=20, checkbox_height=20, fg_color=C['success'])
        self.sim_chk.pack(side='left', padx=5); self.sim_chk.select()
        self.pub_chk = ctk.CTkCheckBox(opt_frm, text="自动上报", font=FF(size=12), checkbox_width=20, checkbox_height=20, fg_color=C['success'])
        self.pub_chk.pack(side='left', padx=20); self.pub_chk.select()

        # ========== 模拟参数调节 ==========
        self._build_sim_params_panel(frm)

        CTkLabel(frm, text="快捷命令", font=FF(size=14, weight='bold'), text_color=C['t']).pack(anchor='w', pady=(15, 10))
        for txt, cmd in [("发布设备信息 (info)", lambda: self._pub_topic('info')),
                         ("交流输出 (ac)", lambda: self._pub_topic('ac')),
                         ("电池数据 (battery)", lambda: self._pub_topic('battery')),
                         ("光伏数据 (pv)", lambda: self._pub_topic('pv')),
                         ("系统状态 (status)", lambda: self._pub_topic('status')),
                         ("能量统计 (energy)", lambda: self._pub_topic('energy')),
                         ("查询固件信息 (OTA_INFO)", self._query_ota_info),
                         ("恢复出厂", self._factory_reset),
                         ("重置OTA状态", self._reset_ota)]:
            CTkButton(frm, text=txt, height=30, fg_color=C['accent'], hover_color=C['highlight'], font=FF(size=12), command=cmd).pack(fill='x', pady=2)

        ota_frame = CTkFrame(frm, fg_color=C['card2'], corner_radius=8); ota_frame.pack(fill='x', pady=(15, 0))
        CTkLabel(ota_frame, text="OTA 命令支持", font=FF(size=13, weight='bold'), text_color=C['ota']).pack(anchor='w', padx=10, pady=(8, 5))
        CTkLabel(ota_frame, text="0x10: OTA_START | 0x11: OTA_DATA\n0x12: OTA_END | 0x13-0x15: ACK/NACK/INFO\n\n"
                                  "ESP32 通过串口发送 0x10 进入IAP模式\n然后逐包发送 0x11 固件数据\n最后发送 0x12 完成校验跳转",
                 font=FF(size=11), text_color=C['t3']).pack(anchor='w', padx=10, pady=(0, 10))

    def _build_log_panel(self, parent):
        frm = CTkFrame(parent, fg_color='transparent', height=240)
        frm.pack(fill='x', padx=15, pady=(15, 10)); frm.pack_propagate(False)
        log_header = CTkFrame(frm, fg_color='transparent'); log_header.pack(fill='x', pady=(0, 8))
        CTkLabel(log_header, text="通信日志", font=FF(size=14, weight='bold'), text_color=C['t']).pack(side='left')
        CTkButton(log_header, text="清空", width=50, height=26, fg_color=C['accent'], hover_color=C['highlight'], font=FF(size=11), command=self._clear_log).pack(side='right')
        self.log_txt = CTkTextbox(frm, height=200, fg_color=C['card2'], border_color=C['border'], text_color=C['t'], font=FF(family='Consolas', size=12), spacing3=2)
        self.log_txt.pack(fill='both', expand=True); self.log_txt.configure(state='disabled')

    def _clear_log(self):
        self.log_txt.configure(state='normal'); self.log_txt.delete('1.0', 'end'); self.log_txt.configure(state='disabled')

    def _get_param_groups(self):
        inv, bl, modbus = self.chip.inverter, self.chip.bootloader, self.chip.modbus_master
        return {
            "概览": [("工作状态", lambda: {'inverting': '逆变运行', 'standby': '待机', 'fault': '故障'}.get(inv.state, inv.state), ""),
                     ("设备型号", lambda: inv.model, ""), ("序列号", lambda: inv.sn, ""),
                     ("ARM固件版本", lambda: inv.firmware_arm, ""),
                     ("AC输出功率", lambda: f"{inv.power:.1f}", "W"), ("电池SOC", lambda: f"{inv.soc:.1f}", "%"),
                     ("PV功率", lambda: f"{inv.pv_power:.1f}", "W"), ("效率", lambda: f"{inv.efficiency:.1f}", "%")],
            "交流输出": [("输出电压", lambda: f"{inv.voltage:.1f}", "V"), ("输出电流", lambda: f"{inv.current:.2f}", "A"),
                         ("有功功率", lambda: f"{inv.power:.1f}", "W"), ("输出频率", lambda: f"{inv.frequency:.2f}", "Hz"),
                         ("负载百分比", lambda: f"{inv.load_percent:.1f}", "%")],
            "电池BMS": [("SOC", lambda: f"{inv.soc:.1f}", "%"), ("SOH", lambda: f"{inv.soh:.1f}", "%"),
                        ("电池电压", lambda: f"{inv.battery_v:.2f}", "V"), ("电池电流", lambda: f"{inv.battery_i:.2f}", "A"),
                        ("充放电状态", lambda: {'charging': '充电中', 'discharging': '放电中', 'idle': '空闲'}.get(inv.charge_state, inv.charge_state), "")],
            "光伏MPPT": [("PV电压", lambda: f"{inv.pv_voltage:.1f}", "V"), ("PV电流", lambda: f"{inv.pv_current:.2f}", "A"),
                         ("PV功率", lambda: f"{inv.pv_power:.1f}", "W"),
                         ("MPPT状态", lambda: {'tracking': '跟踪中', 'scanning': '扫描中', 'standby': '待机'}.get(inv.mppt_state, inv.mppt_state), "")],
            "系统状态": [("工作状态", lambda: {'inverting': '逆变运行', 'standby': '待机'}.get(inv.state, inv.state), ""),
                         ("故障代码", lambda: f"{inv.fault_code}", ""), ("逆变器温度", lambda: f"{inv.temp_inv:.1f}", "°C"),
                         ("MOS管温度", lambda: f"{inv.temp_mos:.1f}", "°C"), ("效率", lambda: f"{inv.efficiency:.1f}", "%")],
            "能量统计": [("当日光伏发电", lambda: f"{inv.daily_pv:.2f}", "kWh"), ("累计光伏发电", lambda: f"{inv.total_pv:.1f}", "kWh"),
                         ("累计运行时间", lambda: f"{inv.runtime_hours:.1f}", "h")],
            "模拟参数": self._get_sim_params_page(),
            "OTA升级": [("Bootloader状态", lambda: bl.get_state_name(), ""), ("固件版本", lambda: inv.firmware_arm, ""),
                        ("已接收固件", lambda: f"{len(bl.firmware)}", "bytes"),
                        ("OTA进度", lambda: f"{self.chip.ota_progress}", "%"),
                        ("OTA模式", lambda: "是" if bl.is_in_ota_mode() else "否", "")],
            "Modbus寄存器": self._get_modbus_params(),
        }

    def _get_sim_params_page(self):
        """获取模拟参数页面的参数列表"""
        inv = self.chip.inverter
        p = inv.sim_params
        
        # 计算能量状态
        diff = p['target_pv_power'] - p['target_ac_power']
        if diff > 0:
            energy_status = f"☀️ 充电中 (+{diff:.0f}W)"
        elif diff < 0:
            energy_status = f"🔋 放电中 ({diff:.0f}W)"
        else:
            energy_status = "⚖️ 平衡"
        
        return [
            ("── 核心参数 ──", lambda: "", ""),
            ("PV目标功率", lambda: f"{p['target_pv_power']:.0f}", "W"),
            ("AC目标功率", lambda: f"{p['target_ac_power']:.0f}", "W"),
            ("能量状态", lambda: energy_status, ""),
            ("── 环境参数 ──", lambda: "", ""),
            ("环境温度", lambda: f"{p['ambient_temp']:.0f}", "°C"),
            ("逆变效率", lambda: f"{p['inverter_efficiency']*100:.0f}", "%"),
            ("── 电池参数 ──", lambda: "", ""),
            ("初始SOC", lambda: f"{p['initial_soc']:.0f}", "%"),
            ("电池容量", lambda: f"{p['battery_capacity']:.0f}", "Ah"),
            ("当前SOC", lambda: f"{inv.soc:.1f}", "%"),
            ("电池电压", lambda: f"{inv.battery_v:.2f}", "V"),
            ("电池电流", lambda: f"{inv.battery_i:.1f}", "A"),
            ("── 波动参数 ──", lambda: "", ""),
            ("PV波动幅度", lambda: f"{p['pv_volatility']*100:.1f}", "%"),
            ("AC波动幅度", lambda: f"{p['ac_volatility']*100:.1f}", "%"),
        ]

    def _get_modbus_params(self):
        m = self.chip.modbus_master
        return [("── DSP (从站1) ──", lambda: "", ""),
                ("DSP运行状态", lambda: f"0x{m.get_register(0x0000):04X}", ""),
                ("输出频率", lambda: f"{m.get_register(0x0001)/100:.2f}", "Hz"),
                ("输出电压", lambda: f"{m.get_register(0x0002)/10:.1f}", "V"),
                ("输出电流", lambda: f"{m.get_register(0x0003)/100:.2f}", "A"),
                ("输出功率", lambda: f"{m.get_register(0x0004)}", "W"),
                ("直流母线电压", lambda: f"{m.get_register(0x0006)/10:.1f}", "V"),
                ("── BMS (从站2) ──", lambda: "", ""),
                ("BMS状态", lambda: f"0x{m.get_register(0x0100):04X}", ""),
                ("SOC", lambda: f"{m.get_register(0x0101)/10:.1f}", "%"),
                ("电池电压", lambda: f"{m.get_register(0x0102)/100:.2f}", "V"),
                ("充电状态", lambda: {0: '空闲', 1: '充电', 2: '放电'}.get(m.get_register(0x0104), '未知'), ""),
                ("── MPPT ──", lambda: "", ""),
                ("PV电压", lambda: f"{m.get_register(0x0201)/10:.1f}", "V"),
                ("PV电流", lambda: f"{m.get_register(0x0202)/100:.2f}", "A"),
                ("PV功率", lambda: f"{m.get_register(0x0203)}", "W")]

    def _switch_group(self, group_name):
        self.current_group = group_name; self.group_title.configure(text=group_name)
        for w in self.params_container.winfo_children(): w.destroy()
        self.param_labels.clear()
        groups = self._get_param_groups()
        if group_name not in groups: return
        for name, getter, unit in groups[group_name]:
            if name.startswith("──"):
                row = CTkFrame(self.params_container, fg_color='transparent', height=28)
                row.pack(fill='x', pady=(8, 2), padx=5); row.pack_propagate(False)
                CTkLabel(row, text=name, text_color=C['highlight'], font=FF(size=12, weight='bold')).pack(anchor='w', padx=10, pady=4)
                continue
            row = CTkFrame(self.params_container, fg_color=C['card2'], corner_radius=6, height=36)
            row.pack(fill='x', pady=3, padx=5); row.pack_propagate(False)
            color = C['ota'] if 'OTA' in name else C['t2']
            CTkLabel(row, text=name, width=160, anchor='w', padx=10, text_color=color, font=FF(size=13)).pack(side='left', pady=6)
            val_lbl = CTkLabel(row, text="--", width=140, anchor='e', text_color=C['success'], font=FF(family='Consolas', size=14, weight='bold'))
            val_lbl.pack(side='left', padx=(0, 5), pady=6)
            if unit:
                CTkLabel(row, text=unit, width=50, anchor='w', text_color=C['t3'], font=FF(size=12)).pack(side='left', pady=6)
            self.param_labels[name] = (val_lbl, getter)

    def _refresh_ports(self):
        if not HAS_SERIAL: return
        vals = [p.device for p in serial.tools.list_ports.comports()]
        self.port_combo.configure(values=vals)
        if vals: self.port_combo.set(vals[0])

    def _on_connect(self):
        if self.chip.connected:
            self.chip.serial_close()
            self.conn_btn.configure(text="连接", fg_color=C['success'])
            self.status_lbl.configure(text="● 串口: 未连接", text_color=C['t3'])
            self._add_log("[INFO] 串口已断开")
        else:
            port = self.port_combo.get()
            if port and self.chip.serial_open(port):
                self.conn_btn.configure(text="断开", fg_color=C['warning'])
                self.status_lbl.configure(text=f"● 串口: {port}", text_color=C['success'])
                self._add_log(f"[INFO] 串口已连接: {port}"); self._apply_settings()
            elif port: self._add_log(f"[ERR] 无法打开串口: {port}")

    def _apply_settings(self):
        sn, key, broker = self.entries['sn'].get(), self.entries['key'].get(), self.entries['broker'].get()
        self.chip.inverter.sn = sn
        if self.chip.connected:
            self.chip.serial_send(CMD_SET_SN, sn.encode('ascii'))
            self.chip.serial_send(CMD_SET_KEY, key.encode('ascii'))
            self.chip.serial_send(CMD_SET_BROKER, broker.encode('ascii'))
            self._add_log(f"设置已应用: SN={sn}")

    def _build_sim_params_panel(self, parent):
        """构建模拟参数调节面板
        
        能量守恒模型：
        - PV发电量 → AC负载 + 电池充电（多余）/ 电池放电（不足）
        """
        inv = self.chip.inverter
        
        # 标题和说明
        title_frm = CTkFrame(parent, fg_color='transparent')
        title_frm.pack(fill='x', pady=(15, 5))
        CTkLabel(title_frm, text="🔧 模拟参数调节", font=FF(size=14, weight='bold'), text_color=C['info']).pack(anchor='w')
        
        # 能量流向说明
        flow_frm = CTkFrame(parent, fg_color=C['card2'], corner_radius=6)
        flow_frm.pack(fill='x', pady=(5, 10))
        CTkLabel(flow_frm, text="能量守恒: PV发电 → AC负载 + 电池充放电",
                 font=FF(size=10), text_color=C['t3']).pack(anchor='w', padx=10, pady=5)
        
        self.sim_widgets = {}  # 保存控件引用
        
        # ========== PV功率调节 ==========
        pv_frm = CTkFrame(parent, fg_color='transparent')
        pv_frm.pack(fill='x', pady=3)
        CTkLabel(pv_frm, text="☀️ PV功率:", width=90, anchor='w', text_color=C['success'], font=FF(size=12)).pack(side='left')
        
        pv_val_frm = CTkFrame(pv_frm, fg_color='transparent')
        pv_val_frm.pack(side='left', fill='x', expand=True, padx=(5, 0))
        
        self.sim_widgets['pv_slider'] = ctk.CTkSlider(pv_val_frm, from_=0, to=2000, number_of_steps=40,
                                                       command=lambda v, k='target_pv_power': self._on_sim_param_change(k, v))
        self.sim_widgets['pv_slider'].pack(side='left', fill='x', expand=True)
        self.sim_widgets['pv_slider'].set(inv.sim_params['target_pv_power'])
        
        self.sim_widgets['pv_label'] = CTkLabel(pv_val_frm, text=f"{inv.sim_params['target_pv_power']:.0f}W",
                                                  width=70, anchor='e', text_color=C['success'], font=FF(size=12, weight='bold'))
        self.sim_widgets['pv_label'].pack(side='left', padx=(5, 0))
        
        # ========== AC负载调节 ==========
        ac_frm = CTkFrame(parent, fg_color='transparent')
        ac_frm.pack(fill='x', pady=3)
        CTkLabel(ac_frm, text="🔌 AC负载:", width=90, anchor='w', text_color=C['warning'], font=FF(size=12)).pack(side='left')
        
        ac_val_frm = CTkFrame(ac_frm, fg_color='transparent')
        ac_val_frm.pack(side='left', fill='x', expand=True, padx=(5, 0))
        
        self.sim_widgets['ac_slider'] = ctk.CTkSlider(ac_val_frm, from_=0, to=6200, number_of_steps=62,
                                                        command=lambda v, k='target_ac_power': self._on_sim_param_change(k, v))
        self.sim_widgets['ac_slider'].pack(side='left', fill='x', expand=True)
        self.sim_widgets['ac_slider'].set(inv.sim_params['target_ac_power'])
        
        self.sim_widgets['ac_label'] = CTkLabel(ac_val_frm, text=f"{inv.sim_params['target_ac_power']:.0f}W",
                                                  width=70, anchor='e', text_color=C['warning'], font=FF(size=12, weight='bold'))
        self.sim_widgets['ac_label'].pack(side='left', padx=(5, 0))
        
        # ========== 电池SOC调节 ==========
        soc_frm = CTkFrame(parent, fg_color='transparent')
        soc_frm.pack(fill='x', pady=3)
        CTkLabel(soc_frm, text="🔋 电池SOC:", width=90, anchor='w', text_color=C['info'], font=FF(size=12)).pack(side='left')
        
        soc_val_frm = CTkFrame(soc_frm, fg_color='transparent')
        soc_val_frm.pack(side='left', fill='x', expand=True, padx=(5, 0))
        
        self.sim_widgets['soc_slider'] = ctk.CTkSlider(soc_val_frm, from_=20, to=100, number_of_steps=80,
                                                        command=lambda v, k='initial_soc': self._on_sim_param_change(k, v))
        self.sim_widgets['soc_slider'].pack(side='left', fill='x', expand=True)
        self.sim_widgets['soc_slider'].set(inv.sim_params['initial_soc'])
        
        self.sim_widgets['soc_label'] = CTkLabel(soc_val_frm, text=f"{inv.sim_params['initial_soc']:.0f}%",
                                                   width=70, anchor='e', text_color=C['info'], font=FF(size=12, weight='bold'))
        self.sim_widgets['soc_label'].pack(side='left', padx=(5, 0))
        
        # ========== 环境温度调节 ==========
        temp_frm = CTkFrame(parent, fg_color='transparent')
        temp_frm.pack(fill='x', pady=3)
        CTkLabel(temp_frm, text="🌡️ 环境温度:", width=90, anchor='w', text_color=C['t2'], font=FF(size=12)).pack(side='left')
        
        temp_val_frm = CTkFrame(temp_frm, fg_color='transparent')
        temp_val_frm.pack(side='left', fill='x', expand=True, padx=(5, 0))
        
        self.sim_widgets['temp_slider'] = ctk.CTkSlider(temp_val_frm, from_=10, to=50, number_of_steps=40,
                                                         command=lambda v, k='ambient_temp': self._on_sim_param_change(k, v))
        self.sim_widgets['temp_slider'].pack(side='left', fill='x', expand=True)
        self.sim_widgets['temp_slider'].set(inv.sim_params['ambient_temp'])
        
        self.sim_widgets['temp_label'] = CTkLabel(temp_val_frm, text=f"{inv.sim_params['ambient_temp']:.0f}°C",
                                                    width=70, anchor='e', text_color=C['t2'], font=FF(size=12, weight='bold'))
        self.sim_widgets['temp_label'].pack(side='left', padx=(5, 0))
        
        # ========== 逆变效率调节 ==========
        eff_frm = CTkFrame(parent, fg_color='transparent')
        eff_frm.pack(fill='x', pady=3)
        CTkLabel(eff_frm, text="⚡ 逆变效率:", width=90, anchor='w', text_color=C['t2'], font=FF(size=12)).pack(side='left')
        
        eff_val_frm = CTkFrame(eff_frm, fg_color='transparent')
        eff_val_frm.pack(side='left', fill='x', expand=True, padx=(5, 0))
        
        self.sim_widgets['eff_slider'] = ctk.CTkSlider(eff_val_frm, from_=0.70, to=0.96, number_of_steps=26,
                                                        command=lambda v, k='inverter_efficiency': self._on_sim_param_change(k, v))
        self.sim_widgets['eff_slider'].pack(side='left', fill='x', expand=True)
        self.sim_widgets['eff_slider'].set(inv.sim_params['inverter_efficiency'])
        
        self.sim_widgets['eff_label'] = CTkLabel(eff_val_frm, text=f"{inv.sim_params['inverter_efficiency']*100:.0f}%",
                                                    width=70, anchor='e', text_color=C['t2'], font=FF(size=12, weight='bold'))
        self.sim_widgets['eff_label'].pack(side='left', padx=(5, 0))
        
        # ========== 能量状态显示 ==========
        energy_frm = CTkFrame(parent, fg_color=C['card2'], corner_radius=6)
        energy_frm.pack(fill='x', pady=(10, 5))
        CTkLabel(energy_frm, text="⚖️ 能量状态:", font=FF(size=12, weight='bold'), text_color=C['t']).pack(anchor='w', padx=10, pady=(8, 5))
        
        self.energy_label = CTkLabel(energy_frm, text="计算中...", font=FF(size=11), text_color=C['t3'])
        self.energy_label.pack(anchor='w', padx=10, pady=(0, 8))
        
        # 重置按钮
        reset_frm = CTkFrame(parent, fg_color='transparent')
        reset_frm.pack(fill='x', pady=(10, 0))
        CTkButton(reset_frm, text="🔄 重置模拟参数", height=30, fg_color=C['accent'],
                   hover_color=C['highlight'], command=self._reset_sim_params,
                   font=FF(size=12)).pack(fill='x')

    def _on_sim_param_change(self, key, value):
        """处理模拟参数变化"""
        inv = self.chip.inverter
        
        if key == 'target_pv_power':
            value = int(value)
            inv.set_sim_param(key, float(value))
            self.sim_widgets['pv_label'].configure(text=f"{value}W")
        elif key == 'target_ac_power':
            value = int(value)
            inv.set_sim_param(key, float(value))
            self.sim_widgets['ac_label'].configure(text=f"{value}W")
        elif key == 'initial_soc':
            value = int(value)
            inv.set_sim_param(key, float(value))
            self.sim_widgets['soc_label'].configure(text=f"{value}%")
        elif key == 'ambient_temp':
            value = int(value)
            inv.set_sim_param(key, float(value))
            self.sim_widgets['temp_label'].configure(text=f"{value}°C")
        elif key == 'inverter_efficiency':
            inv.set_sim_param(key, value)
            self.sim_widgets['eff_label'].configure(text=f"{value*100:.0f}%")
    
    def _reset_sim_params(self):
        """重置模拟参数到默认值"""
        defaults = {
            'target_pv_power': 1500.0,
            'target_ac_power': 2200.0,
            'pv_volatility': 0.02,
            'ac_volatility': 0.03,
            'ambient_temp': 32.0,
            'inverter_efficiency': 0.92,
            'initial_soc': 75.0,
            'battery_capacity': 100.0,
        }
        
        for key, value in defaults.items():
            self.chip.inverter.set_sim_param(key, value)
        
        # 更新滑块显示
        self.sim_widgets['pv_slider'].set(defaults['target_pv_power'])
        self.sim_widgets['pv_label'].configure(text=f"{defaults['target_pv_power']:.0f}W")
        self.sim_widgets['ac_slider'].set(defaults['target_ac_power'])
        self.sim_widgets['ac_label'].configure(text=f"{defaults['target_ac_power']:.0f}W")
        self.sim_widgets['soc_slider'].set(defaults['initial_soc'])
        self.sim_widgets['soc_label'].configure(text=f"{defaults['initial_soc']:.0f}%")
        self.sim_widgets['temp_slider'].set(defaults['ambient_temp'])
        self.sim_widgets['temp_label'].configure(text=f"{defaults['ambient_temp']:.0f}°C")
        self.sim_widgets['eff_slider'].set(defaults['inverter_efficiency'])
        self.sim_widgets['eff_label'].configure(text=f"{defaults['inverter_efficiency']*100:.0f}%")
        
        self._add_log("[SIM] 模拟参数已重置为默认值")

    def _update_energy_display(self):
        """更新能量状态显示"""
        try:
            inv = self.chip.inverter
            p = inv.sim_params
            
            pv = inv.pv_power
            ac = inv.power
            diff = pv - ac
            
            if diff > 10:
                status = f"☀️ PV余电 {diff:.0f}W → 电池充电中"
                color = C['success']
            elif diff < -10:
                status = f"🔋 PV不足 {abs(diff):.0f}W ← 电池放电中"
                color = C['warning']
            else:
                status = "⚖️ PV = AC 平衡"
                color = C['info']
            
            self.energy_label.configure(text=f"{status}\nSOC: {inv.soc:.1f}% | 电池: {inv.battery_i:.1f}A",
                                         text_color=color)
        except:
            pass

    def _pub_topic(self, topic):
        if not self.chip.connected: self._add_log("[WARN] 请先连接串口"); return
        payload = json.dumps(self.chip.inverter.to_json(topic), ensure_ascii=False)
        obj = {"sn": self.chip.inverter.sn, "topic": f"data/{topic}", "payload": payload}
        data = json.dumps(obj, ensure_ascii=False).encode('utf-8')
        ok, info = self.chip.serial_send(CMD_PUBLISH, data)
        if ok: self._add_log(f"[TX] PUBLISH data/{topic}")
        else: self._add_log(f"[ERR] PUBLISH失败: {info}")

    def _query_ota_info(self):
        if not self.chip.connected: self._add_log("[WARN] 请先连接串口"); return
        self.chip.serial_send(CMD_ARM_OTA_INFO, b''); self._add_log("[TX] OTA_INFO 查询")

    def _factory_reset(self):
        if self.chip.connected: self.chip.serial_send(CMD_FACTORY_RESET, b'\x00'); self._add_log("[TX] 恢复出厂命令")
        self.chip.factory_reset()

    def _reset_ota(self):
        self.chip.reset_ota(); self._add_log("[OTA] OTA状态已重置")

    def _add_log(self, msg):
        try:
            self.log_txt.configure(state='normal'); self.log_txt.insert('end', f"{msg}\n")
            self.log_txt.see('end'); self.log_txt.configure(state='disabled')
        except: pass

    def _update_loop(self):
        self.auto_sim = self.sim_chk.get(); self.auto_pub = self.pub_chk.get()
        if self.auto_sim: self.chip.tick(0.2)
        if self.auto_pub and self.chip.connected:
            now = time.time()
            if now - self._last_pub >= self.pub_interval:
                self._last_pub = now
                if not self._first_pub_done: self._first_pub_done = True; self._pub_topic('info')
                topic = self._pub_topics[self._pub_topic_index % len(self._pub_topics)]
                self._pub_topic_index += 1; self._pub_topic(topic)
        for name, (lbl, getter) in self.param_labels.items():
            try: lbl.configure(text=getter())
            except: pass
        # 更新能量状态显示
        self._update_energy_display()
        self.ota_lbl.configure(text=f"🔄 OTA状态: {self.chip.ota_status}")
        self.ota_progress_lbl.configure(text=f"{self.chip.ota_progress}%")
        if self.chip.bootloader.is_in_ota_mode():
            self.chip_status_lbl.configure(text=f"● 芯片: {self.chip.bootloader.get_state_name()}", text_color=C['ota'])
        else:
            self.chip_status_lbl.configure(text="● 芯片: 正常", text_color=C['success'])
        while self.chip.log_queue: self._add_log(self.chip.log_queue.popleft())
        self.root.after(200, self._update_loop)

    def run(self):
        self.root.mainloop()


# ==================== 单元测试 ====================
def self_test():
    print("=" * 50)
    print("GD32F303 ARM芯片模拟器 - 单元测试")
    print("=" * 50)
    chip = GD32F303Chip()

    print("\n[1] 测试帧编码...")
    frame, xor = encode_frame(0x06, b'\x00')
    print(f"  HEARTBEAT帧: {frame.hex(' ')}  XOR: {xor:02X}")

    print("\n[2] 测试OTA流程...")
    chip.bootloader.reset()
    test_data = struct.pack('<I', 102400) + b'0' * 16 + bytes([5]) + b'V1.2.3'
    ok, resp_cmd, _ = chip.bootloader.handle_start(test_data)
    print(f"  OTA_START: ok={ok}, resp=0x{resp_cmd:02X}")
    test_packet = struct.pack('<H', 0) + b'X' * 480
    ok, resp_cmd, _ = chip.bootloader.handle_data(test_packet)
    print(f"  OTA_DATA[0]: ok={ok}, resp=0x{resp_cmd:02X}")

    print("\n[3] 测试Modbus RTU...")
    req = chip.modbus_master.build_read_holding(1, 0x0000, 10)
    print(f"  读保持寄存器请求: {req.hex(' ')}")

    print("\n[4] 测试逆变器...")
    chip.inverter.tick(1.0)
    print(f"  状态: {chip.inverter.state}  功率: {chip.inverter.power:.1f}W  SOC: {chip.inverter.soc:.1f}%")

    print("\n[5] 测试状态获取...")
    print(f"  {chip.get_status()}")
    print("\n" + "=" * 50 + "\n单元测试完成!\n" + "=" * 50)


# ==================== 入口 ====================
def main():
    if '--test' in sys.argv:
        self_test(); return
    if not HAS_CTK:
        print("请先安装: pip install customtkinter pyserial"); return
    app = SimulatorGUI(); app.run()

if __name__ == '__main__':
    main()
