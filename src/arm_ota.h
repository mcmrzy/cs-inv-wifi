/**
 * ================================================================
 *  ARM OTA 模块 — 通过 UART 给 GD32F303 ARM 芯片做固件升级
 *
 *  通信协议:
 *    ESP32 -> ARM: CMD_ARM_OTA_START (0x10)  通知进入 IAP 模式
 *    ESP32 -> ARM: CMD_ARM_OTA_DATA  (0x11)  发送固件数据包
 *    ESP32 -> ARM: CMD_ARM_OTA_END   (0x12)  传输结束，校验跳转
 *    ARM   -> ESP32: CMD_ARM_OTA_ACK (0x13)  确认收到
 *    ARM   -> ESP32: CMD_ARM_OTA_NACK(0x14)  请求重传
 *    ARM   -> ESP32: CMD_ARM_OTA_INFO(0x15)  上报固件信息
 *
 *  使用方法:
 *    1. arm_ota_init() 初始化
 *    2. arm_ota_set_callback() 设置回调（可选）
 *    3. arm_ota_start(firmware, length) 开始升级
 *    4. 在 loop() 中调用 arm_ota_process()
 *    5. 在 UART 帧回调中调用 arm_ota_handle_uart()
 * ================================================================
 */

#ifndef ARM_OTA_H
#define ARM_OTA_H

#include <stdint.h>
#include <stddef.h>

// 命令码在 config.h 中定义（CMD_ARM_OTA_START ~ CMD_ARM_OTA_INFO, 0x10~0x15）
// 如果 config.h 未包含，则在此定义备用
#ifndef CMD_ARM_OTA_START
#define CMD_ARM_OTA_START    0x10
#define CMD_ARM_OTA_DATA     0x11
#define CMD_ARM_OTA_END      0x12
#define CMD_ARM_OTA_ACK      0x13
#define CMD_ARM_OTA_NACK     0x14
#define CMD_ARM_OTA_INFO     0x15
#endif

// ==================== ARM OTA 参数 ====================
#define ARM_OTA_PACKET_SIZE    480   // 每包最大数据字节数（留出帧头开销）
#define ARM_OTA_SEND_INTERVAL  5     // 发送间隔 ms
#define ARM_OTA_ACK_TIMEOUT    2000  // 等待 ACK 超时 ms
#define ARM_OTA_MAX_RETRIES    3     // 最大重传次数
#define ARM_OTA_START_TIMEOUT  5000  // 等待 ARM 进入 IAP 模式超时 ms
#define ARM_OTA_END_TIMEOUT    5000  // 等待 ARM 校验完成超时 ms
#define ARM_OTA_MD5_SIZE       16    // MD5 摘要长度

// ==================== ARM OTA 状态 ====================
enum ArmOtaState {
  ARM_OTA_IDLE,
  ARM_OTA_WAIT_READY,     // 等待 ARM 进入 IAP 模式
  ARM_OTA_SENDING,        // 正在发送固件数据
  ARM_OTA_WAIT_ACK,       // 等待 ARM 确认
  ARM_OTA_VERIFYING,      // 等待 ARM 校验
  ARM_OTA_DONE,
  ARM_OTA_ERROR,
  ARM_OTA_TIMEOUT
};

// ==================== ARM OTA 结果回调 ====================
// state: 当前状态
// percent: 进度百分比 (0-100)
// message: 状态描述信息
typedef void (*ArmOtaCallback)(ArmOtaState state, int percent, const char* message);

// ==================== ARM 固件信息 ====================
struct ArmFirmwareInfo {
  char version[64];       // 固件版本字符串
  uint32_t fw_size;       // 固件大小
  uint8_t md5[ARM_OTA_MD5_SIZE];  // 固件 MD5
  bool valid;             // 信息是否有效
};

// ==================== 流式模式 API（用于 ESP8266 等 RAM 有限平台）====================
// 流式模式下，外部负责下载固件，每次调用 arm_ota_feed() 写入一块数据
// 内部自动分包发送，并在最后一包时发送 END 命令

// 开始流式 OTA（传入固件总大小，固件数据通过 arm_ota_feed() 逐块传入）
// 返回 true 表示已开始
bool arm_ota_start_streaming(size_t total_size);

// 写入固件数据块（流式模式专用）
// 返回 true 表示数据已被接受（会被立即或稍后发送）
// 返回 false 表示 OTA 未在流式模式或状态不对
bool arm_ota_feed(const uint8_t* data, size_t len);

// 结束流式 OTA（通知所有数据已传入，等待 ARM 校验）
// 返回 true 表示 END 命令已发送
bool arm_ota_end_streaming();

// 检查当前是否为流式模式
bool arm_ota_is_streaming_mode();

// ==================== API ====================

// 初始化 ARM OTA 模块
void arm_ota_init();

// 设置状态回调
void arm_ota_set_callback(ArmOtaCallback cb);

// 开始 ARM OTA（传入固件数据和长度）
// 返回 true 表示已开始
bool arm_ota_start(const uint8_t* firmware, size_t len);

// 处理循环（在 main loop 中调用，非阻塞）
void arm_ota_process();

// 获取当前状态
ArmOtaState arm_ota_get_state();

// 获取进度百分比 (0-100)
int arm_ota_get_progress();

// 获取状态描述信息
const char* arm_ota_get_message();

// 检查 ARM 是否处于 IAP 模式就绪状态
bool arm_ota_is_ready();

// 请求 ARM 上报固件版本信息
bool arm_ota_request_info();

// 获取最近一次查询到的 ARM 固件信息
const ArmFirmwareInfo* arm_ota_get_firmware_info();

// 取消 OTA
void arm_ota_cancel();

// 处理来自 ARM 的 UART 帧（在 UART 回调中调用）
// cmd: 命令码, data: 数据指针, len: 数据长度
void arm_ota_handle_uart(uint8_t cmd, const uint8_t* data, uint16_t len);

#endif // ARM_OTA_H
