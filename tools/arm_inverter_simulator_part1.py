#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ARM 逆变器终端模拟器 �?光伏逆变�?WiFi 通讯模块
================================================
功能:
  1. 模拟 ARM MCU 采集的逆变器运行参数（辰烁科技 CS_INV 完整参数集，80+ 参数�?  2. 支持手动修改任意参数值、滑动条快速调�?  3. 通过串口�?ESP32 WiFi 模块通信�?15200bps, 8N1�?  4. 支持发送配置命�?+ 控制命令 + 数据发布
  5. 自动接收并解�?ESP32 回复

协议格式 (二进制帧):
  帧结�? [0xAA][CMD][LEN_H][LEN_L][DATA...][XOR]
  转义规则: 0xAA�?x55 0x01, 0x55�?x55 0x00
  XOR 校验: HEADER ^ CMD ^ LEN_H ^ LEN_L ^ 所有数据字�?
命令�?
  0x01 SET_BROKER    (ARM→ESP) 设置 MQTT 代理
  0x02 PUBLISH       (ARM→ESP) 发布 MQTT 消息
  0x03 COMMAND_RECV  (ESP→ARM) 收到云端命令
  0x04 COMMAND_SEND  (ARM→ESP) 发送控制命令结�?  0x05 FACTORY_RESET (ARM→ESP) 恢复出厂
  0x06 HEARTBEAT     (ESP→ARM) 心跳
  0x08 SET_SN        (ARM→ESP) 设置序列�?  0x09 SET_KEY       (ARM→ESP) 设置设备密钥
  0x0A SET_AP_SSID   (ARM→ESP) 设置热点名称
  0xFE ACK           (ESP→ARM) 应答
  0xFF NACK          (ESP→ARM) 否定应答
