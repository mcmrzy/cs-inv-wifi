/*
 * debug_log.h — 调试模式串口日志上云
 *
 * 单设备调试模式 (云端 set_debug_telemetry) 生效期间, 把设备串口日志按批上行到
 * cs_inv/{sn}/debug/log, 让云端/App 远程判读设备侧日志 (ARM 轮询超时、OTA、
 * WiFi 重连等), 不必现场接串口。
 *
 *  - 串口输出不受影响: 钩子只旁路复制一份, 原样继续打印。
 *  - 缓冲始终滚动 (溢出丢最旧), 仅调试模式生效时上行 —— 打开调试模式后能立刻
 *    看到开启前的一小段历史。
 *  - 走 mqtt_publish_volatile: 只实时发, 不写离线缓存 (调试日志补传无价值,
 *    且不该挤占遥测补传槽位)。
 */
#ifndef DEBUG_LOG_H
#define DEBUG_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

/* 安装串口日志钩子 (app_main 早期调用一次) */
void debug_log_init(void);

/* 主循环调用 (100ms): 调试模式生效且 MQTT 在线时按批上行缓冲日志 */
void debug_log_tick(void);

#ifdef __cplusplus
}
#endif

#endif /* DEBUG_LOG_H */
