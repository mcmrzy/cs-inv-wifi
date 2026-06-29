# OTA 功能测试指南

## 一、测试环境准备

### 1.1 硬件准备
- ESP32-C3 或 ESP8266 开发板
- ARM 模拟器（运行 `arm_inverter_simulator_gui.py`）
- USB 转串口线（连接 ESP32 和电脑）

### 1.2 软件准备
- PlatformIO（编译 ESP32 固件）
- Python 3（运行 HTTP 服务器和 ARM 模拟器）

---

## 二、测试 ESP32 OTA

### 步骤 1：编译第一个固件（当前版本）

```bash
cd d:\trae_project\CS_INV_Inverter_Project\arduino_inverter_wifi
# 修改 platformio.ini 中的版本号，例如：
# build_flags = -DFIRMWARE_VERSION=\"1.0.0\"
pio run
```

复制生成的固件：
```bash
copy .pio\build\esp32-c3-devkitm-1\firmware.bin d:\firmware\esp32-v1.0.0.bin
```

### 步骤 2：修改代码，编译第二个固件（新版本）

修改 `config.h` 中的版本号：
```cpp
#define FIRMWARE_VERSION "1.0.1"
```

或者修改其他可识别的特征（如 LED 闪烁频率、打印信息等）。

重新编译：
```bash
pio run
copy .pio\build\esp32-c3-devkitm-1\firmware.bin d:\firmware\esp32-v1.0.1.bin
```

### 步骤 3：启动 HTTP 服务器

```bash
cd d:\trae_project\CS_INV_Inverter_Project\arduino_inverter_wifi\tools
python ota_test_server.py d:\firmware 8080
```

访问 http://localhost:8080/list 查看固件列表。

### 步骤 4：烧录旧版本固件到 ESP32

```bash
pio run --target upload
```

### 步骤 5：通过 MQTT 触发 OTA

使用 MQTT 客户端（如 MQTT.fx、mosquitto_pub）发送命令：

**主题**: `cs_inv/{你的SN}/ota/cmd`

**Payload**:
```json
{
  "action": "start",
  "target": "esp",
  "url": "http://你的IP:8080/esp32-v1.0.1.bin"
}
```

### 步骤 6：观察升级过程

1. ESP32 串口打印会显示下载进度
2. 升级完成后自动重启
3. 重启后检查版本号是否变为 1.0.1

---

## 三、测试 ARM OTA

### 步骤 1：准备 ARM 模拟固件

创建一个测试用的 "ARM 固件" 文件：

```bash
# 创建一个 100KB 的测试文件（模拟 ARM 固件）
python -c "import os; f=open('arm-v1.2.4.bin','wb'); f.write(b'X'*102400); f.close()"
```

或者使用真实的 GD32F303 固件 `.bin` 文件。

### 步骤 2：启动 ARM 模拟器

```bash
cd d:\trae_project\CS_INV_Inverter_Project\arduino_inverter_wifi\tools
python arm_inverter_simulator_gui.py
```

连接串口，确认通信正常。

### 步骤 3：通过 MQTT 触发 ARM OTA

**主题**: `cs_inv/{你的SN}/ota/cmd`

**Payload**:
```json
{
  "action": "start",
  "target": "arm",
  "url": "http://你的IP:8080/arm-v1.2.4.bin"
}
```

### 步骤 4：观察升级过程

1. ESP32 串口显示下载 ARM 固件
2. 通过 UART 逐包发送给 ARM 模拟器
3. ARM 模拟器显示 OTA 进度
4. 升级完成后 ARM 模拟器显示新固件版本

---

## 四、通过 Web 页面触发 OTA

### 步骤 1：连接 ESP32 热点或同一 WiFi

确保电脑能访问 ESP32 的 Web 页面（`192.168.4.1` 或 ESP32 的 IP）。

### 步骤 2：打开 OTA 页面

浏览器访问：`http://192.168.4.1/ota`

### 步骤 3：填写固件 URL

- 选择目标：`ESP32` 或 `ARM`
- 填写 URL：`http://你的IP:8080/esp32-v1.0.1.bin`
- 点击"开始升级"

### 步骤 4：查看进度

页面会显示下载进度和状态。

---

## 五、常见问题排查

### 5.1 HTTP 下载失败

**现象**: ESP32 打印 "HTTP error" 或 "Download failed"

**排查**:
1. 检查 HTTP 服务器是否运行
2. 检查 URL 是否正确（IP 和端口）
3. 检查防火墙是否允许访问
4. 尝试用浏览器直接访问 URL

### 5.2 ARM OTA 超时

**现象**: ESP32 发送 OTA_START 后无响应

**排查**:
1. 检查 ARM 模拟器是否连接串口
2. 检查串口通信是否正常（心跳是否正常）
3. 检查 ARM 模拟器是否进入 IAP 模式

### 5.3 校验失败

**现象**: 固件下载完成但校验失败

**排查**:
1. 检查固件文件是否完整
2. 检查 MD5 计算是否正确
3. 检查传输过程中是否有数据损坏

### 5.4 重启后还是旧版本

**现象**: OTA 显示成功但重启后版本未变

**排查**:
1. 检查固件是否烧录到正确的分区
2. 检查 ESP32 的 partition table
3. 检查是否有多个固件文件混淆

---

## 六、测试检查清单

- [ ] ESP32 OTA 成功，版本号更新
- [ ] ARM OTA 成功，模拟器显示新版本
- [ ] Web 页面 OTA 功能正常
- [ ] MQTT 远程 OTA 功能正常
- [ ] 升级过程中断网能恢复
- [ ] 升级失败能回滚/重试
- [ ] 大文件（>100KB）OTA 正常

---

## 七、相关文件

- `ota_test_server.py` - HTTP 测试服务器
- `arm_inverter_simulator_gui.py` - ARM 模拟器
- `OTA_Design.md` - OTA 系统设计文档
