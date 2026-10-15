# DSP OTA 镜像格式问题（0.6.20-dsp-ota-diag）

## 现象

云端推送 `DSP-02A / 1.0.50` 后，设备串口出现：

```
E (23352) OTA: dsp direct Boot transfer failed (1)
```

App/云端把它显示成 `arm transfer failed`。当前链路已经是
ESP32-C3 → DSP(F280039C) 直连，没有 ARM 参与；`ARM_TRANSFER_FAILED`
是复用旧错误码造成的误导，0.6.20 已按目标拆分为
`DSP_TRANSFER_FAILED` / `BMS_TRANSFER_FAILED` / `IMAGE_FORMAT_INVALID` /
`OTA_BUSY` / `TARGET_NOT_READY`。

## 根因：上传的是 CCS 输出的 Intel HEX 文本，不是打包镜像

云端记录 `1.0.50` 的 SHA-256 前缀 `c584a8d137b5` 与本地
`C:\Users\29538\Downloads\Invt280039C2_1.0.50.hex` 完全一致：

| 项目 | 实测值 |
|---|---|
| SHA-256 | `c584a8d137b5e00ed3885c06b65b1e14cf2ad92809f94aeeb541ecac8ece29fd` |
| 大小 | 110491 字节（奇数） |
| 前 6 字节 | `3a 30 32 30 30 30`（ASCII `:02000`） |
| 首条数据记录 | `0x082000: 00 48 75 C1`，下一条 `0x082008: 76 48 4D 38 …` |

用真实固件函数 `direct_dsp_ota_flash()` 直接喂这份文件（哈希匹配）：

```
PASS exact SHA-matched DSP 1.0.50 HEX: odd ASCII size rejected;
     padding still fails LB; zero CMD42, erase or data frames
```

即：先因长度为奇数返回 `IMAGE_SIZE_INVALID`；即便补齐长度，首个大端字仍是
`0x3A30` 而不是 `(w0 & 0xFFFC) == 0x0048`，返回 `IMAGE_LB_OPCODE_INVALID`。
两种情况下都**没有**发出 CMD42、擦除帧或数据帧，DSP 没有被复位/擦写。

`docs/esp32/dsp_ota.h` 与 `docs/esp32/README.md` 要求的是
`py host\pack_f28003x.py app.out app.bin --version …` 的产物
（App 代码 + 8 字镜像头 magic/length/CRC32/version，每字高字节在前）。
CCS 直接导出的 `.hex` 不是这个格式，改扩展名或补一个字节都不行。

## 与 F280039C_ARM_GUIDE.html 的对照

ESP 侧按该指南逐项核对，均已是要求的行为：

| 指南要求 | ESP 现状 |
|---|---|
| IAP 帧不变（HR / 擦除 / 1024B 数据包 / EOT） | 与指南逐字节一致（CRC 命令帧低字节先、数据帧高字节先） |
| 包数上限 367（App 区 0x082000~0x0AFEEF） | `DIRECT_DSP_OTA_MAX_PACKETS = 367`，上限 375808 字节 |
| 删除 bank 选择逻辑 | 无 bank 逻辑 |
| HR 首次等待 ≤3 s | 2 s，最多 5 次 |
| 擦除 12 s / 每包 3 s×10 | 一致 |
| 传输前停掉 03/04/25 轮询 | `ota_quiet_begin()` + UART owner 独占 |

## 还缺什么

1. **镜像出处**：`f28003x_boot` 工程的 `host/pack_f28003x.py`（或
   `f28003x_boot/src/iap.c` 里镜像头的字段布局、长度单位、CRC32 算法与
   覆盖范围）。本机没有该工程/脚本，也没有任何一份已打包的 F280039C
   `.bin` 可作参照。F28335 的 `dsp_v1.4.0.bin` /`Invt28335_G9E1_400V(1).bin`
   （393200 字节）首字不是 LB，也不含 `0x5A5A`，不能当模板。
2. **触发路径**：指南给了"复位 + HR"和"App 主动交接"两条路。按
   `docs/ESP32C3_PINOUT.md`，ESP 只接 DSP 的 UART0（GPIO0/1），固件里没有
   DSP 复位 GPIO，所以 ESP 目前只能用第二条路（发 `0x01 05 42 …`，
   等 App 交接）。可用 USB 串口管理员命令 `dsp_boot <密码>` 先验证 DSP
   App 是否真的实现了 0x42 交接：回 `HR` 才算通。

## 0.6.20 包含的改动

- 直连 DSP/BMS 失败原因细分，不再报 `ARM_TRANSFER_FAILED`。
- 进入直连升级前把**实际**镜像长度和前 6 字节写进串口与云端 trace。
- 暂存文件短写改报 `FLASH_WRITE_FAILED`；本地 BLE 路径也带回具体失败原因。
- 保留 0.6.19 的远程 OTA 内存修复与 0.6.18 的显示修复。

本版未烧录，DSP 实机升级未验收。
