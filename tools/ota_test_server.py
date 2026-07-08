#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
OTA测试HTTP服务器
================
为ESP32 OTA升级提供本地HTTP固件下载服务。

功能：
- 提供固件文件下载
- 显示下载进度和状态
- 支持断点续传（可选）
- 支持MQTT命令发送（可选）

用法:
  python ota_test_server.py                        # 启动服务器
  python ota_test_server.py --port 8080            # 指定端口
  python ota_test_server.py --firmware arm_firmware.bin  # 指定固件文件
  python ota_test_server.py --mqtt                 # 同时发送MQTT OTA命令
"""

import os
import sys
import json
import time
import argparse
import hashlib
import socket
from http.server import HTTPServer, SimpleHTTPRequestHandler
from datetime import datetime
from urllib.parse import urlparse, parse_qs

# 获取本机IP地址
def get_local_ip():
    """获取本机局域网IP地址"""
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
        s.close()
        return ip
    except:
        return "127.0.0.1"


class OTARequestHandler(SimpleHTTPRequestHandler):
    """OTA HTTP请求处理器"""
    
    firmware_path = None
    firmware_info = None
    download_count = 0
    start_time = None
    
    def do_GET(self):
        """处理GET请求"""
        parsed = urlparse(self.path)
        
        # 固件下载
        if parsed.path == '/firmware' or parsed.path == '/firmware.bin':
            self._serve_firmware()
        # 固件信息
        elif parsed.path == '/info':
            self._serve_info()
        # 状态页面
        elif parsed.path == '/status':
            self._serve_status()
        # 首页
        elif parsed.path == '/' or parsed.path == '/index.html':
            self._serve_index()
        else:
            super().do_GET()
    
    def _serve_firmware(self):
        """提供固件下载"""
        if not self.firmware_path or not os.path.exists(self.firmware_path):
            self.send_error(404, "Firmware not found")
            return
        
        try:
            file_size = os.path.getsize(self.firmware_path)
            
            # 发送响应头
            self.send_response(200)
            self.send_header('Content-Type', 'application/octet-stream')
            self.send_header('Content-Length', str(file_size))
            self.send_header('Content-Disposition', 'attachment; filename="arm_firmware.bin"')
            self.send_header('X-Firmware-Version', self.firmware_info.get('version', 'unknown'))
            self.send_header('X-Firmware-MD5', self.firmware_info.get('md5', ''))
            self.send_header('X-Firmware-SHA256', self.firmware_info.get('sha256', ''))
            self.end_headers()
            
            # 发送固件数据
            downloaded = 0
            with open(self.firmware_path, 'rb') as f:
                while True:
                    chunk = f.read(4096)
                    if not chunk:
                        break
                    self.wfile.write(chunk)
                    downloaded += len(chunk)
                    
                    # 显示进度
                    if downloaded % (1024 * 10) == 0 or downloaded == file_size:
                        progress = (downloaded * 100) // file_size
                        print(f"\r[下载] {progress}% ({downloaded}/{file_size} bytes)", end='', flush=True)
            
            print()  # 换行
            OTARequestHandler.download_count += 1
            print(f"[OK] 固件下载完成: {self.firmware_info.get('version', 'unknown')}")
            
        except Exception as e:
            print(f"[ERROR] 下载失败: {e}")
            self.send_error(500, str(e))
    
    def _serve_info(self):
        """提供固件信息"""
        if not self.firmware_info:
            self.send_error(404, "No firmware loaded")
            return
        
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.end_headers()
        self.wfile.write(json.dumps(self.firmware_info, indent=2, ensure_ascii=False).encode('utf-8'))
    
    def _serve_status(self):
        """提供服务器状态"""
        status = {
            'server': 'OTA Test Server',
            'firmware': os.path.basename(self.firmware_path) if self.firmware_path else None,
            'version': self.firmware_info.get('version') if self.firmware_info else None,
            'size': self.firmware_info.get('size_bytes') if self.firmware_info else None,
            'md5': self.firmware_info.get('md5') if self.firmware_info else None,
            'downloads': OTARequestHandler.download_count,
            'uptime': time.time() - OTARequestHandler.start_time if OTARequestHandler.start_time else 0,
            'timestamp': datetime.now().isoformat()
        }
        
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.end_headers()
        self.wfile.write(json.dumps(status, indent=2).encode('utf-8'))
    
    def _serve_index(self):
        """提供首页"""
        html = """<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <title>OTA测试服务器</title>
    <style>
        body { font-family: Arial, sans-serif; margin: 40px; background: #f5f5f5; }
        .container { max-width: 800px; margin: 0 auto; background: white; padding: 30px; border-radius: 10px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }
        h1 { color: #333; border-bottom: 2px solid #4CAF50; padding-bottom: 10px; }
        .info { background: #e8f5e9; padding: 15px; border-radius: 5px; margin: 15px 0; }
        .download-btn { display: inline-block; background: #4CAF50; color: white; padding: 12px 24px; text-decoration: none; border-radius: 5px; margin: 10px 5px; }
        .download-btn:hover { background: #45a049; }
        .mqtt-btn { background: #2196F3; }
        .mqtt-btn:hover { background: #1976D2; }
        pre { background: #f5f5f5; padding: 15px; border-radius: 5px; overflow-x: auto; }
    </style>
</head>
<body>
    <div class="container">
        <h1>OTA测试服务器</h1>
        
        <div class="info">
            <h3>固件信息</h3>
            <p><strong>文件:</strong> {firmware_file}</p>
            <p><strong>版本:</strong> {version}</p>
            <p><strong>大小:</strong> {size_kb} KB</p>
            <p><strong>MD5:</strong> {md5}</p>
        </div>
        
        <h3>下载链接</h3>
        <a href="/firmware" class="download-btn">下载固件</a>
        <a href="/info" class="download-btn">固件信息(JSON)</a>
        <a href="/status" class="download-btn">服务器状态</a>
        
        <h3>MQTT OTA命令示例</h3>
        <pre>
{mqtt_example}
        </pre>
        
        <h3>使用说明</h3>
        <ol>
            <li>确保ESP32和此服务器在同一网络</li>
            <li>通过MQTT发送OTA命令，URL指向此服务器</li>
            <li>观察模拟器日志查看OTA进度</li>
        </ol>
    </div>
</body>
</html>"""
        
        local_ip = get_local_ip()
        port = self.server.server_address[1]
        
        mqtt_example = json.dumps({
            "command": "start",
            "target": "arm",
            "url": f"http://{local_ip}:{port}/firmware",
            "version": self.firmware_info.get('version', 'V1.3.0'),
            "file_size": self.firmware_info.get('size_bytes', 0),
            "task_id": "test_ota_001"
        }, indent=2, ensure_ascii=False)
        
        content = html.format(
            firmware_file=os.path.basename(self.firmware_path) if self.firmware_path else 'N/A',
            version=self.firmware_info.get('version', 'N/A') if self.firmware_info else 'N/A',
            size_kb=self.firmware_info.get('size_kb', 0) if self.firmware_info else 0,
            md5=self.firmware_info.get('md5', 'N/A') if self.firmware_info else 'N/A',
            mqtt_example=mqtt_example
        )
        
        self.send_response(200)
        self.send_header('Content-Type', 'text/html; charset=utf-8')
        self.end_headers()
        self.wfile.write(content.encode('utf-8'))
    
    def log_message(self, format, *args):
        """自定义日志格式"""
        print(f"[{datetime.now().strftime('%H:%M:%S')}] {args[0]}")


def load_firmware_info(firmware_path):
    """加载固件信息"""
    info_path = firmware_path.replace('.bin', '_info.json')
    
    if os.path.exists(info_path):
        try:
            with open(info_path, 'r', encoding='utf-8') as f:
                return json.load(f)
        except:
            pass
    
    # 如果没有info文件，计算基本信息
    if os.path.exists(firmware_path):
        with open(firmware_path, 'rb') as f:
            data = f.read()
        
        return {
            'version': 'unknown',
            'size_bytes': len(data),
            'size_kb': len(data) // 1024,
            'md5': hashlib.md5(data).hexdigest(),
            'sha256': hashlib.sha256(data).hexdigest()
        }
    
    return None


def send_mqtt_ota_command(host, port, firmware_info):
    """发送MQTT OTA命令（可选）"""
    try:
        import paho.mqtt.client as mqtt
    except ImportError:
        print("[WARN] paho-mqtt未安装，跳过MQTT命令发送")
        print("       安装: pip install paho-mqtt")
        return False
    
    local_ip = get_local_ip()
    server_port = 8080  # HTTP服务器端口
    
    ota_command = {
        "command": "start",
        "target": "arm",
        "url": f"http://{local_ip}:{server_port}/firmware",
        "version": firmware_info.get('version', 'V1.3.0'),
        "file_size": firmware_info.get('size_bytes', 0),
        "task_id": f"test_ota_{int(time.time())}"
    }
    
    print(f"\n[MQTT] 发送OTA命令到 {host}:{port}")
    print(f"[MQTT] 固件URL: {ota_command['url']}")
    
    client = mqtt.Client()
    try:
        client.connect(host, port, 60)
        client.publish("ota/cmd", json.dumps(ota_command))
        client.disconnect()
        print("[MQTT] OTA命令已发送")
        return True
    except Exception as e:
        print(f"[MQTT] 发送失败: {e}")
        return False


def main():
    parser = argparse.ArgumentParser(
        description='OTA测试HTTP服务器',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
示例:
  %(prog)s                                          # 启动服务器
  %(prog)s --port 8080                              # 指定端口
  %(prog)s --firmware arm_firmware.bin               # 指定固件
  %(prog)s --mqtt --mqtt-host 192.168.1.100         # 同时发送MQTT命令
        """
    )
    
    parser.add_argument('--port', type=int, default=8080,
                        help='HTTP服务器端口 (默认: 8080)')
    parser.add_argument('--firmware', type=str, default='arm_firmware.bin',
                        help='固件文件路径 (默认: arm_firmware.bin)')
    parser.add_argument('--dir', type=str, default=None,
                        help='固件文件所在目录 (默认: 当前目录)')
    parser.add_argument('--mqtt', action='store_true',
                        help='发送MQTT OTA命令')
    parser.add_argument('--mqtt-host', type=str, default='localhost',
                        help='MQTT服务器地址 (默认: localhost)')
    parser.add_argument('--mqtt-port', type=int, default=1883,
                        help='MQTT服务器端口 (默认: 1883)')
    
    args = parser.parse_args()
    
    # 固件文件路径
    if args.dir:
        firmware_path = os.path.join(args.dir, args.firmware)
    else:
        firmware_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), args.firmware)
    
    # 检查固件文件
    if not os.path.exists(firmware_path):
        print(f"[ERROR] 固件文件不存在: {firmware_path}")
        print("\n请先生成固件:")
        print("  python generate_arm_firmware.py")
        sys.exit(1)
    
    # 加载固件信息
    firmware_info = load_firmware_info(firmware_path)
    if not firmware_info:
        print("[ERROR] 无法加载固件信息")
        sys.exit(1)
    
    # 设置处理器参数
    OTARequestHandler.firmware_path = firmware_path
    OTARequestHandler.firmware_info = firmware_info
    OTARequestHandler.start_time = time.time()
    
    # 获取本机IP
    local_ip = get_local_ip()
    
    print("=" * 60)
    print("OTA测试服务器")
    print("=" * 60)
    print(f"固件文件: {os.path.basename(firmware_path)}")
    print(f"固件版本: {firmware_info.get('version', 'unknown')}")
    print(f"固件大小: {firmware_info.get('size_kb', 0)} KB")
    print(f"固件MD5: {firmware_info.get('md5', 'N/A')[:16]}...")
    print("-" * 60)
    print(f"服务器地址: http://{local_ip}:{args.port}")
    print(f"固件下载: http://{local_ip}:{args.port}/firmware")
    print(f"固件信息: http://{local_ip}:{args.port}/info")
    print(f"服务器状态: http://{local_ip}:{args.port}/status")
    print("=" * 60)
    print("\n等待下载请求...\n")
    
    # 发送MQTT命令（可选）
    if args.mqtt:
        send_mqtt_ota_command(args.mqtt_host, args.mqtt_port, firmware_info)
    
    # 启动HTTP服务器
    try:
        server = HTTPServer(('0.0.0.0', args.port), OTARequestHandler)
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n[INFO] 服务器已停止")
    except Exception as e:
        print(f"\n[ERROR] 服务器启动失败: {e}")
        sys.exit(1)


if __name__ == '__main__':
    main()
