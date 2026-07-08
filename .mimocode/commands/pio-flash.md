---
description: "PlatformIO 编译+烧录 ESP32 固件 (Arduino 框架)"
---

# PlatformIO 编译烧录命令

编译 ESP32 Arduino 固件, 可选烧录到指定串口。

## 用法

用户输入: `$ARGUMENTS` — 格式为 `[port] [--env env_name] [--no-flash]`

- `port`: 串口号, 如 COM7 (默认 COM7)
- `--env`: PlatformIO 环境名 (默认 `esp32-c3-devkitm-1`)
- `--no-flash`: 仅编译不烧录

## 执行步骤

1. 解析参数: 提取 port (默认 COM7)、env (默认 esp32-c3-devkitm-1)、是否仅编译
2. 确认工作目录为 `D:\CS_INV_WIFI` (含 platformio.ini)
3. 编译:
   ```powershell
   pio run -e <env> 2>&1
   ```
   - 超时 180 秒
   - 若编译失败, 输出错误并停止, 不继续烧录
4. 烧录 (若未指定 --no-flash):
   ```powershell
   pio run -e <env> -t upload --upload-port <port> 2>&1
   ```
   - 超时 120 秒
   - **前提**: 串口监视器已关闭, 串口未被占用
5. 输出编译结果 (固件大小、RAM 使用) 和烧录结果

## 注意事项

- 烧录前需关闭串口监视器 (COM7 占用问题)
- 若编译报 `RAM > 95%` 警告, 提醒用户检查内存使用
- 项目根目录: `D:\CS_INV_WIFI`
- platformio.ini 中定义了 `esp32-c3-devkitm-1` 环境