"""

import sys, os, time, json, struct, threading, random, math
from datetime import datetime
from collections import deque

try:
    import serial
    import serial.tools.list_ports
    HAS_SERIAL = True
except ImportError:
    HAS_SERIAL = False

try:
    import tkinter as tk
    from tkinter import ttk, scrolledtext, messagebox, filedialog
    HAS_TK = True
except ImportError:
    HAS_TK = False

# ==================== 帧协议常�?====================
FRAME_HEADER     = 0xAA
FRAME_ESCAPE     = 0x55
FRAME_ESC_HEADER = 0x01
FRAME_ESC_ESCAPE = 0x00
FRAME_DATA_MAX   = 512

CMD_SET_BROKER    = 0x01
CMD_PUBLISH       = 0x02
CMD_COMMAND_RECV  = 0x03
CMD_COMMAND_SEND  = 0x04
CMD_FACTORY_RESET = 0x05
CMD_HEARTBEAT     = 0x06
CMD_SET_SN        = 0x08
CMD_SET_KEY       = 0x09
CMD_SET_AP_SSID   = 0x0A
CMD_ACK           = 0xFE
CMD_NACK          = 0xFF

CMD_NAMES = {
    0x01:"SET_BROKER", 0x02:"PUBLISH", 0x03:"COMMAND_RECV(ESP→ARM)",
    0x04:"COMMAND_SEND(ARM→ESP)", 0x05:"FACTORY_RESET", 0x06:"HEARTBEAT",
    0x08:"SET_SN", 0x09:"SET_KEY", 0x0A:"SET_AP_SSID", 0xFE:"ACK", 0xFF:"NACK",
}

CMD_DIRECTION = {
    0x01:"ARM→ESP",0x02:"ARM→ESP",0x03:"ESP→ARM",0x04:"ARM→ESP",0x05:"ARM→ESP",
    0x06:"ESP→ARM",0x08:"ARM→ESP",0x09:"ARM→ESP",0x0A:"ARM→ESP",0xFE:"ESP→ARM",0xFF:"ESP→ARM",
}

# ==================== 帧编解码 ====================
def calc_xor(cmd, data_bytes):
    x = FRAME_HEADER ^ cmd
    dl = len(data_bytes)
    x ^= (dl>>8)&0xFF; x ^= dl&0xFF
    for b in data_bytes: x ^= b
    return x&0xFF

def encode_frame(cmd, data_bytes):
    xor_val = calc_xor(cmd, data_bytes)
    dl = len(data_bytes)
    raw = bytearray([FRAME_HEADER, cmd, (dl>>8)&0xFF, dl&0xFF])
    raw.extend(data_bytes); raw.append(xor_val)
    escaped = bytearray([FRAME_HEADER])
    for b in raw[1:]:
        if b==FRAME_HEADER: escaped.extend([FRAME_ESCAPE, FRAME_ESC_HEADER])
        elif b==FRAME_ESCAPE: escaped.extend([FRAME_ESCAPE, FRAME_ESC_ESCAPE])
        else: escaped.append(b)
    return bytes(escaped), xor_val

class FrameReceiver:
    RX_WAIT_HEADER, RX_RECV_CMD, RX_RECV_LEN_H, RX_RECV_LEN_L, RX_RECV_DATA, RX_RECV_XOR = range(6)
    def __init__(self): self.reset()
    def reset(self):
        self.state=self.RX_WAIT_HEADER; self.cmd=0; self.data_len=0; self.data_idx=0
        self.data_buf=bytearray(FRAME_DATA_MAX); self.rx_xor=0; self.esc_next=False
    def feed(self,byte_val):
        b=byte_val
        if self.state!=self.RX_WAIT_HEADER:
            if not self.esc_next and b==FRAME_ESCAPE: self.esc_next=True; return None
            if self.esc_next:
                self.esc_next=False
                if b==FRAME_ESC_HEADER: b=FRAME_HEADER
                elif b==FRAME_ESC_ESCAPE: b=FRAME_ESCAPE
        if self.state==self.RX_WAIT_HEADER:
            if b==FRAME_HEADER: self.state=self.RX_RECV_CMD; self.rx_xor=FRAME_HEADER; self.esc_next=False
            return None
        elif self.state==self.RX_RECV_CMD: self.cmd=b; self.rx_xor^=b; self.state=self.RX_RECV_LEN_H; return None
        elif self.state==self.RX_RECV_LEN_H: self.data_len=b<<8; self.rx_xor^=b; self.state=self.RX_RECV_LEN_L; return None
        elif self.state==self.RX_RECV_LEN_L:
            self.data_len|=b; self.rx_xor^=b; self.data_idx=0
            if self.data_len==0: self.state=self.RX_RECV_XOR
            elif self.data_len>FRAME_DATA_MAX: self.state=self.RX_WAIT_HEADER
            else: self.state=self.RX_RECV_DATA
            return None
        elif self.state==self.RX_RECV_DATA:
            self.data_buf[self.data_idx]=b; self.data_idx+=1; self.rx_xor^=b
            if self.data_idx>=self.data_len: self.state=self.RX_RECV_XOR
            return None
        elif self.state==self.RX_RECV_XOR:
            self.state=self.RX_WAIT_HEADER; valid=(b==self.rx_xor)
            return {'cmd':self.cmd,'cmd_name':CMD_NAMES.get(self.cmd,f"UNK_0x{self.cmd:02X}"),
                    'direction':CMD_DIRECTION.get(self.cmd,'???'),'length':self.data_len,
                    'data':bytes(self.data_buf[:self.data_len]),'xor':b,'expected_xor':self.rx_xor,
                    'valid':valid,'timestamp':datetime.now()}
        return None

# ==================== 逆变器参数模�?(辰烁科技 CS_INV 完整�? ====================
class InverterModel:
    def __init__(self):
        # ===== 设备信息 (4950-4999) =====
        self.protocol_number   = 0x00010001
        self.protocol_version  = 0x00020000
        self.arm_version       = "V1.2.3.20240510"
        self.dsp_version       = "V2.0.1.20240315"
        self.serial_number     = "CS202400100001"
        self.device_type_code  = 0xC001       # CS_INV-5K
        self.device_type       = "CS_INV-5K"
        # ===== 额定参数 =====
        self.nominal_active_power   = 5.0     # kW
        self.nominal_reactive_power = 1.0     # kVar
        self.output_type            = 1       # 0=单相,1=三相
        # ===== 发电数据 =====
        self.daily_power_yields   = 12.5      # kWh
        self.total_power_yields   = 12580.0   # kWh
        self.total_power_yields_01 = 12580.3  # kWh (0.1分辨�?
        self.monthly_power_yields = 380.5     # kWh
        self.total_running_time   = 8640      # h
        self.daily_running_time   = 480       # min
        # ===== 温度 =====
        self.internal_temperature = 48.5      # °C
        # ===== MPPT 1-12 =====
        self.mppt_voltage = [320.0, 0,0,0,0,0,0,0,0,0,0,0]
        self.mppt_current = [5.2,   0,0,0,0,0,0,0,0,0,0,0]
        self.total_dc_power = 1976.0           # W
        # ===== 电网电压/电流 (三相) =====
        self.phase_a_voltage = 220.0
        self.phase_b_voltage = 221.0
        self.phase_c_voltage = 219.0
        self.phase_a_current = 8.5
        self.phase_b_current = 8.3
        self.phase_c_current = 8.6
        # ===== 功率参数 =====
        self.total_active_power   = 1870.0     # W
        self.total_reactive_power = 50.0       # Var
        self.total_apparent_power = 1871.0     # VA
        self.power_factor         = 0.99
        self.grid_frequency       = 50.01      # Hz
        self.grid_frequency_001   = 50.01      # Hz (0.01分辨�?
        # ===== 状态信�?=====
        self.work_state_1     = 0x0000  # 0x0000=Run
        self.work_state_1_str = "Run"
        self.work_state_2     = 0x0000
        self.inverter_state_1 = 0
        self.inverter_state_2 = 0
        # ===== 故障/告警 =====
        self.fault_year=0; self.fault_month=0; self.fault_day=0
        self.fault_hour=0; self.fault_minute=0; self.fault_second=0
        self.fault_code = 0
        # ===== 其他参数 =====
        self.insulation_resistance = 9999    # kΩ
        self.bus_voltage           = 360.0   # V
        self.negative_ground_voltage = 0.0   # V
        self.pid_work_state  = 0
        self.pid_alarm_code  = 0
        self.country_code    = 1
        # ===== 电表功率 =====
        self.meter_total_power   = 1500.0
        self.meter_phase_a_power = 500.0
        self.meter_phase_b_power = 500.0
        self.meter_phase_c_power = 500.0
        self.load_power          = 350.0     # W
        # ===== 馈网/取电电量 =====
        self.daily_feed_energy  = 10.5      # kWh
        self.total_feed_energy  = 12000.0   # kWh
        self.daily_grid_import  = 0.5       # kWh
        self.total_grid_import  = 100.0     # kWh
        # ===== 组串电流 1-24 =====
        self.string_current = [0.0]*24
        self.string_current[0] = 5.2
        self.string_current[1] = 5.1
        # ===== 功率设定�?=====
        self.active_power_setting   = 5000.0
        self.reactive_power_setting = 0.0
        self.power_factor_setting   = 1.0
        # ===== 设置参数 (保持寄存�? =====
        self.time_year=2026; self.time_month=5; self.time_day=18
        self.time_hour=15; self.time_minute=30; self.time_second=0
        self.power_on_off          = 1     # 1=开�?        self.power_limit_switch    = 0     # 0=关闭限功�?        self.power_limit_percent   = 100.0 # %
        self.feed_limit_switch     = 0
        self.feed_limit_value      = 50.0  # kW
        self.feed_limit_percent    = 100.0 # %
        self.power_factor_set      = 1.0
        self.active_overload_switch = 0
        self.svg_reactive_percent  = 0.0
        self.local_remote_control  = 1     # 1=远程

        self._last_update = time.time()

    # ===== 工作状态字符串 =====
    WORK_STATE_MAP = {0x0000:"Run",0x8000:"Stop",0x1300:"Key Stop",0x1500:"Emergency Stop",
                      0x1400:"Standby",0x1200:"Initial Standby",0x1600:"Starting",
                      0x9100:"Alarm Run",0x8100:"Derating Run",0x8200:"Dispatch Run",
                      0x5500:"Fault",0x2500:"Comm Fault"}

    DEVICE_TYPE_MAP = {0xC001:"CS_INV-5K",0xC002:"CS_INV-6K",0xC003:"CS_INV-8K",
                       0xC004:"CS_INV-10K",0xC005:"CS_INV-15K",0xC006:"CS_INV-20K",
                       0xC007:"CS_INV-30K",0xC008:"CS_INV-50K",0xC009:"CS_INV-100K"}

    def _float_round(self,v,d): return round(v,d) if isinstance(v,(int,float)) else v

    def __setattr__(self,name,value):
        if name=='work_state_1':
            object.__setattr__(self,'work_state_1_str',self.WORK_STATE_MAP.get(value,'Unknown'))
        if name=='device_type_code':
            object.__setattr__(self,'device_type',self.DEVICE_TYPE_MAP.get(value,'Unknown'))
        object.__setattr__(self,name,value)

    def set_parameter(self, name, value):
        if hasattr(self,name): setattr(self,name,value); return True
        return False

    def get_parameter(self, name):
        return getattr(self,name,None)

    def _snapshot(self):
        """获取所有参数（用于实时显示�?""
        p={}
        p['serial_number'] = self.serial_number
        p['device_type']   = self.device_type
        p['arm_version']   = self.arm_version
        p['dsp_version']   = self.dsp_version
        # AC
        for k in ['phase_a_voltage','phase_b_voltage','phase_c_voltage',
                  'phase_a_current','phase_b_current','phase_c_current',
                  'total_active_power','total_reactive_power','total_apparent_power',
                  'power_factor','grid_frequency','grid_frequency_001']:
            p[k]=self._float_round(getattr(self,k),2)
        # DC / MPPT
        for i in range(12):
            p[f'mppt_voltage_{i+1}']=self._float_round(self.mppt_voltage[i],1)
            p[f'mppt_current_{i+1}']=self._float_round(self.mppt_current[i],2)
        p['total_dc_power'] = self._float_round(self.total_dc_power,1)
        # 状�?        p['work_state_1']=self.work_state_1_str
        p['internal_temperature']=self._float_round(self.internal_temperature,1)
        p['bus_voltage']=self._float_round(self.bus_voltage,1)
        p['insulation_resistance']=self.insulation_resistance
        p['fault_code']=self.fault_code
        p['efficiency'] = min(98.0, max(90.0, round(self.total_active_power/max(1,self.total_dc_power)*100,1))) if self.total_dc_power>10 else 95.0
        # 发电
        for k in ['daily_power_yields','total_power_yields','monthly_power_yields',
                  'total_running_time','daily_running_time']:
            p[k]=self._float_round(getattr(self,k),2)
        # 电表/馈网
        for k in ['meter_total_power','load_power','daily_feed_energy','total_feed_energy']:
            p[k]=self._float_round(getattr(self,k),2)
        # 设定�?        for k in ['active_power_setting','reactive_power_setting','power_factor_setting',
                  'power_limit_percent','feed_limit_value','local_remote_control']:
            p[k]=self._float_round(getattr(self,k),2)
        return p

    def tick(self):
        now=time.time(); dt=min(now-self._last_update,5); self._last_update=now
        if dt<=0: return
        # AC 波动
        self.phase_a_voltage+=random.gauss(0,0.8)*dt; self.phase_a_voltage=max(180,min(260,self.phase_a_voltage))
        self.phase_a_current+=random.gauss(0,0.15)*dt; self.phase_a_current=max(0,min(25,self.phase_a_current))
        self.phase_b_voltage = self.phase_a_voltage+random.gauss(0,2)
        self.phase_c_voltage = self.phase_a_voltage+random.gauss(0,2)
        self.phase_b_current = self.phase_a_current+random.gauss(0,0.5)
        self.phase_c_current = self.phase_a_current+random.gauss(0,0.5)
        self.grid_frequency+=random.gauss(0,0.01)*dt; self.grid_frequency=max(49.5,min(50.5,self.grid_frequency))
        self.grid_frequency_001=self.grid_frequency
        # 功率计算
        self.total_active_power=(self.phase_a_voltage*self.phase_a_current+
            self.phase_b_voltage*self.phase_b_current+self.phase_c_voltage*self.phase_c_current)*self.power_factor/1000
        self.total_apparent_power=self.total_active_power/self.power_factor if self.power_factor>0 else self.total_active_power
        self.total_reactive_power=math.sqrt(max(0,self.total_apparent_power**2-self.total_active_power**2))
        # DC
        self.mppt_voltage[0]+=random.gauss(0,1.5)*dt; self.mppt_voltage[0]=max(150,min(550,self.mppt_voltage[0]))
        self.mppt_current[0]+=random.gauss(0,0.1)*dt; self.mppt_current[0]=max(0,min(14,self.mppt_current[0]))
        self.total_dc_power=self.mppt_voltage[0]*self.mppt_current[0]
        self.string_current[0]=self.mppt_current[0]
        # 温度
        self.internal_temperature+=random.gauss(0,0.1)*dt; self.internal_temperature=max(20,min(75,self.internal_temperature))
        self.bus_voltage+=random.gauss(0,1)*dt; self.bus_voltage=max(200,min(450,self.bus_voltage))
        # 累计
        einc=self.total_active_power*dt/3600000.
        self.daily_power_yields+=einc; self.total_power_yields+=einc; self.monthly_power_yields+=einc
        self.total_power_yields_01=self.total_power_yields

        self.daily_running_time+=int(dt/60) if random.random()<0.1 else 0
        self.daily_feed_energy+=einc*0.8; self.total_feed_energy+=einc*0.8
        # 电表
        self.meter_total_power=self.total_active_power-self.load_power if self.total_active_power>self.load_power else 0

    def to_publish_payload(self, category="all"):
        """生成 MQTT JSON payload（匹�?inverter_data.h 格式�?""
        if category=="device":
            return json.dumps({"manufacturer":"辰烁科技","model":self.device_type,
                "serial_number":self.serial_number,"device_type_code":self.device_type_code,
                "arm_version":self.arm_version,"dsp_version":self.dsp_version,
                "nominal_power":self.nominal_active_power},ensure_ascii=False)
        if category=="ac":
            return json.dumps({k:round(getattr(self,k),2) for k in
                ['phase_a_voltage','phase_b_voltage','phase_c_voltage','phase_a_current',
                 'phase_b_current','phase_c_current','total_active_power','total_reactive_power',
                 'total_apparent_power','power_factor','grid_frequency','grid_frequency_001']},ensure_ascii=False)
        if category=="dc":
            obj={"total_dc_power":round(self.total_dc_power,1)}
            for i in range(12):
                obj[f"mppt{i+1}_v"]=round(self.mppt_voltage[i],1)
                obj[f"mppt{i+1}_i"]=round(self.mppt_current[i],2)
            return json.dumps(obj,ensure_ascii=False)
        if category=="status":
            return json.dumps({"work_state":self.work_state_1_str,"work_state_code":self.work_state_1,
                "temp":round(self.internal_temperature,1),"bus_voltage":round(self.bus_voltage,1),
                "insulation":self.insulation_resistance,"fault_code":self.fault_code,
                "efficiency":round(self.total_active_power/max(1,self.total_dc_power)*100,1)},ensure_ascii=False)
        if category=="energy":
            return json.dumps({k:round(getattr(self,k),2) for k in
                ['daily_power_yields','total_power_yields','monthly_power_yields',
                 'daily_feed_energy','total_feed_energy','daily_grid_import','total_grid_import',
                 'total_running_time','daily_running_time']},ensure_ascii=False)
        if category=="meter":
            return json.dumps({k:round(getattr(self,k),2) for k in
                ['meter_total_power','meter_phase_a_power','meter_phase_b_power',
                 'meter_phase_c_power','load_power']},ensure_ascii=False)
        if category=="strings":
            return json.dumps({"string_currents":[round(v,3) for v in self.string_current]},ensure_ascii=False)
        if category=="settings":
            return json.dumps({k:round(getattr(self,k),2) for k in
                ['active_power_setting','reactive_power_setting','power_factor_setting',
                 'power_limit_percent','feed_limit_value','local_remote_control']},ensure_ascii=False)
        # all
        return json.dumps(self._snapshot(),ensure_ascii=False)

    def handle_control(self, cmd_data):
        """处理控制命令，返�?(ok, message)"""
        try:
            if isinstance(cmd_data,str): obj=json.loads(cmd_data)
            else: obj=cmd_data
        except: return False,"JSON解析失败"
        cmd=obj.get("command","")
        params=obj.get("params",{})
        if cmd=="power_on":
            self.power_on_off=1; self.work_state_1=0x0000; self.work_state_1_str="Run"
            return True,"开机命令已执行"
        elif cmd=="power_off":
            self.power_on_off=0; self.work_state_1=0x8000; self.work_state_1_str="Stop"
            return True,"关机命令已执�?
        elif cmd=="set_time":
            for k in ['year','month','day','hour','minute','second']:
                if k in params: setattr(self,f'time_{k}',params[k])
            return True,f"时间已设置为 {params.get('year',2026)}-{params.get('month',1):02d}-{params.get('day',1):02d}"
        elif cmd=="power_limit":
            enable=params.get("enable",False); percent=params.get("percent",1000)
            self.power_limit_switch=1 if enable else 0
            if enable: self.power_limit_percent=percent/10.
            return True,"限功率已使能" if enable else "限功率已关闭"
        elif cmd=="feed_limit":
            enable=params.get("enable",False); value=params.get("value",5000)
            self.feed_limit_switch=1 if enable else 0
            if enable: self.feed_limit_value=value/100.
            return True,"馈网限制已使�? if enable else "馈网限制已关�?
        elif cmd=="power_factor":
            pf=params.get("value",1000); self.power_factor_set=pf/1000.
            self.power_factor=self.power_factor_set
            return True,f"功率因数已设�?{self.power_factor_set:.3f}"
        elif cmd=="remote_control":
            mode=params.get("mode",1); self.local_remote_control=mode
            names={1:"远程",2:"本地",3:"本地及远�?}
            return True,f"控制模式已切换为 {names.get(mode,'?')}"
        else:
            return False,f"未知命令: {cmd}"

# ==================== 串口管理 ====================
class SerialManager:
    def __init__(self):
        self.serial_port=None; self.is_open=False; self._stop=threading.Event()
        self._rx_thread=None; self._lock=threading.Lock()
        self._rx_frame_queue=deque(maxlen=500); self._receiver=FrameReceiver()
        self._tx_log=deque(maxlen=1000); self._rx_log=deque(maxlen=1000)
        self.on_frame_received=None; self.on_status_change=None; self.on_tx_sent=None; self.on_error=None

    def list_ports(self):
        if not HAS_SERIAL: return []
        return [{'device':p.device,'description':p.description,'hwid':p.hwid} for p in serial.tools.list_ports.comports()]

    def open(self,port,baud=115200):
        if not HAS_SERIAL: return False
        if self.is_open: self.close()
        try:
            self.serial_port=serial.Serial(port,baud,serial.EIGHTBITS,serial.PARITY_NONE,
                serial.STOPBITS_ONE,timeout=0.05,write_timeout=1.0)
            self.is_open=True; self._stop.clear(); self._receiver.reset()
            self._rx_thread=threading.Thread(target=self._rx_loop,daemon=True); self._rx_thread.start()
            if self.on_status_change: self.on_status_change(True,port)
            return True
        except Exception as e: self.is_open=False; self._on_err(f"打开串口失败: {e}"); return False

    def close(self):
        if self.is_open:
            self._stop.set()
            if self._rx_thread and self._rx_thread.is_alive(): self._rx_thread.join(timeout=1)
            try: self.serial_port.close()
            except: pass
            self.serial_port=None; self.is_open=False
            if self.on_status_change: self.on_status_change(False,"")

    def send_frame(self,cmd,data_bytes):
        if not self.is_open or not self.serial_port: return False,"串口未打开"
        try:
            frame,xor_val=encode_frame(cmd,data_bytes)
            with self._lock: self.serial_port.write(frame); self.serial_port.flush()
            self._tx_log.append({'timestamp':datetime.now(),'cmd':cmd,
                'cmd_name':CMD_NAMES.get(cmd,f"0x{cmd:02X}"),'data':data_bytes,
                'xor':xor_val,'direction':'TX'})
            if self.on_tx_sent: self.on_tx_sent(cmd,CMD_NAMES.get(cmd,f"0x{cmd:02X}"),data_bytes)
            return True,frame.hex(' ').upper()
        except Exception as e: return False,str(e)

    def get_rx_frames(self): return [self._rx_frame_queue.popleft() for _ in range(len(self._rx_frame_queue))]
    def get_tx_log(self): return list(self._tx_log)
    def get_rx_log(self): return list(self._rx_log)

    def _rx_loop(self):
        while not self._stop.is_set():
            try:
                if not self.serial_port or not self.serial_port.is_open: time.sleep(0.1); continue
                w=self.serial_port.in_waiting
                if w>0:
                    for b in self.serial_port.read(min(w,256)):
                        r=self._receiver.feed(b)
                        if r:
                            r['direction']='RX'; self._rx_frame_queue.append(r); self._rx_log.append(r)
                            if self.on_frame_received: self.on_frame_received(r['cmd'],r['cmd_name'],bytes(r['data']),r['valid'])
                else: time.sleep(0.01)
            except serial.SerialException:
                self.is_open = False
                if not self._stop.is_set():
                    self._on_err("串口断开")
                break
            except Exception:
                time.sleep(0.1)
    def _on_err(self,msg):
        if self.on_error: self.on_error(msg)

# ==================== 应用主控制器 ====================
class AppController:
    def __init__(self):
        self.serial_mgr=SerialManager(); self.inverter=InverterModel()
        self.auto_simulate=True; self.auto_publish=True; self.publish_interval=5.0
        self._last_pub=0; self.device_sn="CS202400100001"
        self.device_key="a1b2c3d4e5f6a1b2c3d4e5f6"; self.mqtt_broker="192.168.8.115:1883"
        self.ap_ssid=""; self._event_log=deque(maxlen=2000)
        self.serial_mgr.on_frame_received=self._on_rx; self.serial_mgr.on_tx_sent=self._on_tx

    def list_ports(self): return self.serial_mgr.list_ports()
    def open_serial(self,p,b=115200): return self.serial_mgr.open(p,b)
    def close_serial(self): self.serial_mgr.close()
    @property
    def is_connected(self): return self.serial_mgr.is_open

    # ---- 发送命�?----
    def send_publish(self,topic,payload):
        obj={"topic":topic,"payload":payload,"sn":self.device_sn}
        data=json.dumps(obj,ensure_ascii=False,separators=(',',':')).encode('utf-8')
        return self.serial_mgr.send_frame(CMD_PUBLISH,data)

    def send_set_sn(self,sn): return self.serial_mgr.send_frame(CMD_SET_SN,sn.encode('ascii'))
    def send_set_broker(self,h,p=1883): return self.serial_mgr.send_frame(CMD_SET_BROKER,f"{h}:{p}".encode('ascii'))
    def send_set_key(self,k): return self.serial_mgr.send_frame(CMD_SET_KEY,k.encode('ascii'))
    def send_set_ap_ssid(self,s):
        return self.serial_mgr.send_frame(CMD_SET_AP_SSID,s.encode('ascii') if s else b'')
    def send_factory_reset(self): return self.serial_mgr.send_frame(CMD_FACTORY_RESET,b'\x00')
    def send_command_result(self,js): return self.serial_mgr.send_frame(CMD_COMMAND_SEND,js.encode('utf-8'))

    def publish_inverter_data(self,category="all"):
        return self.send_publish(f"data/{category}",self.inverter.to_publish_payload(category))

    def publish_control_command(self,cmd_name,params={}):
        obj={"command":cmd_name,"params":params}
        return self.send_publish("cmd",json.dumps(obj,ensure_ascii=False))

    # ---- 主循�?----
    def update(self):
        if self.auto_simulate: self.inverter.tick()
        if self.auto_publish and self.is_connected:
            now=time.time()
            if now-self._last_pub>=self.publish_interval:
                self._last_pub=now
                cats=["ac","dc","status","energy","meter","strings","settings"]
                self.publish_inverter_data(cats[int(now/self.publish_interval)%len(cats)])

    # ---- 回调 ----
    def _on_rx(self,cmd,name,data,valid):
        msg=f"[RX] {name}"
        if not valid: msg+=" (校验失败!)"
        msg+=f" | {len(data)}B"
        if cmd==CMD_COMMAND_RECV:
            try:
                t=data.decode('utf-8'); obj=json.loads(t); topic=obj.get('topic','')
                pl=obj.get('payload','')
                if topic: msg+=f"\n  Topic: {topic}\n  Payload: {str(pl)[:80]}"
                tag=f"event" if 'event' in obj else "cmd"
            except: msg+=f"\n  RAW: {data.hex(' ').upper()}"
        elif cmd==CMD_HEARTBEAT: msg+="\n  心跳信号"
        elif cmd==CMD_ACK: msg+=" �?OK"
        elif cmd==CMD_NACK: msg+=" �?FAIL"
        self._event_log.append(msg)

    def _on_tx(self,cmd,name,data):
        try:
            s=data.decode('utf-8','replace')
            if cmd==CMD_PUBLISH:
                try:
                    obj=json.loads(s); t=obj.get('topic',''); pp=obj.get('payload','')[:60]
                    msg=f"[TX] {name} | topic={t}"
                    if pp: msg+=f" | payload={pp}..."
                except: msg=f"[TX] {name} | {s[:80]}"
            elif cmd==CMD_SET_SN: msg=f"[TX] {name} | SN={s}"
            elif cmd==CMD_SET_BROKER: msg=f"[TX] {name} | Broker={s}"
            elif cmd==CMD_SET_KEY: msg=f"[TX] {name} | Key={s[:4]}****"
            elif cmd==CMD_SET_AP_SSID: msg=f"[TX] {name} | SSID={s}"
            elif cmd==CMD_FACTORY_RESET: msg=f"[TX] {name} | 恢复出厂"
            else: msg=f"[TX] {name} | {s[:60]}"
        except: msg=f"[TX] {name} | {data.hex(' ').upper()[:60]}"
        self._event_log.append(msg)

    def add_event(self,msg): self._event_log.append(f"[{datetime.now().strftime('%H:%M:%S')}] {msg}")

# ==================== 入口 ====================
if __name__=='__main__':
    print("="*60); print("  ARM 逆变器终端模拟器"); print("  辰烁科技 CS_INV 完整�?); print("="*60)
    print(f"  pyserial: {'OK' if HAS_SERIAL else 'MISSING'}")
    if HAS_TK: from arm_inverter_simulator_gui import main; main()
    else: print("需�?GUI 环境")
