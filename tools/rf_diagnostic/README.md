# ESP32-C2 RF 诊断工具

## 用途
诊断 ESP32-C2 射频 TX 性能问题（晶振、PA、天线、匹配网络）

## 使用方法

### 1. 编译并烧录
```bash
cd tools/rf_diagnostic
pio run -t upload
```

如果 COM 口不是默认的，指定端口：
```bash
pio run -t upload --upload-port COM11
```

### 2. 打开串口监视器
```bash
pio device monitor -b 115200
```

### 3. 观察测试结果
程序会自动运行以下测试：
- **TEST 1**: WiFi 扫描 (RX 测试) — 能扫到多少热点
- **TEST 2**: WiFi AP 广播 (TX 测试) — 手机能否搜到 `ESP32C2_RF_TEST`
- **TEST 3**: BLE 广播 (TX 测试) — 手机 BLE 能否搜到 `ESP32C2_BLE`
- **TEST 4**: RF 校准数据读取
- **TEST 5**: TX 功率配置测试
- **TEST 6**: 时钟稳定性检测（晶振频偏）
- **TEST 7**: WiFi 连接测试（综合 TX/RX）
- **TEST 8**: 天线检查提示

### 4. 重点关注

#### TEST 2 (WiFi AP) 和 TEST 3 (BLE) — 需要你配合操作：
- 程序会提示 "CHECK YOUR PHONE NOW"
- 用手机的 WiFi 设置搜索 `ESP32C2_RF_TEST`
- 用 nRF Connect 等 BLE 扫描 App 搜索 `ESP32C2_BLE`
- **如果搜不到**：TX 路径有问题（PA/天线/匹配网络）

#### TEST 6 (时钟稳定性) — 自动检测：
- Drift < 10 ppm → 晶振正常
- Drift 10-50 ppm → 晶振偏移，可能影响 RF
- Drift > 50 ppm → 晶振故障，很可能是 TX 不好的原因
- Jitter > 1000 us → 时钟不稳定

## 常见问题

### TX 搜不到但 RX 能扫到
原因分析：
1. **晶振频偏过大** → 导致 TX 频率偏移，接收端无法解调
2. **PA 损坏** → TX 功率不足
3. **天线匹配网络问题** → 阻抗不匹配，TX 信号反射
4. **Balun 焊接问题** → 差分转单端信号路径断开

### 晶振相关
- ESP32-C2 需要 40MHz 晶振，±10ppm 精度
- 负载电容要匹配（通常 12-18pF）
- 晶振走线要短，不能跨层
- 晶振下方不能有其他高频信号走线（SDIO、UART）
- 晶振旁边不能有大电感或天线
