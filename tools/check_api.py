#!/usr/bin/env python3
"""检查 MQTT API 兼容性"""

import re
import os

src_dir = os.path.join(os.path.dirname(__file__), '..', 'src')

# 读取 mqtt_handler.h
with open(os.path.join(src_dir, 'mqtt_handler.h'), 'r') as f:
    header = f.read()

# 检查 main.cpp 中的调用
with open(os.path.join(src_dir, 'main.cpp'), 'r') as f:
    main_cpp = f.read()

# 检查 mqtt_handler.cpp
with open(os.path.join(src_dir, 'mqtt_handler.cpp'), 'r') as f:
    mqtt_cpp = f.read()

print("=== MQTT API 检查 ===\n")

# 检查函数签名
api_checks = [
    ('mqtt_init', r'bool\s+mqtt_init\s*\(\s*MQTTMessageHandler\s*\)'),
    ('mqtt_connect', r'bool\s+mqtt_connect\s*\(\s*void\s*\)'),
    ('mqtt_loop', r'void\s+mqtt_loop\s*\(\s*void\s*\)'),
    ('mqtt_publish', r'int\s+mqtt_publish\s*\([^)]+\)'),
]

print("头文件 API 定义:")
for name, pattern in api_checks:
    match = re.search(pattern, header)
    status = "✓" if match else "✗"
    print(f"  {status} {name}")

print("\nmain.cpp 调用检查:")
calls = [
    'mqtt_init(on_mqtt_message)',
    'mqtt_loop()',
    'mqtt_publish(',
    'mqtt_is_connected()',
    'mqtt_disconnect()',
]

for call in calls:
    count = main_cpp.count(call)
    status = "✓" if count > 0 else "○"
    print(f"  {status} {call} (x{count})")

print("\n库依赖检查:")
if 'PubSubClient' in mqtt_cpp:
    print("  ✓ 使用 PubSubClient")
if 'WiFiClientSecure' in mqtt_cpp:
    print("  ✓ 使用 WiFiClientSecure (TLS)")
if 'MQTT_CA_CERT' in mqtt_cpp:
    print("  ✓ 使用 CA 证书")

print("\n完成!")
