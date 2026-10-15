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
 *    4. 在 main loop 中调用 arm_ota_process()
 *    5. 在 UART 帧回调中调用 arm_ota_handle_uart()
 * ================================================================
 */

#ifndef ARM_OTA_H
#define ARM_OTA_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifndef CMD_ARM_OTA_START
#define CMD_ARM_OTA_START    0x10
#define CMD_ARM_OTA_DATA     0x11
#define CMD_ARM_OTA_END      0x12
#define CMD_ARM_OTA_ACK      0x13
#define CMD_ARM_OTA_NACK     0x14
#define CMD_ARM_OTA_INFO     0x15
#endif

#define ARM_OTA_PACKET_SIZE    480
#define ARM_OTA_SEND_INTERVAL  5
#define ARM_OTA_ACK_TIMEOUT    2000
#define ARM_OTA_MAX_RETRIES    3
#define ARM_OTA_START_TIMEOUT  5000
#define ARM_OTA_END_TIMEOUT    5000
#define ARM_OTA_MD5_SIZE       16

typedef enum {
    ARM_OTA_IDLE,
    ARM_OTA_WAIT_READY,
    ARM_OTA_SENDING,
    ARM_OTA_WAIT_ACK,
    ARM_OTA_VERIFYING,
    ARM_OTA_DONE,
    ARM_OTA_ERROR,
    ARM_OTA_TIMEOUT
} ArmOtaState;

typedef void (*ArmOtaCallback)(ArmOtaState state, int percent, const char* message);
typedef bool (*ArmOtaCancelCheck)(void);

typedef struct {
    char version[64];
    uint32_t fw_size;
    uint8_t md5[ARM_OTA_MD5_SIZE];
    bool valid;
} ArmFirmwareInfo;

bool arm_ota_start_streaming(size_t total_size);
bool arm_ota_set_expected_md5(const uint8_t md5[ARM_OTA_MD5_SIZE]);
bool arm_ota_feed(const uint8_t* data, size_t len);
bool arm_ota_end_streaming();
bool arm_ota_end_streaming_cancelable(const volatile bool* cancel_flag);
bool arm_ota_is_streaming_mode();

void arm_ota_init();
void arm_ota_set_callback(ArmOtaCallback cb);
void arm_ota_set_cancel_check(ArmOtaCancelCheck cb);
bool arm_ota_start(const uint8_t* firmware, size_t len);
void arm_ota_process();
ArmOtaState arm_ota_get_state();
int arm_ota_get_progress();
const char* arm_ota_get_message();
bool arm_ota_is_ready();
bool arm_ota_request_info();
const ArmFirmwareInfo* arm_ota_get_firmware_info();
void arm_ota_cancel();
void arm_ota_handle_uart(uint8_t cmd, const uint8_t* data, uint16_t len);

#endif // ARM_OTA_H
