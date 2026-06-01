#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
OTA 测试 HTTP 服务器
==================
提供固件下载服务，支持 ESP32 和 ARM OTA 测试

用法:
  python ota_test_server.py [固件目录] [端口]
  
示例:
  python ota_test_server.py ./firmware 8080
  
然后访问:
  http://localhost:8080/esp32-firmware.bin
  http://localhost:8080/arm-firmware.bin
"""

import sys
import os
import hashlib
import json
from datetime import datetime
from http.server import HTTPServer, SimpleHTTPRequestHandler

# 固件目录
FIRMWARE_DIR = sys.argv[1] if len(sys.argv) > 1 else "."
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 8080


class OTAHandler(SimpleHTTPRequestHandler):
    """自定义 HTTP 处理器"""
    
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=FIRMWARE_DIR, **kwargs)
    
    def log_message(self, format, *args):
        """自定义日志格式"""
        timestamp = datetime.now().strftime('%H:%M:%S')
        print(f"[{timestamp}] {args[0]}")
    
    def do_GET(self):
        """处理 GET 请求"""
        # API: 获取固件列表
        if self.path == '/list':
            self._send_firmware_list()
            return
        
        # API: 获取固件信息
        if self.path.startswith('/info/'):
            filename = self.path[6:]  # 去掉 /info/
            self._send_firmware_info(filename)
            return
        
        # 普通文件下载
        super().do_GET()
    
    def _send_firmware_list(self):
        """发送固件列表"""
        files = []
        try:
            for f in os.listdir(FIRMWARE_DIR):
                if f.endswith('.bin'):
                    filepath = os.path.join(FIRMWARE_DIR, f)
                    stat = os.stat(filepath)
                    files.append({
                        'name': f,
                        'size': stat.st_size,
                        'modified': datetime.fromtimestamp(stat.st_mtime).isoformat()
                    })
        except Exception as e:
            print(f"Error listing files: {e}")
        
        self._send_json({'firmwares': files})
    
    def _send_firmware_info(self, filename):
        """发送固件详细信息（含 MD5）"""
        filepath = os.path.join(FIRMWARE_DIR, filename)
        
        if not os.path.exists(filepath):
            self._send_json({'error': 'File not found'}, 404)
            return
        
        try:
            # 计算 MD5
            md5_hash = hashlib.md5()
            with open(filepath, 'rb') as f:
                for chunk in iter(lambda: f.read(8192), b''):
                    md5_hash.update(chunk)
            
            stat = os.stat(filepath)
            
            self._send_json({
                'name': filename,
                'size': stat.st_size,
                'md5': md5_hash.hexdigest(),
                'modified': datetime.fromtimestamp(stat.st_mtime).isoformat()
            })
        except Exception as e:
            self._send_json({'error': str(e)}, 500)
    
    def _send_json(self, data, status=200):
        """发送 JSON 响应"""
        response = json.dumps(data, ensure_ascii=False).encode('utf-8')
        self.send_response(status)
        self.send_header('Content-Type', 'application/json; charset=utf-8')
        self.send_header('Content-Length', len(response))
        self.send_header('Access-Control-Allow-Origin', '*')
        self.end_headers()
        self.wfile.write(response)


def main():
    """主函数"""
    # 确保固件目录存在
    if not os.path.exists(FIRMWARE_DIR):
        os.makedirs(FIRMWARE_DIR)
        print(f"创建固件目录: {FIRMWARE_DIR}")
    
    # 启动服务器
    server = HTTPServer(('0.0.0.0', PORT), OTAHandler)
    
    print("=" * 60)
    print("OTA 测试 HTTP 服务器已启动")
    print("=" * 60)
    print(f"固件目录: {os.path.abspath(FIRMWARE_DIR)}")
    print(f"访问地址: http://localhost:{PORT}")
    print("")
    print("API 接口:")
    print(f"  GET http://localhost:{PORT}/list       - 固件列表")
    print(f"  GET http://localhost:{PORT}/info/xxx.bin - 固件信息(MD5)")
    print(f"  GET http://localhost:{PORT}/xxx.bin    - 下载固件")
    print("")
    print("支持的文件: *.bin")
    print("=" * 60)
    
    # 列出当前固件
    files = [f for f in os.listdir(FIRMWARE_DIR) if f.endswith('.bin')]
    if files:
        print("当前固件文件:")
        for f in files:
            size = os.path.getsize(os.path.join(FIRMWARE_DIR, f))
            print(f"  - {f} ({size} bytes)")
    else:
        print("当前目录没有 .bin 固件文件")
        print("提示: 把固件文件放到此目录即可下载")
    print("=" * 60)
    
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n服务器已停止")


if __name__ == '__main__':
    main()
