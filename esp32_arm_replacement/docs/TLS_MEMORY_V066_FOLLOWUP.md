# v0.6.6：共享显示缓冲，减少 TLS 握手前的常驻占用

## 新的设备证据

v0.6.5 已使用动态 TLS 缓冲，失败点从 `mbedtls_ssl_setup` 移到了
`mbedtls_ssl_handshake`。设备日志仍显示：

```
phase=before_connect free=7752 largest=7168 min=7692
Dynamic Impl: alloc(3847 bytes) failed
phase=error free=13608 largest=5632 min=780
phase=before_connect free=13948 largest=5888 min=780
Dynamic Impl: alloc(3847 bytes) failed
```

因此 v0.6.5 没有在设备上解决握手内存不足。错误后的 free/largest 是
部分 TLS 资源已清理后的快照，不能用它们断言申请失败时还有 5632 字节连续空间。
当前证据不足以证明泄漏；握手前常驻占用过大已经明确。

## 本次最小修复

原固件同时保留了开机动画 `s_band[320*16]` 和正常界面
`s_strip_pixels[320*16]`，每个占内部 RAM 10,240 字节。
开机动画与正常界面都在同一 panel task 中按先后顺序执行，不存在并发使用需求。

v0.6.6 移除动画私有数组，动画借用显示任务已有的 RGB565/DMA 条带缓冲。
动画返回后，正常界面强制完整重绘。没有新增堆分配，没有改变条带高度、
图像内容、渐入/渐出、任务栈尺寸、BLE 常驻能力或 BMS 查询逻辑。

保留 v0.6.5 的动态 TLS 缓冲、16KB 接收能力和四个连接阶段的内存日志。
不通过关闭 BLE 或缩小 HTTPS 接收能力绕过问题。

## 验证范围

真实开机动画代码的回归检查先在 v0.6.5 失败，原因是动画传给驱动的指针
没有使用 panel owner 的缓冲；修复后通过。检查覆盖两种主题、已知背景后
才开启显示、居中品牌图像、驱动原地字节交换、缓冲前后边界和完整淡出。

打包前要求完成三种配置构建、既有本地回归、应用身份描述和 SHA256 校验。
打包脚本比较同为 Production 配置的 v0.6.5/v0.6.6 ELF 内部 RAM 节，确认
静态占用实际减少 10,240 字节，记录于 release 的
`validation/static_ram_comparison.json`。

这是文件与软件行为验证；本轮没有烧录、联网验收或测量新版本的运行堆余量。
需要设备升级后重新抓取 before_connect/connected/error 内存日志，确认
`firmware=0.6.6`，并实际测试反复重连和 HTTPS OTA。

配网任务临时占用 6144 字节栈，主循环看到 WiFi connected 后可能在该任务
退出前发起 MQTT。它与首连、重试的空闲量差异吻合，但本轮没有据此修改时序。
若共享缓冲后仍不足，再依设备日志决定下一步，而不是继续盲目裁减任务栈。

BMS `bytes=0` 无回复仍需要独立检查；SN 的重启后加载与本次握手内存失败
是不同路径。
