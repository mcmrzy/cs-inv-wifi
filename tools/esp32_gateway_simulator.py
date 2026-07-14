#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ESP32 WiFi 网关模拟器
====================
模拟 ESP32-C3 的行为：
1. 通过串口接收 ARM 的 UART 帧协议数据
2. 解析后通过 MQTT 发布到云端
3. 自动进行 IP 定位，融入 status 消息
4. 接收云端命令，通过 UART 转发给 ARM

配套使用: arm_inverter_simulator_gui.py (ARM 端模拟器)
"""

import sys
import os
import time
import json
import threading
import random
import serial
import serial.tools.list_ports
from datetime import datetime
from collections import deque

try:
    import paho.mqtt.client as mqtt
    HAS_MQTT = True
except ImportError:
    HAS_MQTT = False
    print("需要安装 paho-mqtt: pip install paho-mqtt")

# ==================== 协议常量 ====================
FRAME_HEADER, FRAME_ESCAPE = 0xAA, 0x55
FRAME_ESC_HEADER, FRAME_ESC_ESCAPE = 0x01, 0x00
FRAME_DATA_MAX = 1024

CMD_SET_BROKER, CMD_PUBLISH, CMD_COMMAND_RECV = 0x01, 0x02, 0x03
CMD_COMMAND_SEND, CMD_FACTORY_RESET, CMD_HEARTBEAT = 0x04, 0x05, 0x06
CMD_SET_SN, CMD_SET_KEY, CMD_SET_AP_SSID = 0x08, 0x09, 0x0A
CMD_ACK, CMD_NACK = 0xFE, 0xFF

CMD_NAMES = {
    0x01: "SET_BROKER", 0x02: "PUBLISH", 0x03: "COMMAND_RECV",
    0x04: "COMMAND_SEND", 0x05: "FACTORY_RESET", 0x06: "HEARTBEAT",
    0x08: "SET_SN", 0x09: "SET_KEY", 0x0A: "SET_AP_SSID",
    0xFE: "ACK", 0xFF: "NACK",
}

# ==================== IP 定位模拟数据 ====================
# 模拟不同城市的 IP 定位结果
MOCK_LOCATIONS = [
    {
        "ip": "223.5.5.5",
        "city": "Hangzhou",
        "region": "Zhejiang",
        "country": "CN",
        "country_name": "China",
        "lat": 30.2741,
        "lon": 120.1551,
        "timezone": "Asia/Shanghai"
    },
    {
        "ip": "183.2.172.1",
        "city": "Guangzhou",
        "region": "Guangdong",
        "country": "CN",
        "country_name": "China",
        "lat": 23.1291,
        "lon": 113.2644,
        "timezone": "Asia/Shanghai"
    },
    {
        "ip": "61.135.169.125",
        "city": "Beijing",
        "region": "Beijing",
        "country": "CN",
        "country_name": "China",
        "lat": 39.9042,
        "lon": 116.4074,
        "timezone": "Asia/Shanghai"
    },
    {
        "ip": "114.114.114.114",
        "city": "Nanjing",
        "region": "Jiangsu",
        "country": "CN",
        "country_name": "China",
        "lat": 32.0603,
        "lon": 118.7969,
        "timezone": "Asia/Shanghai"
    }
]

# ==================== 帧编解码 ====================
def calc_xor(cmd, data):
    x = FRAME_HEADER ^ cmd
    dl = len(data)
    x ^= (dl >> 8) & 0xFF
    x ^= dl & 0xFF
    for b in data:
        x ^= b
    return x & 0xFF


def encode_frame(cmd, data):
    xor_val = calc_xor(cmd, data)
    dl = len(data)
    raw = bytearray([FRAME_HEADER, cmd, (dl >> 8) & 0xFF, dl & 0xFF])
    raw.extend(data)
    raw.append(xor_val)
    escaped = bytearray([FRAME_HEADER])
    for b in raw[1:]:
        if b == FRAME_HEADER:
            escaped.extend([FRAME_ESCAPE, FRAME_ESC_HEADER])
        elif b == FRAME_ESCAPE:
            escaped.extend([FRAME_ESCAPE, FRAME_ESC_ESCAPE])
        else:
            escaped.append(b)
    return bytes(escaped), xor_val


class FrameReceiver:
    RX_WAIT_HEADER, RX_RECV_CMD, RX_RECV_LEN_H, RX_RECV_LEN_L, RX_RECV_DATA, RX_RECV_XOR = range(6)

    def __init__(self):
        self.reset()

    def reset(self):
        self.state = self.RX_WAIT_HEADER
        self.cmd = self.data_len = self.data_idx = self.rx_xor = 0
        self.data_buf = bytearray(FRAME_DATA_MAX)
        self.esc_next = False

    def feed(self, b):
        if self.state != self.RX_WAIT_HEADER:
            if not self.esc_next and b == FRAME_ESCAPE:
                self.esc_next = True
                return None
            if self.esc_next:
                self.esc_next = False
                if b == FRAME_ESC_HEADER:
                    b = FRAME_HEADER
                elif b == FRAME_ESC_ESCAPE:
                    b = FRAME_ESCAPE

        if self.state == self.RX_WAIT_HEADER:
            if b == FRAME_HEADER:
                self.state = self.RX_RECV_CMD
                self.rx_xor = FRAME_HEADER
            return None
        elif self.state == self.RX_RECV_CMD:
            self.cmd = b
            self.rx_xor ^= b
            self.state = self.RX_RECV_LEN_H
            return None
        elif self.state == self.RX_RECV_LEN_H:
            self.data_len = b << 8
            self.rx_xor ^= b
            self.state = self.RX_RECV_LEN_L
            return None
        elif self.state == self.RX_RECV_LEN_L:
            self.data_len |= b
            self.rx_xor ^= b
            self.data_idx = 0
            if self.data_len == 0:
                self.state = self.RX_RECV_XOR
            elif self.data_len > FRAME_DATA_MAX:
                self.state = self.RX_WAIT_HEADER
            else:
                self.state = self.RX_RECV_DATA
            return None
        elif self.state == self.RX_RECV_DATA:
            self.data_buf[self.data_idx] = b
            self.data_idx += 1
            self.rx_xor ^= b
            if self.data_idx >= self.data_len:
                self.state = self.RX_RECV_XOR
            return None
        elif self.state == self.RX_RECV_XOR:
            self.state = self.RX_WAIT_HEADER
            valid = (b == self.rx_xor)
            return {
                'cmd': self.cmd,
                'cmd_name': CMD_NAMES.get(self.cmd, f"UNK_{self.cmd:02X}"),
                'length': self.data_len,
                'data': bytes(self.data_buf[:self.data_len]),
                'xor': b,
                'expected_xor': self.rx_xor,
                'valid': valid,
                'timestamp': datetime.now()
            }
        return None


# ==================== IP 定位模块 ====================
class IPGeolocation:
    """模拟 IP 定位功能"""

    def __init__(self):
        self.location = None
        self.last_update = 0
        self.update_interval = 3600  # 1小时缓存
        self.state = "idle"  # idle, requesting, success, failed

    def request_update(self):
        """触发定位请求"""
        if self.state == "requesting":
            return
        if self.location and (time.time() - self.last_update) < self.update_interval:
            print("[IPGEO] 缓存仍有效，跳过请求")
            return

        self.state = "requesting"
        print("[IPGEO] 正在请求定位...")

        # 模拟异步请求（实际 ESP32 使用 HTTP）
        threading.Timer(1.0, self._on_response).start()

    def _on_response(self):
        """模拟定位响应"""
        # 随机选择一个城市作为定位结果
        self.location = random.choice(MOCK_LOCATIONS)
        self.last_update = time.time()
        self.state = "success"
        print(f"[IPGEO] 定位成功: {self.location['city']}, {self.location['country']} "
              f"({self.location['lat']:.4f}, {self.location['lon']:.4f})")

    def get_location(self):
        """获取当前定位"""
        return self.location

    def to_json(self):
        """转为 JSON 字符串"""
        if not self.location:
            return "null"
        return json.dumps(self.location, ensure_ascii=False)

    def get_status(self):
        """获取状态描述"""
        status_map = {
            "idle": "未定位" if not self.location else "已缓存",
            "requesting": "定位中",
            "success": "定位成功",
            "failed": "定位失败"
        }
        return status_map.get(self.state, "未知")


# ==================== ESP32 网关模拟器 ====================
class ESP32GatewaySimulator:
    def __init__(self):
        self.serial_port = None
        self.serial_thread = None
        self.running = False

        self.mqtt_client = None
        self.mqtt_connected = False
        self.mqtt_broker = "localhost"
        self.mqtt_port = 1883

        self.device_sn = "H1CNA0013500001O"
        self.device_key = "a1b2c3d4e5f6"

        self.frame_receiver = FrameReceiver()
        self.ipgeo = IPGeolocation()

        self.log_queue = deque(maxlen=500)
        self.last_heartbeat = 0
        self.heartbeat_interval = 60

        self._lock = threading.Lock()

    def log(self, msg):
        """添加日志"""
        ts = datetime.now().strftime('%H:%M:%S')
        log_msg = f"[{ts}] {msg}"
        self.log_queue.append(log_msg)
        print(log_msg)

    def list_serial_ports(self):
        """列出可用串口"""
        return [{'device': p.device, 'desc': p.description}
                for p in serial.tools.list_ports.comports()]

    def open_serial(self, port, baud=115200):
        """打开串口连接 ARM"""
        try:
            self.serial_port = serial.Serial(port, baud, timeout=0.05)
            self.running = True
            self.serial_thread = threading.Thread(target=self._serial_loop, daemon=True)
            self.serial_thread.start()
            self.log(f"[UART] 已连接: {port} @ {baud}")

            # 连接成功后触发 IP 定位
            self.ipgeo.request_update()

            return True
        except Exception as e:
            self.log(f"[ERR] 串口打开失败: {e}")
            return False

    def close_serial(self):
        """关闭串口"""
        self.running = False
        if self.serial_thread:
            self.serial_thread.join(timeout=1)
        if self.serial_port:
            self.serial_port.close()
            self.serial_port = None
        self.log("[UART] 已断开")

    def _serial_loop(self):
        """串口接收线程"""
        while self.running:
            try:
                if not self.serial_port or not self.serial_port.is_open:
                    time.sleep(0.1)
                    continue

                waiting = self.serial_port.in_waiting
                if waiting > 0:
                    data = self.serial_port.read(min(waiting, 256))
                    for b in data:
                        frame = self.frame_receiver.feed(b)
                        if frame:
                            self._on_frame(frame)
                else:
                    time.sleep(0.01)
            except Exception as e:
                self.log(f"[ERR] 串口读取错误: {e}")
                time.sleep(0.1)

    def _on_frame(self, frame):
        """处理收到的 UART 帧"""
        cmd = frame['cmd']
        cmd_name = frame['cmd_name']
        data = frame['data']
        valid = frame['valid']

        if not valid:
            self.log(f"[RX] {cmd_name} (校验失败!)")
            self._send_nack()
            return

        self.log(f"[RX] {cmd_name} ({len(data)} bytes)")

        if cmd == CMD_PUBLISH:
            # ARM 请求发布 MQTT 消息
            self._handle_publish(data)
            self._send_ack()
        elif cmd == CMD_SET_SN:
            # 设置 SN
            sn = data.decode('ascii', 'replace').strip('\x00')
            self.device_sn = sn
            self.log(f"[SN] 更新为: {sn}")
            self._send_ack()
        elif cmd == CMD_SET_KEY:
            key = data.decode('ascii', 'replace').strip('\x00')
            self.device_key = key
            self.log(f"[KEY] 更新")
            self._send_ack()
        elif cmd == CMD_SET_BROKER:
            broker = data.decode('ascii', 'replace').strip('\x00')
            self.log(f"[BROKER] 更新为: {broker}")
            self._send_ack()
        elif cmd == CMD_FACTORY_RESET:
            self.log("[SYS] 恢复出厂设置")
            self._send_ack()
        elif cmd == CMD_HEARTBEAT:
            self.last_heartbeat = time.time()
            self.log("[HB] 收到 ARM 心跳")
            self._send_ack()
        elif cmd == CMD_COMMAND_SEND:
            # ARM 主动发送命令响应
            self._handle_command_response(data)
            self._send_ack()
        else:
            self.log(f"[RX] 未处理命令: 0x{cmd:02X}")
            self._send_ack()

    def _handle_publish(self, data):
        """处理 PUBLISH 命令，转发到 MQTT"""
        try:
            json_str = data.decode('utf-8')
            obj = json.loads(json_str)

            topic = obj.get('topic', '')
            payload = obj.get('payload', '')
            sn = obj.get('sn', self.device_sn)

            if topic and payload:
                mqtt_topic = f"cs_inv/{sn}/{topic}"

                # 添加信封：时间戳 + data
                ts = int(time.time())
                try:
                    payload_obj = json.loads(payload)
                except (json.JSONDecodeError, TypeError):
                    payload_obj = payload
                envelope = json.dumps({
                    "t": ts,
                    "data": payload_obj
                }, ensure_ascii=False)

                if self.mqtt_connected and self.mqtt_client:
                    self.mqtt_client.publish(mqtt_topic, envelope)
                    self.log(f"[MQTT->] {mqtt_topic} ({len(envelope)} bytes)")
                else:
                    self.log(f"[MQTT] 未连接，丢弃: {mqtt_topic}")
        except json.JSONDecodeError as e:
            self.log(f"[ERR] JSON 解析失败: {e}")
        except Exception as e:
            self.log(f"[ERR] 发布失败: {e}")

    def _handle_command_response(self, data):
        """处理 ARM 的命令响应，转发到云端"""
        try:
            json_str = data.decode('utf-8')
            obj = json.loads(json_str)
            self.log(f"[CMD-RESP] {json.dumps(obj, ensure_ascii=False)[:200]}")

            if self.mqtt_connected and self.mqtt_client:
                topic = f"cs_inv/{self.device_sn}/cmd/response"
                ts = int(time.time())
                envelope = json.dumps({"t": ts, "data": obj}, ensure_ascii=False)
                self.mqtt_client.publish(topic, envelope)
        except Exception as e:
            self.log(f"[ERR] 命令响应处理失败: {e}")

    # ==================== UART 发送 ====================
    def _send_ack(self):
        """发送 ACK 帧"""
        frame, _ = encode_frame(CMD_ACK, b'')
        self._uart_write(frame)

    def _send_nack(self):
        """发送 NACK 帧"""
        frame, _ = encode_frame(CMD_NACK, b'')
        self._uart_write(frame)

    def send_command(self, cmd_payload):
        """向 ARM 发送命令帧（COMMAND_RECV）"""
        data = cmd_payload.encode('utf-8') if isinstance(cmd_payload, str) else cmd_payload
        frame, _ = encode_frame(CMD_COMMAND_RECV, data)
        self._uart_write(frame)
        self.log(f"[TX] COMMAND_RECV ({len(data)} bytes)")

    def _uart_write(self, frame_bytes):
        """写入串口"""
        with self._lock:
            if self.serial_port and self.serial_port.is_open:
                try:
                    self.serial_port.write(frame_bytes)
                except Exception as e:
                    self.log(f"[ERR] 串口写入失败: {e}")

    # ==================== MQTT 管理 ====================
    def mqtt_connect(self, broker=None, port=None):
        """连接 MQTT Broker"""
        if not HAS_MQTT:
            self.log("[MQTT] paho-mqtt 未安装")
            return False

        if broker:
            self.mqtt_broker = broker
        if port:
            self.mqtt_port = port

        try:
            self.mqtt_client = mqtt.Client(client_id=self.device_sn)
            self.mqtt_client.on_connect = self._on_mqtt_connect
            self.mqtt_client.on_disconnect = self._on_mqtt_disconnect
            self.mqtt_client.on_message = self._on_mqtt_message

            # 设置 LWT
            lwt_topic = f"cs_inv/{self.device_sn}/status"
            lwt_payload = json.dumps({"online": False, "sn": self.device_sn})
            self.mqtt_client.will_set(lwt_topic, lwt_payload, qos=1, retain=True)

            self.log(f"[MQTT] 连接 {self.mqtt_broker}:{self.mqtt_port}...")
            self.mqtt_client.connect(self.mqtt_broker, self.mqtt_port, keepalive=60)
            self.mqtt_client.loop_start()
            return True
        except Exception as e:
            self.log(f"[MQTT] 连接失败: {e}")
            return False

    def mqtt_disconnect(self):
        """断开 MQTT"""
        if self.mqtt_client:
            self.mqtt_client.loop_stop()
            self.mqtt_client.disconnect()
            self.mqtt_connected = False
            self.log("[MQTT] 已断开")

    def _on_mqtt_connect(self, client, userdata, flags, rc):
        """MQTT 连接回调"""
        if rc == 0:
            self.mqtt_connected = True
            self.log("[MQTT] 连接成功!")

            # 发布上线状态
            status_topic = f"cs_inv/{self.device_sn}/status"
            online = json.dumps({"online": True, "sn": self.device_sn})
            client.publish(status_topic, online, retain=True)

            # 订阅命令主题
            cmd_topic = f"cs_inv/{self.device_sn}/cmd"
            client.subscribe(cmd_topic)
            self.log(f"[MQTT] 订阅: {cmd_topic}")

            # 订阅 OTA 命令主题
            ota_topic = f"cs_inv/{self.device_sn}/ota/cmd"
            client.subscribe(ota_topic)
            self.log(f"[MQTT] 订阅: {ota_topic}")
        else:
            self.log(f"[MQTT] 连接失败, rc={rc}")

    def _on_mqtt_disconnect(self, client, userdata, rc):
        """MQTT 断开回调"""
        self.mqtt_connected = False
        if rc != 0:
            self.log(f"[MQTT] 意外断开, rc={rc}")
        else:
            self.log("[MQTT] 正常断开")

    def _on_mqtt_message(self, client, userdata, msg):
        """MQTT 消息回调 - 云端下发命令"""
        try:
            topic = msg.topic
            payload = msg.payload.decode('utf-8')
            self.log(f"[MQTT<-] {topic} ({len(payload)} bytes)")

            obj = json.loads(payload)

            # 提取命令内容，转发给 ARM
            if '/cmd' in topic and '/ota/' not in topic:
                cmd_data = json.dumps(obj, ensure_ascii=False)
                self.send_command(cmd_data)
                self.log(f"[TX] 转发命令到 ARM")
            elif '/ota/cmd' in topic:
                action = obj.get('action', '')
                self.log(f"[OTA] 收到 OTA 命令: {action}")
                # OTA 命令由 ESP32 自身处理，这里模拟响应
                self._handle_ota_command(obj)
        except Exception as e:
            self.log(f"[ERR] MQTT 消息处理失败: {e}")

    def _handle_ota_command(self, obj):
        """处理 OTA 命令（模拟）"""
        action = obj.get('action', '')
        self.log(f"[OTA] 动作: {action}")

        if action == "start":
            url = obj.get('url', '')
            version = obj.get('version', '')
            self.log(f"[OTA] 开始下载: {url} v{version}")
            # 模拟 OTA 进度
            self._simulate_ota_progress(version)
        elif action == "cancel":
            self.log("[OTA] 取消 OTA")
        elif action == "query":
            self.log("[OTA] 查询 OTA 状态")

    def _simulate_ota_progress(self, version):
        """模拟 OTA 升级进度"""
        states = ["downloading", "verifying", "installing", "rebooting", "succeeded"]

        def progress(idx=0):
            if idx < len(states):
                state = states[idx]
                self.log(f"[OTA] 状态: {state}")
                if self.mqtt_connected and self.mqtt_client:
                    topic = f"cs_inv/{self.device_sn}/ota/status"
                    payload = json.dumps({
                        "state": state,
                        "version": version,
                        "progress": min(100, (idx + 1) * 20)
                    })
                    self.mqtt_client.publish(topic, payload)
                threading.Timer(2.0, progress, args=(idx + 1,)).start()

        progress()

    # ==================== 状态查询 ====================
    def get_status(self):
        """获取模拟器状态摘要"""
        return {
            'serial_connected': self.serial_port is not None and self.serial_port.is_open,
            'mqtt_connected': self.mqtt_connected,
            'device_sn': self.device_sn,
            'mqtt_broker': f"{self.mqtt_broker}:{self.mqtt_port}",
            'ipgeo': self.ipgeo.get_status(),
            'last_heartbeat': self.last_heartbeat,
            'log_count': len(self.log_queue)
        }

    def shutdown(self):
        """关闭模拟器"""
        self.running = False
        self.mqtt_disconnect()
        self.close_serial()
        self.log("[SYS] 模拟器已关闭")


# ==================== 命令行入口 ====================
def main():
    """命令行交互模式"""
    print("=" * 60)
    print("  ESP32 WiFi 网关模拟器")
    print("=" * 60)

    sim = ESP32GatewaySimulator()

    # 列出可用串口
    ports = sim.list_serial_ports()
    if ports:
        print("\n可用串口:")
        for i, p in enumerate(ports):
            print(f"  [{i}] {p['device']} - {p['desc']}")
    else:
        print("\n未发现串口，可稍后手动连接")

    print("\n命令: open <port> | mqtt <broker> [port] | status | quit")
    print("      send <json>    - 发送命令到 ARM")
    print("      sn <serial>    - 设置设备 SN")

    try:
        while True:
            try:
                line = input("\n> ").strip()
            except EOFError:
                break

            if not line:
                continue

            parts = line.split()
            cmd = parts[0].lower()

            if cmd == 'quit' or cmd == 'exit':
                break
            elif cmd == 'open' and len(parts) >= 2:
                port = parts[1]
                baud = int(parts[2]) if len(parts) > 2 else 115200
                sim.open_serial(port, baud)
            elif cmd == 'mqtt' and len(parts) >= 2:
                broker = parts[1]
                port = int(parts[2]) if len(parts) > 2 else 1883
                sim.mqtt_connect(broker, port)
            elif cmd == 'send' and len(parts) >= 2:
                payload = ' '.join(parts[1:])
                sim.send_command(payload)
            elif cmd == 'sn' and len(parts) >= 2:
                sim.device_sn = parts[1]
                sim.log(f"[SN] 设置为: {parts[1]}")
            elif cmd == 'status':
                st = sim.get_status()
                for k, v in st.items():
                    print(f"  {k}: {v}")
            elif cmd == 'help':
                print("  open <port> [baud] - 打开串口")
                print("  mqtt <broker> [port] - 连接 MQTT")
                print("  send <json> - 发送命令到 ARM")
                print("  sn <serial> - 设置 SN")
                print("  status - 查看状态")
                print("  quit - 退出")
            else:
                print(f"未知命令: {cmd}，输入 help 查看帮助")

    except KeyboardInterrupt:
        print("\n中断退出")
    finally:
        sim.shutdown()


if __name__ == '__main__':
    main()
