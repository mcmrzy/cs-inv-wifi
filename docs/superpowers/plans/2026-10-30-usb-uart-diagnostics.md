# USB UART TTL 诊断固件

用户明确要求关闭屏幕，使用USB串口控制波特率、TX/RX交换、地址搜索等通信测试。采用独立工程`esp32_uart_diag`，不改完整面板工程。USB使用ESP32-C3原生Serial/JTAG(GPIO18/19)，UART0只使用GPIO0/1。配置只在RAM生效，不初始化网络、屏幕、BMS，不写NVS或eFuse。

1. 为命令边界、Modbus匹配/CRC/回显/异常和旧direct只读探测编写主机测试，先观察失败。
2. 实现单任务USB命令行与UART独占收发：速率、校验/停止位、方向、反相、读寄存器、轮询、地址搜索、组合搜索、监听、原始发送、CRC、回环和输入电平采样。长操作支持Ctrl+C/stop停止。搜索只发送03/04读请求。
3. ESP-IDF构建并核对实际配置、分区和二进制；发布App/完整4MB镜像、操作说明与测试/构建报告。硬件连通需要烧录后有效匹配应答，不能由构建代替。

WHEN RX只有发送帧的回显，THEN 输出ECHO，不记为FOUND。WHEN 收到CRC正确且目标地址/功能/数据长度匹配的应答，THEN输出READ_OK或EXCEPTION；搜索命中配置仅保留RAM。WHEN 搜索取消或无命中，THEN恢复搜索前配置。WHEN输入超长、越界或格式错误，THEN不发送、不改变配置。
