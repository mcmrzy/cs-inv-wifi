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
from datetime import datetime, date
from collections import deque

# ==================== 事件驱动架构 ====================
class EventBus:
    """事件总线管理器"""
    
    def __init__(self):
        self.listeners = {}  # {event_type: [callbacks]}
        self.queue = queue.Queue()
        self._running = False
        self._thread = None
    
    def subscribe(self, event_type, callback):
        """订阅事件"""
        if event_type not in self.listeners:
            self.listeners[event_type] = []
        self.listeners[event_type].append(callback)
    
    def unsubscribe(self, event_type, callback):
        """取消订阅事件"""
        if event_type in self.listeners:
            try:
                self.listeners[event_type].remove(callback)
            except ValueError:
                pass
    
    def publish(self, event_type, **kwargs):
        """发布事件（同步）"""
        event = {'type': event_type, 'data': kwargs}
        if event_type in self.listeners:
            for callback in self.listeners[event_type]:
                try:
                    callback(event)
                except Exception as e:
                    print(f"事件处理错误 {event_type}: {e}")
    
    def publish_async(self, event_type, **kwargs):
        """发布事件（异步）"""
        self.queue.put({'type': event_type, 'data': kwargs})
    
    def start(self):
        """启动事件处理线程"""
        if not self._running:
            self._running = True
            self._thread = threading.Thread(target=self._process_queue, daemon=True)
            self._thread.start()
    
    def stop(self):
        """停止事件处理线程"""
        self._running = False
        if self._thread:
            self._thread.join(timeout=1)
    
    def _process_queue(self):
        """处理异步事件队列"""
        while self._running:
            try:
                event = self.queue.get(timeout=1)
                self.publish(event['type'], **event['data'])
                self.queue.task_done()
            except queue.Empty:
                continue

# 全局事件总线实例
event_bus = EventBus()

# ==================== 状态机模式 ====================
class InverterState:
    """逆变器状态基类"""
    
    def on_enter(self, inverter):
        """进入状态时调用"""
        pass
    
    def on_exit(self, inverter):
        """退出状态时调用"""
        pass
    
    def handle_event(self, inverter, event):
        """处理事件，返回新状态或None"""
        return None
    
    def tick(self, inverter, dt):
        """状态特定的tick处理"""
        pass
    
    def get_name(self):
        """获取状态名称"""
        return self.__class__.__name__.replace('State', '').lower()

class InvertingState(InverterState):
    """逆变运行状态"""
    
    def on_enter(self, inverter):
        inverter.state = 'inverting'
    
    def handle_event(self, inverter, event):
        if event == 'overtemp' or event == 'overload' or event == 'fault':
            return FaultState()
        elif event == 'low_soc':
            return StandbyState()
        elif event == 'shutdown':
            return ShutdownState()
        return None

class StandbyState(InverterState):
    """待机状态"""
    
    def on_enter(self, inverter):
        inverter.state = 'standby'
        inverter.power = 0
        inverter.current = 0
    
    def handle_event(self, inverter, event):
        if event == 'start':
            return InvertingState()
        elif event == 'fault':
            return FaultState()
        return None
    
    def tick(self, inverter, dt):
        # 待机时消耗少量功率
        inverter.power = 0
        inverter.current = 0

class FaultState(InverterState):
    """故障状态"""
    
    def on_enter(self, inverter):
        inverter.state = 'fault'
        inverter.power = 0
        inverter.current = 0
    
    def handle_event(self, inverter, event):
        if event == 'clear_fault':
            return InvertingState()
        return None
    
    def tick(self, inverter, dt):
        # 故障状态下关闭输出
        inverter.power = 0
        inverter.current = 0

class ShutdownState(InverterState):
    """关机状态"""
    
    def on_enter(self, inverter):
        inverter.state = 'shutdown'
        inverter.power = 0
        inverter.current = 0
    
    def handle_event(self, inverter, event):
        if event == 'start':
            return InvertingState()
        return None
    
    def tick(self, inverter, dt):
        inverter.power = 0
        inverter.current = 0

class BypassState(InverterState):
    """旁路状态"""
    
    def on_enter(self, inverter):
        inverter.state = 'bypass'
    
    def handle_event(self, inverter, event):
        if event == 'exit_bypass':
            return InvertingState()
        elif event == 'fault':
            return FaultState()
        return None

class StateMachine:
    """状态机管理器"""
    
    def __init__(self, initial_state):
        self.current_state = initial_state
    
    def transition(self, inverter, new_state):
        """执行状态转换"""
        if self.current_state:
            self.current_state.on_exit(inverter)
        self.current_state = new_state
        self.current_state.on_enter(inverter)
    
    def handle_event(self, inverter, event):
        """处理事件"""
        new_state = self.current_state.handle_event(inverter, event)
        if new_state:
            self.transition(inverter, new_state)
            return True
        return False
    
    def tick(self, inverter, dt):
        """状态机tick"""
        self.current_state.tick(inverter, dt)

# ==================== 数据持久化模块 ====================
class DataPersistence:
    """数据持久化管理器"""
    
    def __init__(self, base_path='./data'):
        self.base_path = base_path
        os.makedirs(base_path, exist_ok=True)
    
    def save_energy_data(self, inverter_data):
        """保存能量统计数据"""
        today = date.today().isoformat()
        filepath = os.path.join(self.base_path, f"energy_{today}.json")
        
        data = {
            'date': today,
            'daily_pv': inverter_data.get('daily_pv', 0),
            'daily_charge': inverter_data.get('daily_charge', 0),
            'daily_discharge': inverter_data.get('daily_discharge', 0),
            'total_pv': inverter_data.get('total_pv', 0),
            'total_charge': inverter_data.get('total_charge', 0),
            'total_discharge': inverter_data.get('total_discharge', 0),
            'runtime_hours': inverter_data.get('runtime_hours', 0),
            'updated_at': datetime.now().isoformat()
        }
        
        with open(filepath, 'w', encoding='utf-8') as f:
            json.dump(data, f, indent=2, ensure_ascii=False)
    
    def load_energy_data(self):
        """加载能量统计数据"""
        today = date.today().isoformat()
        filepath = os.path.join(self.base_path, f"energy_{today}.json")
        
        if os.path.exists(filepath):
            try:
                with open(filepath, 'r', encoding='utf-8') as f:
                    return json.load(f)
            except:
                pass
        
        # 尝试加载历史数据作为初始值
        history_path = os.path.join(self.base_path, "energy_history.json")
        if os.path.exists(history_path):
            try:
                with open(filepath, 'r', encoding='utf-8') as f:
                    return json.load(f)
            except:
                pass
        
        return {}
    
    def save_alarm_history(self, alarms):
        """保存告警历史"""
        filepath = os.path.join(self.base_path, "alarm_history.json")
        
        data = {
            'updated_at': datetime.now().isoformat(),
            'alarms': alarms
        }
        
        with open(filepath, 'w', encoding='utf-8') as f:
            json.dump(data, f, indent=2, ensure_ascii=False)
    
    def load_alarm_history(self):
        """加载告警历史"""
        filepath = os.path.join(self.base_path, "alarm_history.json")
        
        if os.path.exists(filepath):
            try:
                with open(filepath, 'r', encoding='utf-8') as f:
                    data = json.load(f)
                    return data.get('alarms', [])
            except:
                pass
        
        return []
    
    def save_config(self, config):
        """保存配置参数"""
        filepath = os.path.join(self.base_path, "config.json")
        
        data = {
            'updated_at': datetime.now().isoformat(),
            'config': config
        }
        
        with open(filepath, 'w', encoding='utf-8') as f:
            json.dump(data, f, indent=2, ensure_ascii=False)
    
    def load_config(self):
        """加载配置参数"""
        filepath = os.path.join(self.base_path, "config.json")
        
        if os.path.exists(filepath):
            try:
                with open(filepath, 'r', encoding='utf-8') as f:
                    data = json.load(f)
                    return data.get('config', {})
            except:
                pass
        
        return {}
    
    def save_fault_history(self, faults):
        """保存故障历史"""
        filepath = os.path.join(self.base_path, "fault_history.json")
        
        data = {
            'updated_at': datetime.now().isoformat(),
            'faults': faults
        }
        
        with open(filepath, 'w', encoding='utf-8') as f:
            json.dump(data, f, indent=2, ensure_ascii=False)
    
    def load_fault_history(self):
        """加载故障历史"""
        filepath = os.path.join(self.base_path, "fault_history.json")
        
        if os.path.exists(filepath):
            try:
                with open(filepath, 'r', encoding='utf-8') as f:
                    data = json.load(f)
                    return data.get('faults', [])
            except:
                pass
        
        return []

