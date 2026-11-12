# 0.4.27 补传任务栈溢出修复

用户日志中的固件为 0.4.26，ELF SHA256 为 `3b064089c762148ed79ddd188ca0835f8288072b00f85a61386b465e325f5cac`，与本机 ELF、应用描述中的哈希一致。原 ELF 保存在 `.release-work/cache-reboot-0426/crashed-0.4.26.elf`。

首个明确异常为 `cache_flush` 任务的 Stack protection fault，栈范围约 4096 字节。`0x4210f34e` 和 `0x4210f36c` 对应 `_vfprintf_r`，返回地址 `0x42111818` 对应 `__sbprintf`。后续 Store/Instruction access fault 出现在连续异常之后，不能据此判断为另一项独立硬件问题。

实际 ESP32-C3 编译器报告的修复前静态帧如下。这些数字尚不包括 libc、SPIFFS、cJSON 和网络库的下层调用。

- offline_cache_flush_all 为 1408 字节。
- flush_batch 为 640 字节。
- cache_batch_encode 为 1104 字节。

修复包含三项。

1. 压缩/解压的 1024 字节临时数组改用 cJSON 分配器按需分配，失败时返回并保留原批次，测试覆盖分配失败后的再次恢复。
2. 补传回调改用 esp_mqtt_client_enqueue，SDK 将载荷复制到 outbox，由 8192 字节栈的 MQTT 任务负责网络与 TLS。补传任务继续等待对应消息 ID 的 PUBACK，队列满或没有确认都不会视为发送成功。
3. 补传任务改为 6144 字节栈，任务结束检查历史最低余量，低于 1024 字节输出告警。缓存模块增加单函数 1536 字节栈帧编译门槛。

主机回归验证禁止补传调用同步 publish，覆盖原始 V3、长载荷、队列满、缺失确认和 OTA 暂停。压缩还原和实际文件提交测试继续运行。目标编译器的前后栈报告保存在 `.release-work/cache-reboot-0426/`。

这次修复不要求清空缓存、修改串口引脚或重新初始化屏幕。实机升级后仍需观察联网补传期间是否再次异常。主机测试和静态栈报告不能代替硬件运行验证。

修复后目标编译器报告：cache_batch_encode 为 80 字节，offline_cache_flush_all 为 688 字节，flush_batch 为 640 字节，offline_send_callback 为 80 字节。压缩函数减少 1024 字节，补传主函数减少 720 字节。完整主机回归和 ESP32-C3 构建通过，应用槽保留原有 56 KiB 最低余量。尚未在用户硬件上复现升级后的联网补传。
