# 2026-10-15 DSP SCI 接收路径核对与 ESP 对照版

## 已检查与尚缺的材料

已读现场 0.6.24-progress 日志、当前 ESP 源码、ESP-IDF 5.5.4 UART 实现，
以及 TI 公开的 F28003x DriverLib SCI 文档与头文件。

用户给出的 DSP 工程路径为
`C:\Users\chenshuo\Downloads\f280039c_bootloader\boot`。
当前机器中 `boot.h`、`src/iap.c`、`src/serial.c`、`src/boot_main.c` 均不存在于该路径。
以下是官方 API 对照，不能冒充对实际工程副本或当前片上 Bootloader 的审查。

## 官方 API 对照

来源：

- [F28003x SCI API Guide](https://software-dl.ti.com/C2000/docs/C2000_driverlib_api_guide/f28003x/build/html/modules/sci.html)
- [TI 官方 F28003x sci.h](https://github.com/TexasInstruments/c2000ware-core-sdk/blob/main/driverlib/f28003x/driverlib/sci.h)

1. `SCI_resetRxFIFO()` 内部先清除 `SCI_FFRX_RXFIFORESET`，随后重新置位。
   `SCI_resetTxFIFO()` 同样先清再置。`SCI_enableFIFO()` 也将这两个复位位置位。
   因此“resetRxFIFO 后一直被按在复位态，必须再次 enableFIFO”不是该官方实现的语义。
2. `SCI_isDataAvailableNonFIFO()` 查询 `SCIRXST.RXRDY`，供非 FIFO 模式使用。
   FIFO 模式应通过 `SCI_getRxFIFOStatus()` 判断 FIFO 中是否存在字符。
3. `SCI_readCharNonBlocking()` 直接读接收缓冲，官方注释允许在判断非 FIFO 接收缓冲
   或 FIFO 有数据后使用；不能仅凭其函数名把它判成非 FIFO 专用。
4. FIFO 模式的阻塞发送使用 `SCI_writeCharBlockingFIFO()`，避免以非 FIFO 的 TXRDY
   作为 FIFO 空间判断依据。

如果实际 serial.c 与用户贴出的组合一致，最小审查方向是：保留 FIFO 初始化，
将收数可用判断换成 FIFO 状态，将阻塞发送换成 FIFO 版本。下列仅为适配示意，
并未修改、编译或烧录 DSP 工程：

```c
if (SCI_getRxFIFOStatus(OTA_SCI_BASE) != SCI_FIFO_RX0) {
    return (int)(SCI_readCharNonBlocking(OTA_SCI_BASE) & 0x00FFU);
}
/* 在 sci_putc 的实际实现中使用： */
SCI_writeCharBlockingFIFO(OTA_SCI_BASE, (uint16_t)ch);
```

还需核对 `wait_byte()` 是否每次按起始时间差计时、计时变量是否按 ISR 共享方式声明、
CPUTimer0 时钟/中断是否实际运行、错误/溢出后如何恢复。仅凭 g_idle_ms 是累计量不能
判错：累计单调时基配合起始时间差是正常计时方法。
把字节间超时改成 200 ms 不能修复模式判断错误，也不能保证容忍任意主站间隙。

## ESP 0.6.25-progress 对照版

包：`release/CSERGY_ESP32C3_v0.6.25_Progress_20261015.zip`。

DSP TX 软件环形缓冲由 2048 改为 0，控制帧直接写入 128 字节硬件 FIFO；BMS 保持
8192 字节 TX 缓冲。保留原有整帧发送和按帧长计算的发送完成等待。
新增失败阶段及该阶段 TX/RX 计数、最后帧长、最大发送墙钟耗时和 RX 前缀。
失败快照在恢复 EOT 之前保存，main_loop 解锁后才格式化输出。

完整主机回归和 ESP-IDF 5.5.4 构建通过；应用 1,520,240 字节。
app SHA256：`2bf5a3d0c7025240f3989f423cfa71cfcdc99f680c43bdbdf9ce6d71ac306c3a`。
未烧录，未完成物理 OTA 验收。软件发送耗时不是线路字节间隙测量。