# ==================== 协议常量 ====================
FRAME_HEADER, FRAME_ESCAPE = 0xAA, 0x55
FRAME_ESC_HEADER, FRAME_ESC_ESCAPE = 0x01, 0x00
FRAME_DATA_MAX = 1024

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
        
        # 数据持久化
        self.persistence = DataPersistence()
        
        self.sn = "H1CNA00135000014"
        self.model = "CS-I10-6k2"
        self.manufacturer = "辰烁科技"
        self.firmware_arm = "V1.2.3.20240510"
        self.firmware_esp = "V1.0.5.20240420"
        self.firmware_dsp = "V1.1.0.20240315"
        self.firmware_bms = "V2.0.1.20240401"
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
        self.protect_status = 0  # 保护状态位掩码
        self.bms_fault_code = 0  # BMS故障码
        
        # 状态机
        self.state_machine = StateMachine(InvertingState())
        
        # 加载持久化数据
        self._load_persisted_data()
        
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
        # PV1参数
        self.pv1_voltage = 85.0
        self.pv1_current = 12.0
        self.pv1_power = 1020.0
        self.pv1_voltage_max = 92.5
        self.pv1_power_max = 1150.0
        # PV2参数
        self.pv2_voltage = 82.0
        self.pv2_current = 11.5
        self.pv2_power = 943.0
        self.pv2_voltage_max = 88.0
        self.pv2_power_max = 1050.0
        # 总功率
        self.pv_power_total = self.pv1_power + self.pv2_power
        self.mppt_state = "tracking"
        
        # ========== 能量统计 ==========
        self.daily_pv = 8.56; self.total_pv = 1250.3
        self.daily_charge = 7.80; self.total_charge = 1100.5
        self.daily_discharge = 6.20; self.total_discharge = 980.8
        self.daily_load = 6.00; self.total_load = 950.2
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
            'ac_volatility': 0.03,       # AC波动幅度 (0-0.1)
            'ambient_temp': 32.0,        # 环境温度 (°C)
            'inverter_efficiency': 0.92, # 逆变效率 (0-1)
            'initial_soc': 75.0,         # 初始SOC (%)
            'battery_capacity': 100.0,   # 电池容量 (Ah)
            'auto_alarm_enable': True,    # 是否启用自动告警
            'alarm_probability': 0.0001,  # 随机告警概率（降低到0.01%）
        }
        
        # ========== 告警配置 ==========
        self.alarm_config = {
            'overtemp_threshold': 65.0,   # 过温阈值 (°C)
            'undervolt_threshold': 46.0,  # 欠压阈值 (V)
            'overvolt_threshold': 58.0,   # 过压阈值 (V)
            'overload_threshold': 0.95,   # 过载阈值 (额定功率比例)
        }
        
        # ========== 告警状态 ==========
        self.active_alarms = []  # 当前活动告警列表
        self._alarm_cooldown = {}  # {code: cooldown_end_time} 告警冷却期
        self._manual_alarms = set()  # 手动触发的告警，不会被 _check_alarms 自动清除
        self._last_alarm_pub = 0  # 上次告警上报时间
        self._alarm_pub_callback = None  # 告警上报回调
        self.alarm_history = []  # 告警历史记录
        self.last_alarm_time = 0
        
        # ========== 远程控制参数 ==========
        self.remote_controls = {
            'power_limit': self.rated_power,  # 功率限制 (W)
            'charge_enable': True,            # 充电使能
            'discharge_enable': True,         # 放电使能
            'grid_charge_enable': False,      # 电网充电使能
            'max_charge_current': 30.0,       # 最大充电电流 (A)
            'max_discharge_current': 40.0,    # 最大放电电流 (A)
        }
        
        # ========== 故障状态 ==========
        self.active_faults = {}  # 存储活动故障 {fault_type: end_time}
        self.fault_history = []  # 故障历史记录
        
        # ========== 物理模型参数 ==========
        # 散热系统
        self.fan_speed = 0  # 风扇转速 (0-100%)
        self.fan_on_temp = 45.0  # 风扇启动温度
        self.fan_full_temp = 65.0  # 风扇全速温度
        
        # 效率曲线参数
        self.eff_curve = {
            'low_load': 0.85,    # 低负载效率 (<10%)
            'medium_load': 0.94,  # 中等负载效率 (30-70%)
            'high_load': 0.92,    # 高负载效率 (>90%)
        }
        
        # 老化参数
        self.cycle_count = 120  # 电池循环次数
        self.max_cycles = 3000  # 设计循环寿命
        self.soh_degradation = 0.02  # 每100次循环SOH下降百分比
        
        self._last_update = time.time()

    def set_sim_param(self, key, value):
        """设置模拟参数"""
        if key in self.sim_params:
            self.sim_params[key] = value
            if key == 'initial_soc':
                self.soc = value
            if key == 'battery_capacity':
                self.battery_capacity = value

    def get_sim_params(self):
        """获取所有模拟参数"""
        return dict(self.sim_params)
    
    def _calc_efficiency(self, load_percent):
        """根据负载率计算效率（效率曲线）"""
        if load_percent < 10:
            return self.eff_curve['low_load']
        elif load_percent < 30:
            return self.eff_curve['low_load'] + (self.eff_curve['medium_load'] - self.eff_curve['low_load']) * (load_percent - 10) / 20
        elif load_percent < 70:
            return self.eff_curve['medium_load']
        elif load_percent < 90:
            return self.eff_curve['medium_load'] - (self.eff_curve['medium_load'] - self.eff_curve['high_load']) * (load_percent - 70) / 20
        else:
            return self.eff_curve['high_load']
    
    def _calc_fan_speed(self, temp):
        """根据温度计算风扇转速"""
        if temp < self.fan_on_temp:
            return 0
        elif temp >= self.fan_full_temp:
            return 100
        else:
            return ((temp - self.fan_on_temp) / (self.fan_full_temp - self.fan_on_temp)) * 100
    
    def _update_degradation(self):
        """更新老化状态"""
        degradation = (self.cycle_count / self.max_cycles) * self.soh_degradation * 100
        self.soh = max(80, 100 - degradation)
    
    # ========== 告警定义 ==========
    ALARM_DEFINITIONS = {
        1: {'level': 'fault', 'message': '逆变器过温保护'},
        2: {'level': 'fault', 'message': '电池过压保护'},
        3: {'level': 'fault', 'message': '电池欠压保护'},
        4: {'level': 'fault', 'message': '输出过载保护'},
        5: {'level': 'fault', 'message': '直流母线过压'},
        6: {'level': 'warning', 'message': '逆变器温度过高'},
        7: {'level': 'warning', 'message': '电池SOC过低'},
        8: {'level': 'warning', 'message': 'PV输入异常'},
        9: {'level': 'warning', 'message': '电芯压差过大'},
        10: {'level': 'info', 'message': '系统启动完成'},
        11: {'level': 'info', 'message': '进入待机模式'},
        12: {'level': 'info', 'message': '恢复并网运行'},
    }
    
    def _add_alarm(self, code, manual=False):
        """添加告警
        :param code: 告警代码
        :param manual: 是否手动触发（手动触发的告警不会被 _check_alarms 自动清除）
        """
        now = time.time()
        # 冷却期内不重复触发
        if code in self._alarm_cooldown and now < self._alarm_cooldown[code]:
            return
        if code not in self.active_alarms:
            alarm_info = self.ALARM_DEFINITIONS.get(code, {'level': 'info', 'message': f'未知告警 {code}'})
            self.active_alarms.append(code)
            if manual:
                self._manual_alarms.add(code)
            alarm_record = {
                'code': code,
                'level': alarm_info['level'],
                'message': alarm_info['message'],
                'timestamp': int(now),
                'active': True
            }
            self.alarm_history.append(alarm_record)
            self.last_alarm_time = now
            self.alarm_code = code
            if alarm_info['level'] == 'fault':
                self.fault_code = code
                self.state = 'fault'
            
            # 直接上报告警（同步，带防重）
            now2 = time.time()
            if (hasattr(self, '_alarm_pub_callback') and self._alarm_pub_callback
                    and now2 - getattr(self, '_last_alarm_pub', 0) >= 2):
                self._last_alarm_pub = now2
                self._alarm_pub_callback()
    
    def _remove_alarm(self, code):
        """清除告警"""
        if code in self.active_alarms:
            self.active_alarms.remove(code)
            self._manual_alarms.discard(code)  # 从手动告警集合中移除
            # 设置冷却期，10秒内不重复触发
            self._alarm_cooldown[code] = time.time() + 10
            for alarm in self.alarm_history:
                if alarm['code'] == code and alarm['active']:
                    alarm['active'] = False
                    alarm['clear_timestamp'] = int(time.time())
            if self.alarm_code == code:
                self.alarm_code = self.active_alarms[0] if self.active_alarms else 0
            if self.fault_code == code:
                self.fault_code = 0
                if not any(self.ALARM_DEFINITIONS.get(a, {}).get('level') == 'fault' for a in self.active_alarms):
                    self.state = 'inverting'
    
    def _clear_all_alarms(self):
        """清除所有告警"""
        for code in list(self.active_alarms):
            self._remove_alarm(code)
        self._manual_alarms.clear()  # 清空手动告警集合
    
    def _check_alarms(self):
        """检查告警条件"""
        if not self.sim_params.get('auto_alarm_enable', True):
            return
        
        # 过温检测
        if self.temp_inv > self.alarm_config['overtemp_threshold']:
            self._add_alarm(1)
        elif self.temp_inv < self.alarm_config['overtemp_threshold'] - 5:
            if 1 not in self._manual_alarms:  # 手动触发的告警不自动清除
                self._remove_alarm(1)
        
        # 电池过压检测
        if self.battery_v > self.alarm_config['overvolt_threshold']:
            self._add_alarm(2)
        elif self.battery_v < self.alarm_config['overvolt_threshold'] - 2:
            if 2 not in self._manual_alarms:
                self._remove_alarm(2)
        
        # 电池欠压检测
        if self.battery_v < self.alarm_config['undervolt_threshold']:
            self._add_alarm(3)
        elif self.battery_v > self.alarm_config['undervolt_threshold'] + 2:
            if 3 not in self._manual_alarms:
                self._remove_alarm(3)
        
        # 过载检测
        if self.load_percent > self.alarm_config['overload_threshold'] * 100:
            self._add_alarm(4)
        elif self.load_percent < self.alarm_config['overload_threshold'] * 100 - 5:
            if 4 not in self._manual_alarms:
                self._remove_alarm(4)
        
        # 温度过高警告
        if self.temp_inv > 55 and self.temp_inv <= self.alarm_config['overtemp_threshold']:
            self._add_alarm(6)
        elif self.temp_inv < 50:
            if 6 not in self._manual_alarms:
                self._remove_alarm(6)
        
        # SOC过低警告
        if self.soc < 25:
            self._add_alarm(7)
        elif self.soc > 35:
            if 7 not in self._manual_alarms:
                self._remove_alarm(7)
        
        # 随机告警（带自动清除）
        if random.random() < self.sim_params.get('alarm_probability', 0):
            random_codes = [8, 9, 10, 11, 12]
            code = random.choice(random_codes)
            if code not in self.active_alarms:
                self._add_alarm(code)
                # 设置30秒后自动清除随机告警
                clear_time = time.time() + 30
                if not hasattr(self, '_random_alarm_clear_time'):
                    self._random_alarm_clear_time = {}
                self._random_alarm_clear_time[code] = clear_time
        
        # 检查并清除过期的随机告警
        if hasattr(self, '_random_alarm_clear_time'):
            now = time.time()
            expired = [code for code, clear_time in self._random_alarm_clear_time.items() 
                      if now >= clear_time and code in self.active_alarms]
            for code in expired:
                self._remove_alarm(code)
                del self._random_alarm_clear_time[code]
        
        # 检查是否需要发送恢复消息
        # 当前没有 fault/warning 级别告警
        has_critical = any(
            self.ALARM_DEFINITIONS.get(a, {}).get('level') in ('fault', 'warning')
            for a in self.active_alarms
        )
        # 之前有 fault/warning 告警，现在没有了 -> 发送恢复
        if not has_critical and getattr(self, '_had_critical_alarm', False):
            self._had_critical_alarm = False
            now = time.time()
            if (hasattr(self, '_alarm_pub_callback') and self._alarm_pub_callback
                    and now - getattr(self, '_last_alarm_pub', 0) >= 2):
                self._last_alarm_pub = now
                self._alarm_pub_callback()
        elif has_critical:
            self._had_critical_alarm = True
    
    def set_remote_control(self, key, value):
        """设置远程控制参数"""
        if key in self.remote_controls:
            self.remote_controls[key] = value
            # 应用功率限制
            if key == 'power_limit':
                self.rated_power = min(self.rated_power, value)
    
    def get_remote_controls(self):
        """获取所有远程控制参数"""
        return dict(self.remote_controls)
    
    # ========== 数据持久化方法 ==========
    def _load_persisted_data(self):
        """加载持久化数据"""
        # 加载能量数据
        energy_data = self.persistence.load_energy_data()
        if energy_data:
            self.daily_pv = energy_data.get('daily_pv', self.daily_pv)
            self.daily_charge = energy_data.get('daily_charge', self.daily_charge)
            self.daily_discharge = energy_data.get('daily_discharge', self.daily_discharge)
            self.total_pv = energy_data.get('total_pv', self.total_pv)
            self.total_charge = energy_data.get('total_charge', self.total_charge)
            self.total_discharge = energy_data.get('total_discharge', self.total_discharge)
            self.runtime_hours = energy_data.get('runtime_hours', self.runtime_hours)
        
        # 加载告警历史
        self.alarm_history = self.persistence.load_alarm_history()
        
        # 加载故障历史
        self.fault_history = self.persistence.load_fault_history()
        
        # 加载配置
        config = self.persistence.load_config()
        if config:
            self.sim_params.update(config.get('sim_params', {}))
            self.alarm_config.update(config.get('alarm_config', {}))
            self.remote_controls.update(config.get('remote_controls', {}))
    
    def _save_persisted_data(self):
        """保存持久化数据"""
        # 保存能量数据
        energy_data = {
            'daily_pv': self.daily_pv,
            'daily_charge': self.daily_charge,
            'daily_discharge': self.daily_discharge,
            'total_pv': self.total_pv,
            'total_charge': self.total_charge,
            'total_discharge': self.total_discharge,
            'runtime_hours': self.runtime_hours
        }
        self.persistence.save_energy_data(energy_data)
        
        # 保存告警历史
        self.persistence.save_alarm_history(self.alarm_history)
        
        # 保存故障历史
        self.persistence.save_fault_history(self.fault_history)
        
        # 保存配置
        config = {
            'sim_params': self.sim_params,
            'alarm_config': self.alarm_config,
            'remote_controls': self.remote_controls
        }
        self.persistence.save_config(config)
    
    def reset_daily_stats(self):
        """重置每日统计数据（每日零点调用）"""
        self.daily_pv = 0
        self.daily_charge = 0
        self.daily_discharge = 0
    
    # ========== 故障注入功能 ==========
    FAULT_TYPES = {
        'overtemp': {'code': 1, 'description': '过温故障', 'action': '温度飙升'},
        'overvoltage': {'code': 2, 'description': '电池过压', 'action': '电压升高'},
        'undervoltage': {'code': 3, 'description': '电池欠压', 'action': '电压降低'},
        'overload': {'code': 4, 'description': '过载保护', 'action': '负载突增'},
        'short_circuit': {'code': 20, 'description': '输出短路', 'action': '功率骤降'},
        'comm_error': {'code': 21, 'description': '通信故障', 'action': '数据异常'},
        'pv_error': {'code': 8, 'description': 'PV输入异常', 'action': 'PV功率归零'},
        'cell_imbalance': {'code': 9, 'description': '电芯压差过大', 'action': '电芯电压差异'},
    }
    
    def inject_fault(self, fault_type, duration=30):
        """
        注入故障
        :param fault_type: 故障类型，取自 FAULT_TYPES
        :param duration: 故障持续时间（秒），默认30秒
        """
        if fault_type not in self.FAULT_TYPES:
            return False, f"未知故障类型: {fault_type}"
        
        fault_info = self.FAULT_TYPES[fault_type]
        end_time = time.time() + duration
        
        # 添加故障到活动列表
        self.active_faults[fault_type] = end_time
        
        # 记录故障历史
        fault_record = {
            'type': fault_type,
            'code': fault_info['code'],
            'description': fault_info['description'],
            'start_time': int(time.time()),
            'duration': duration,
            'end_time': int(end_time),
            'active': True
        }
        self.fault_history.append(fault_record)
        
        # 触发对应告警
        self._add_alarm(fault_info['code'])
        
        return True, f"已注入故障: {fault_info['description']}，持续{duration}秒"
    
    def clear_fault(self, fault_type):
        """清除指定故障"""
        if fault_type in self.active_faults:
            del self.active_faults[fault_type]
            fault_info = self.FAULT_TYPES.get(fault_type, {})
            if 'code' in fault_info:
                self._remove_alarm(fault_info['code'])
            # 更新故障历史
            for fault in self.fault_history:
                if fault['type'] == fault_type and fault.get('active'):
                    fault['active'] = False
                    fault['actual_end_time'] = int(time.time())
            return True, f"已清除故障: {fault_type}"
        return False, "故障不存在"
    
    def clear_all_faults(self):
        """清除所有故障"""
        for fault_type in list(self.active_faults.keys()):
            self.clear_fault(fault_type)
        return True, "已清除所有故障"
    
    def _apply_faults(self):
        """应用故障效果到模拟参数"""
        now = time.time()
        
        # 检查并清除已过期的故障
        expired_faults = [ft for ft, end_time in self.active_faults.items() if now >= end_time]
        for ft in expired_faults:
            self.clear_fault(ft)
        
        # 应用活动故障的效果
        for fault_type in self.active_faults:
            if fault_type == 'overtemp':
                # 温度飙升
                self.temp_inv = min(85, self.temp_inv + 5)
            elif fault_type == 'overvoltage':
                # 电池电压升高
                self.battery_v = min(65, self.battery_v + 2)
            elif fault_type == 'undervoltage':
                # 电池电压降低
                self.battery_v = max(40, self.battery_v - 2)
            elif fault_type == 'overload':
                # 负载突增
                self.power = min(self.rated_power * 1.2, self.power * 1.1)
            elif fault_type == 'short_circuit':
                # 输出短路，功率骤降
                self.power = max(0, self.power - 500)
            elif fault_type == 'pv_error':
                # PV功率归零
                self.pv1_power = 0
                self.pv2_power = 0
                self.pv_power_total = 0
            elif fault_type == 'cell_imbalance':
                # 电芯电压差异增大
                if self.cell_voltages:
                    max_idx = random.randint(0, len(self.cell_voltages)-1)
                    min_idx = random.randint(0, len(self.cell_voltages)-1)
                    self.cell_voltages[max_idx] = min(3.65, self.cell_voltages[max_idx] + 0.1)
                    self.cell_voltages[min_idx] = max(3.0, self.cell_voltages[min_idx] - 0.1)
    
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
        
        # ========== 2. 应用远程控制限制 ==========
        rc = self.remote_controls
        effective_power_limit = min(self.rated_power, rc['power_limit'])
        
        # ========== 3. 计算目标AC功率（带小幅度波动） ==========
        ac_noise = 1.0 + random.gauss(0, p['ac_volatility'])
        target_ac = p['target_ac_power'] * ac_noise
        target_ac = max(0, min(effective_power_limit, target_ac))  # 应用功率限制
        
        # 充放电使能控制
        if not rc['charge_enable'] and target_pv > target_ac:
            target_ac = target_pv  # 禁止充电，多余能量通过调整负载消耗
        if not rc['discharge_enable'] and target_pv < target_ac:
            target_ac = target_pv  # 禁止放电，只能使用PV能量
        
        # 电流限制
        max_charge_power = rc['max_charge_current'] * self.battery_v if self.battery_v > 0 else 9999
        max_discharge_power = rc['max_discharge_current'] * self.battery_v if self.battery_v > 0 else 9999
        
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
        
        # ========== 7. 计算PV参数（双路MPPT） ==========
        # PV1参数
        pv1_ratio = 0.53  # PV1占总功率的比例
        pv1_power = target_pv * pv1_ratio
        if pv1_power > 0:
            self.pv1_voltage = 70.0 + (pv1_power / 1060.0) * 50.0  # 70-120V
            self.pv1_voltage += random.gauss(0, 2.0)
            self.pv1_voltage = max(60, min(150, self.pv1_voltage))
            self.pv1_current = self._calc_pv_current(pv1_power, self.pv1_voltage)
            # 更新峰值记录
            if self.pv1_voltage > self.pv1_voltage_max:
                self.pv1_voltage_max = round(self.pv1_voltage, 1)
            if pv1_power > self.pv1_power_max:
                self.pv1_power_max = round(pv1_power, 1)
        else:
            self.pv1_voltage = 0
            self.pv1_current = 0
        self.pv1_power = pv1_power
        
        # PV2参数
        pv2_power = target_pv * (1 - pv1_ratio)
        if pv2_power > 0:
            self.pv2_voltage = 70.0 + (pv2_power / 940.0) * 50.0  # 70-120V
            self.pv2_voltage += random.gauss(0, 2.0)
            self.pv2_voltage = max(60, min(150, self.pv2_voltage))
            self.pv2_current = self._calc_pv_current(pv2_power, self.pv2_voltage)
            # 更新峰值记录
            if self.pv2_voltage > self.pv2_voltage_max:
                self.pv2_voltage_max = round(self.pv2_voltage, 1)
            if pv2_power > self.pv2_power_max:
                self.pv2_power_max = round(pv2_power, 1)
        else:
            self.pv2_voltage = 0
            self.pv2_current = 0
        self.pv2_power = pv2_power
        
        # 总功率
        self.pv_power_total = self.pv1_power + self.pv2_power
        
        # MPPT状态
        if self.pv_power_total > 100:
            self.mppt_state = "tracking"
        elif self.pv_power_total > 10:
            self.mppt_state = "scanning"
        else:
            self.mppt_state = "standby"
        
        # ========== 8. 能量统计 ==========
        pv_energy = self.pv_power_total * dt / 3600000.0  # kWh
        self.daily_pv += pv_energy; self.total_pv += pv_energy
        
        if self.charge_state == "charging":
            bat_energy = abs(self.battery_i * self.battery_v) * dt / 3600000.0
            self.daily_charge += bat_energy
            self.total_charge += bat_energy
        elif self.charge_state == "discharging":
            bat_energy = abs(self.battery_i * self.battery_v) * dt / 3600000.0
            self.daily_discharge += bat_energy
            self.total_discharge += bat_energy
        
        # 负载统计（AC输出能量）
        load_energy = self.power * dt / 3600000.0
        self.daily_load += load_energy
        self.total_load += load_energy
        
        # ========== 9. 温度计算（带散热模型） ==========
        load_factor = self.power / self.rated_power
        heat_generated = load_factor * 20.0
        temp_target = p['ambient_temp'] + 15 + heat_generated
        
        # 风扇散热效果
        self.fan_speed = self._calc_fan_speed(self.temp_inv)
        cooling_effect = (self.fan_speed / 100.0) * 5.0  # 风扇最多降低5度
        
        temp_target -= cooling_effect
        self.temp_inv += (temp_target - self.temp_inv) * dt * 0.1
        self.temp_inv = max(25, min(75, self.temp_inv + random.gauss(0, 0.2)))
        
        self.temp_mos = self.temp_inv + random.uniform(3, 8)
        self.temp_mos = max(30, min(85, self.temp_mos))
        
        self.temp_ambient = p['ambient_temp'] + random.gauss(0, 0.1)
        
        # ========== 10. 效率计算（使用效率曲线） ==========
        total_input = self.pv_power_total + max(0, -battery_power)
        if total_input > 10:
            # 使用效率曲线计算基础效率
            base_eff = self._calc_efficiency(self.load_percent)
            # 温度对效率的影响
            temp_penalty = max(0, (self.temp_inv - 40) * 0.001)
            self.efficiency = (base_eff - temp_penalty) * 100
            self.efficiency = max(70, min(96, self.efficiency))
        else:
            self.efficiency = 90.0
        
        # ========== 11. 老化更新 ==========
        if self.charge_state == "charging" and self.soc > 95:
            # 每次接近满充增加循环计数
            self.cycle_count += 0.01
            self._update_degradation()
        
        # ========== 11. 更新电芯数据 ==========
        base_volt = 3.30 + (self.soc / 100.0) * 0.15
        for i in range(self.cell_count):
            self.cell_voltages[i] = base_volt + random.gauss(0, 0.01)
            self.cell_voltages[i] = max(3.0, min(3.65, self.cell_voltages[i]))
            self.cell_temps[i] = self.temp_inv / self.cell_count + random.uniform(-1, 2)
            self.cell_temps[i] = max(20, min(45, self.cell_temps[i]))
        
        # ========== 12. 运行时间 ==========
        self.runtime_hours += dt / 3600.0
        
        # ========== 13. 告警检查 ==========
        if self.state != 'fault':  # 故障状态下不检查新告警
            self._check_alarms()
        
        # ========== 14. 故障效果应用 ==========
        self._apply_faults()
        
        # ========== 15. 状态机处理 ==========
        self.state_machine.tick(self, dt)
        
        # 根据状态触发状态转换事件
        if self.fault_code != 0 and self.state != 'fault':
            self.state_machine.handle_event(self, 'fault')
        elif self.fault_code == 0 and self.state == 'fault':
            self.state_machine.handle_event(self, 'clear_fault')
        elif self.soc < 20 and self.state == 'inverting':
            self.state_machine.handle_event(self, 'low_soc')

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
        # 从Modbus读取PV参数（仅支持单路MPPT）
        self.pv1_voltage = self.modbus.get_register(0x0201) / 10.0
        self.pv1_current = self.modbus.get_register(0x0202) / 100.0
        self.pv1_power = self.modbus.get_register(0x0203)
        # PV2使用默认值
        self.pv2_voltage = 82.0
        self.pv2_current = 11.5
        self.pv2_power = 943.0
        self.pv_power_total = self.pv1_power + self.pv2_power

    def to_json(self, topic):
        if topic == 'info':
            return {'sn': self.sn, 'model': self.model, 'manufacturer': self.manufacturer,
                    'firmware_arm': self.firmware_arm, 'firmware_esp': self.firmware_esp,
                    'firmware_dsp': getattr(self, 'firmware_dsp', 'V1.1.0'),
                    'firmware_bms': getattr(self, 'firmware_bms', 'V2.0.1'),
                    'type': self.type, 'rated_power': self.rated_power, 'rated_voltage': self.rated_voltage,
                    'rated_freq': self.rated_freq, 'battery_voltage': self.battery_voltage,
                    'battery_type': self.battery_type, 'cell_count': self.cell_count,
                    'parallel_mode': 'standalone', 'parallel_role': 'standalone', 'parallel_phase': None}
        elif topic == 'ac':
            return {'voltage': round(self.voltage, 1), 'current': round(self.current, 2),
                    'power': round(self.power, 1), 'apparent': round(self.voltage * self.current, 1),
                    'frequency': round(self.frequency, 2), 'pf': round(min(1.0, self.power / max(1, self.voltage * self.current)), 2),
                    'load_percent': round(self.load_percent, 1), 'thd_v': round(random.uniform(1.5, 3.5), 1)}
        elif topic == 'battery':
            cell_volt_max = max(self.cell_voltages) if self.cell_voltages else 0
            cell_volt_min = min(self.cell_voltages) if self.cell_voltages else 0
            return {'soc': round(self.soc, 1), 'soh': round(self.soh, 1),
                    'voltage': round(self.battery_v, 2), 'current': round(self.battery_i, 2),
                    'power': round(self.battery_v * self.battery_i, 1),
                    'capacity_remain': round(self.soc / 100.0 * self.battery_capacity, 1),
                    'capacity_total': self.battery_capacity,
                    'cycle_count': int(self.cycle_count),
                    'temp_max': round(max(self.cell_temps), 1),
                    'temp_min': round(min(self.cell_temps), 1),
                    'cell_volt_max': round(cell_volt_max, 3),
                    'cell_volt_min': round(cell_volt_min, 3),
                    'cell_volt_diff': round(cell_volt_max - cell_volt_min, 3),
                    'charge_state': self.charge_state,
                    'protect_status': self.protect_status,
                    'bms_fault_code': self.bms_fault_code,
                    'max_chg_current': 60.0,
                    'max_dischg_current': 120.0,
                    'charge_volt_ref': 54.6,
                    'dischg_cut_volt': 44.0,
                    'temp_battery': round(sum(self.cell_temps) / len(self.cell_temps), 1) if self.cell_temps else 25.0}
        elif topic == 'pv':
            return {'pv1_voltage': round(self.pv1_voltage, 1), 'pv1_current': round(self.pv1_current, 2),
                    'pv1_power': round(self.pv1_power, 1), 'pv1_voltage_max': round(self.pv1_voltage_max, 1),
                    'pv1_power_max': round(self.pv1_power_max, 1), 'pv2_voltage': round(self.pv2_voltage, 1),
                    'pv2_current': round(self.pv2_current, 2), 'pv2_power': round(self.pv2_power, 1),
                    'pv2_voltage_max': round(self.pv2_voltage_max, 1), 'pv2_power_max': round(self.pv2_power_max, 1),
                    'pv_power_total': round(self.pv_power_total, 1), 'mppt_state': self.mppt_state}
        elif topic == 'status':
            return {'state': self.state, 'fault_code': self.fault_code, 'alarm_code': self.alarm_code,
                    'temp_inv': round(self.temp_inv, 1), 'temp_mos': round(self.temp_mos, 1),
                    'temp_ambient': round(self.temp_ambient, 1),
                    'dc_bus_voltage': round(self.battery_v * 1.2, 1),
                    'efficiency': round(self.efficiency, 1),
                    'runtime_hours': round(self.runtime_hours, 1),
                    'fan_speed': round(self.fan_speed, 1)}
        elif topic == 'energy':
            return {'daily_pv': round(self.daily_pv, 2), 'total_pv': round(self.total_pv, 1),
                    'daily_charge': round(self.daily_charge, 2), 'total_charge': round(self.total_charge, 1),
                    'daily_discharge': round(self.daily_discharge, 2), 'total_discharge': round(self.total_discharge, 1),
                    'daily_load': round(self.daily_load, 2), 'total_load': round(self.total_load, 1),
                    'runtime_hours': round(self.runtime_hours, 1)}
        elif topic == 'cells':
            return {'cell_count': self.cell_count,
                    'voltages': [round(v, 3) for v in self.cell_voltages],
                    'temps': [round(t, 1) for t in self.cell_temps]}
        elif topic == 'alarm':
            # 只有 fault 或 warning 级别的告警才上报
            critical_alarms = [
                code for code in self.active_alarms
                if self.ALARM_DEFINITIONS.get(code, {}).get('level') in ('fault', 'warning')
            ]
            if critical_alarms:
                alarms = []
                for code in critical_alarms:
                    info = self.ALARM_DEFINITIONS.get(code, {})
                    alarms.append({
                        'code': code,
                        'level': info.get('level', 'info'),
                        'message': info.get('message', f'未知告警 {code}')
                    })
                # 主告警码取第一个 fault/warning 级别的
                main_code = critical_alarms[0]
                alarm_info = self.ALARM_DEFINITIONS.get(main_code, {})
                return {'code': main_code,
                        'level': alarm_info.get('level', 'info'),
                        'message': alarm_info.get('message', '未知告警'),
                        'count': len(critical_alarms),
                        'alarms': alarms,
                        'timestamp': int(time.time())}
            # 无故障/警告时返回恢复状态
            return {'code': 0, 'level': 'normal', 'message': '故障恢复，系统正常',
                    'count': 0, 'alarms': [], 'timestamp': int(time.time())}
        elif topic == 'parallel':
            return {'mode': 'standalone', 'count': 1, 'total_rated_power': self.rated_power,
                    'total_active_power': round(self.power, 1), 'sync_state': 'synced', 'machines': []}
        elif topic == 'three_phase':
            return {'voltage_l1': round(self.voltage, 1), 'voltage_l2': round(self.voltage, 1),
                    'voltage_l3': round(self.voltage, 1), 'current_l1': round(self.current, 2),
                    'current_l2': round(self.current, 2), 'current_l3': round(self.current, 2),
                    'power_l1': round(self.power, 1), 'power_l2': round(self.power, 1),
                    'power_l3': round(self.power, 1), 'power_total': round(self.power, 1),
                    'voltage_ll': round(self.voltage * 1.732, 1), 'frequency': round(self.frequency, 2),
                    'unbalance_v': 0.5, 'unbalance_i': 0.8}
        elif topic == 'control':
            return self.get_remote_controls()
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
            self._handle_remote_command(data)
        elif cmd == CMD_ACK:
            pass  # 已在 _handle_frame 记录，不回复
        elif cmd == CMD_NACK:
            pass  # 已在 _handle_frame 记录，不回复
        else:
            self._log(f"未知命令: {cmd:02X}"); self.serial_send(CMD_NACK, b'')
    
    def _handle_remote_command(self, data):
        """处理远程控制命令"""
        try:
            cmd_data = json.loads(data.decode('utf-8'))
            cmd_type = cmd_data.get('cmd', '')
            params = cmd_data.get('params', {})
            task_id = cmd_data.get('task_id', '')
            
            if cmd_type == 'set_control':
                # 设置远程控制参数
                results = {}
                for key, value in params.items():
                    if key in self.inverter.remote_controls:
                        self.inverter.set_remote_control(key, value)
                        results[key] = True
                        self._log(f"远程设置 {key} = {value}")
                    else:
                        results[key] = False
                self.serial_send(CMD_ACK, b'')
                # 发送执行结果给ESP32，转发到MQTT
                self._send_cmd_result(task_id, cmd_type, True, "控制参数已设置", results)
            
            elif cmd_type == 'set_alarm':
                # 手动设置告警
                alarm_code = params.get('code', 0)
                if alarm_code > 0:
                    self.inverter._add_alarm(alarm_code, manual=True)  # 标记为手动触发
                    self._log(f"远程触发告警: {alarm_code}")
                    self.serial_send(CMD_ACK, b'')
                    self._send_cmd_result(task_id, cmd_type, True, f"已触发告警 {alarm_code}")
                else:
                    self.inverter._clear_all_alarms()
                    self._log("远程清除所有告警")
                    self.serial_send(CMD_ACK, b'')
                    self._send_cmd_result(task_id, cmd_type, True, "已清除所有告警")
            
            elif cmd_type == 'set_sim_param':
                # 设置模拟参数
                results = {}
                for key, value in params.items():
                    if key in self.inverter.sim_params:
                        self.inverter.set_sim_param(key, value)
                        results[key] = value
                        self._log(f"远程设置模拟参数 {key} = {value}")
                self.serial_send(CMD_ACK, b'')
                self._send_cmd_result(task_id, cmd_type, True, "模拟参数已设置", results)
            
            elif cmd_type == 'reset':
                # 重置逆变器
                self.inverter.soc = 75.0
                self.inverter._clear_all_alarms()
                self.inverter.state = 'inverting'
                self._log("远程重置逆变器")
                self.serial_send(CMD_ACK, b'')
                self._send_cmd_result(task_id, cmd_type, True, "逆变器已重置")
            
            else:
                self._log(f"未知远程命令: {cmd_type}")
                self.serial_send(CMD_NACK, b'')
                self._send_cmd_result(task_id, cmd_type, False, f"未知命令: {cmd_type}")
                
        except Exception as e:
            self._log(f"远程命令处理错误: {e}")
            self.serial_send(CMD_NACK, b'')
            self._send_cmd_result("", "unknown", False, f"处理错误: {e}")
    
    def _send_cmd_result(self, task_id, cmd_type, success, message, data=None):
        """发送命令执行结果给ESP32"""
        result = {
            'task_id': task_id,
            'cmd': cmd_type,
            'success': success,
            'message': message,
            'timestamp': int(time.time())
        }
        if data:
            result['data'] = data
        result_json = json.dumps(result, ensure_ascii=False).encode('utf-8')
        self.serial_send(CMD_COMMAND_SEND, result_json)
        self._log(f"[TX] CMD_RESULT: {cmd_type} {'成功' if success else '失败'}")

    def _handle_publish(self, data):
        try:
            obj = json.loads(data.decode('utf-8'))
            topic = obj.get('topic', '')
            topic_map = {'/info': 'info', '/ac': 'ac', '/battery': 'battery',
                         '/pv': 'pv', '/status': 'status', '/energy': 'energy',
                         '/alarm': 'alarm', '/control': 'control', '/cells': 'cells',
                         '/parallel': 'parallel', '/three_phase': 'three_phase'}
            for suffix, key in topic_map.items():
                if topic.endswith(suffix):
                    obj['payload'] = json.dumps(self.inverter.to_json(key), ensure_ascii=False); break
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
        self.auto_sim = True; self.auto_pub = True; self.pub_interval = 180.0
        self._last_pub = 0; self._pub_topic_index = 0
        self._pub_topics = ['info', 'ac', 'battery', 'pv', 'status', 'energy', 'cells', 'control']
        self._first_pub_done = False
        self._last_alarm_pub = 0  # 上次告警上报时间，用于防重
        # 设置告警上报回调（同步调用）
        self.chip.inverter._alarm_pub_callback = lambda: self._pub_topic('alarm')
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
        for grp in ["概览", "交流输出", "电池BMS", "光伏MPPT", "系统状态", "能量统计", "电芯数据", "告警事件", "控制状态", "模拟参数", "OTA升级", "Modbus寄存器"]:
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
                         ("电芯数据 (cells)", lambda: self._pub_topic('cells')),
                         ("控制状态 (control)", lambda: self._pub_topic('control')),
                         ("告警事件 (alarm)", lambda: self._pub_topic('alarm')),
                         ("并机状态 (parallel)", lambda: self._pub_topic('parallel')),
                         ("三相数据 (three_phase)", lambda: self._pub_topic('three_phase')),
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
                     ("PV功率", lambda: f"{inv.pv_power_total:.1f}", "W"), ("效率", lambda: f"{inv.efficiency:.1f}", "%")],
            "交流输出": [("输出电压", lambda: f"{inv.voltage:.1f}", "V"), ("输出电流", lambda: f"{inv.current:.2f}", "A"),
                         ("有功功率", lambda: f"{inv.power:.1f}", "W"), ("视在功率", lambda: f"{inv.voltage * inv.current:.1f}", "VA"),
                         ("输出频率", lambda: f"{inv.frequency:.2f}", "Hz"),
                         ("功率因数", lambda: f"{min(1.0, inv.power / max(1, inv.voltage * inv.current)):.2f}", ""),
                         ("负载百分比", lambda: f"{inv.load_percent:.1f}", "%"),
                         ("电压THD", lambda: f"{random.uniform(1.5, 3.5):.1f}", "%")],
            "电池BMS": [("SOC", lambda: f"{inv.soc:.1f}", "%"), ("SOH", lambda: f"{inv.soh:.1f}", "%"),
                        ("电池电压", lambda: f"{inv.battery_v:.2f}", "V"), ("电池电流", lambda: f"{inv.battery_i:.2f}", "A"),
                        ("电池功率", lambda: f"{inv.battery_v * inv.battery_i:.1f}", "W"),
                        ("剩余容量", lambda: f"{inv.soc / 100.0 * inv.battery_capacity:.1f}", "Ah"),
                        ("额定容量", lambda: f"{inv.battery_capacity:.1f}", "Ah"),
                        ("循环次数", lambda: f"{int(inv.cycle_count)}", "次"),
                        ("充放电状态", lambda: {'charging': '充电中', 'discharging': '放电中', 'idle': '空闲'}.get(inv.charge_state, inv.charge_state), ""),
                        ("最高温度", lambda: f"{max(inv.cell_temps):.1f}", "°C"), ("最低温度", lambda: f"{min(inv.cell_temps):.1f}", "°C"),
                        ("单体最高电压", lambda: f"{max(inv.cell_voltages):.3f}", "V"), ("单体最低电压", lambda: f"{min(inv.cell_voltages):.3f}", "V"),
                        ("电芯压差", lambda: f"{max(inv.cell_voltages) - min(inv.cell_voltages):.3f}", "V"),
                        ("保护状态", lambda: f"0x{inv.protect_status:02X}", ""), ("BMS故障码", lambda: f"0x{inv.bms_fault_code:04X}", ""),
                        ("最大充电电流", lambda: "60.0", "A"), ("最大放电电流", lambda: "120.0", "A"),
                        ("充电截止电压", lambda: "54.6", "V"), ("放电截止电压", lambda: "44.0", "V"),
                        ("电池平均温度", lambda: f"{sum(inv.cell_temps) / len(inv.cell_temps):.1f}", "°C")],
            "光伏MPPT": [("PV1电压", lambda: f"{inv.pv1_voltage:.1f}", "V"), ("PV1电流", lambda: f"{inv.pv1_current:.2f}", "A"),
                         ("PV1功率", lambda: f"{inv.pv1_power:.1f}", "W"), ("PV1峰值电压", lambda: f"{inv.pv1_voltage_max:.1f}", "V"),
                         ("PV1峰值功率", lambda: f"{inv.pv1_power_max:.1f}", "W"), ("PV2电压", lambda: f"{inv.pv2_voltage:.1f}", "V"),
                         ("PV2电流", lambda: f"{inv.pv2_current:.2f}", "A"), ("PV2功率", lambda: f"{inv.pv2_power:.1f}", "W"),
                         ("PV2峰值电压", lambda: f"{inv.pv2_voltage_max:.1f}", "V"), ("PV2峰值功率", lambda: f"{inv.pv2_power_max:.1f}", "W"),
                         ("总功率", lambda: f"{inv.pv_power_total:.1f}", "W"),
                         ("MPPT状态", lambda: {'tracking': '跟踪中', 'scanning': '扫描中', 'standby': '待机'}.get(inv.mppt_state, inv.mppt_state), "")],
            "系统状态": [("工作状态", lambda: {'inverting': '逆变运行', 'standby': '待机'}.get(inv.state, inv.state), ""),
                         ("故障代码", lambda: f"{inv.fault_code}", ""), ("告警代码", lambda: f"{inv.alarm_code}", ""),
                         ("逆变器温度", lambda: f"{inv.temp_inv:.1f}", "°C"),
                         ("MOS管温度", lambda: f"{inv.temp_mos:.1f}", "°C"), ("环境温度", lambda: f"{inv.temp_ambient:.1f}", "°C"),
                         ("直流母线电压", lambda: f"{inv.battery_v * 1.2:.1f}", "V"),
                         ("运行时长", lambda: f"{inv.runtime_hours:.1f}", "h"),
                         ("风扇转速", lambda: f"{inv.fan_speed:.0f}", "%"), ("效率", lambda: f"{inv.efficiency:.1f}", "%")],
            "能量统计": [("当日光伏发电", lambda: f"{inv.daily_pv:.2f}", "kWh"), ("累计光伏发电", lambda: f"{inv.total_pv:.1f}", "kWh"),
                         ("当日充电量", lambda: f"{inv.daily_charge:.2f}", "kWh"), ("累计充电量", lambda: f"{inv.total_charge:.1f}", "kWh"),
                         ("当日放电量", lambda: f"{inv.daily_discharge:.2f}", "kWh"), ("累计放电量", lambda: f"{inv.total_discharge:.1f}", "kWh"),
                         ("当日用电量", lambda: f"{inv.daily_load:.2f}", "kWh"), ("累计用电量", lambda: f"{inv.total_load:.1f}", "kWh"),
                         ("累计运行时间", lambda: f"{inv.runtime_hours:.1f}", "h")],
            "电芯数据": [("电芯数量", lambda: f"{inv.cell_count}", "个")] +
                        [(f"电芯{i+1}电压", lambda i=i: f"{inv.cell_voltages[i]:.3f}", "V") for i in range(inv.cell_count)] +
                        [(f"电芯{i+1}温度", lambda i=i: f"{inv.cell_temps[i]:.1f}", "°C") for i in range(inv.cell_count)],
            "告警事件": [("活动告警数", lambda: f"{len(inv.active_alarms)}", ""),
                         ("当前告警码", lambda: f"{inv.alarm_code}", ""),
                         ("当前故障码", lambda: f"{inv.fault_code}", ""),
                         ("告警历史数", lambda: f"{len(inv.alarm_history)}", "")] +
                        [(f"告警{i+1}", lambda i=i: f"code={inv.active_alarms[i]}" if i < len(inv.active_alarms) else "-", "") for i in range(8)],
            "控制状态": [("功率限制", lambda: f"{inv.remote_controls['power_limit']}", "W"),
                         ("充电使能", lambda: "是" if inv.remote_controls['charge_enable'] else "否", ""),
                         ("放电使能", lambda: "是" if inv.remote_controls['discharge_enable'] else "否", ""),
                         ("电网充电使能", lambda: "是" if inv.remote_controls['grid_charge_enable'] else "否", ""),
                         ("最大充电电流", lambda: f"{inv.remote_controls['max_charge_current']:.1f}", "A"),
                         ("最大放电电流", lambda: f"{inv.remote_controls['max_discharge_current']:.1f}", "A")],
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
        
        # 添加故障注入面板
        self._create_fault_injection_panel(parent)

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
    
    # ========== 故障注入功能 ==========
    def _create_fault_injection_panel(self, parent):
        """创建故障注入面板"""
        fault_frame = CTkFrame(parent, fg_color='transparent')
        fault_frame.pack(fill='x', pady=5)
        
        # 故障注入标题
        CTkLabel(fault_frame, text="⚠️ 故障注入测试", width=90, anchor='w', 
                  text_color=C['error'], font=FF(size=12, weight='bold')).pack(side='left')
        
        # 故障类型选择
        fault_types = [
            ('overtemp', '过温'),
            ('overvoltage', '过压'),
            ('undervoltage', '欠压'),
            ('overload', '过载'),
            ('short_circuit', '短路'),
            ('pv_error', 'PV异常'),
            ('cell_imbalance', '电芯压差'),
        ]
        
        # 创建故障按钮
        btn_frm = CTkFrame(fault_frame, fg_color='transparent')
        btn_frm.pack(side='left', fill='x', expand=True, padx=(5, 0))
        
        for fault_type, label in fault_types:
            btn = CTkButton(btn_frm, text=label, width=60, height=24,
                           fg_color=C['error'], hover_color='#a30000',
                           font=FF(size=10),
                           command=lambda ft=fault_type: self._inject_fault(ft))
            btn.pack(side='left', padx=2)
        
        # 清除故障按钮
        clear_btn = CTkButton(fault_frame, text="清除全部", width=70, height=24,
                             fg_color=C['info'], hover_color='#1a6b8a',
                             font=FF(size=10),
                             command=self._clear_all_faults)
        clear_btn.pack(side='left', padx=(5, 0))
        
        return fault_frame
    
    def _inject_fault(self, fault_type):
        """注入故障"""
        success, msg = self.chip.inverter.inject_fault(fault_type, duration=30)
        if success:
            self._add_log(f"[FAULT] {msg}")
        else:
            self._add_log(f"[FAULT] 注入失败: {msg}")
    
    def _clear_all_faults(self):
        """清除所有故障"""
        success, msg = self.chip.inverter.clear_all_faults()
        self.chip.inverter._clear_all_alarms()
        self._add_log(f"[FAULT] {msg}")
        # 清除后立即上报空alarm数据
        if self.auto_pub and self.chip.connected:
            self._pub_topic('alarm')

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
                if not self._first_pub_done:
                    self._first_pub_done = True
                    self._pub_topic('info')
                # 一次发送所有主题
                for topic in self._pub_topics:
                    self._pub_topic(topic)
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
