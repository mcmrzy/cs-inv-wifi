/**
 * ================================================================
 *  Collector IAP OTA 模块 — GD32F303 ARM / DSP 固件升级
 *
 *  流程与 ARM 端 (external/driver_arm 的 Collector.c/Update.c) 对齐:
 *    1. App 触发:   ESP 发 0x07/param1=0x0001/param2=4/"ARMU"
 *                   App 回 status 0 后延时复位进 bootloader
 *    2. 信息查询:   boot 发 0x09/param1=1(ARM) 或 2(DSP)
 *                   ESP 回 0x09: data[0]=0 + transport size(u32LE)
 *                                + 链式 CRC32(u32LE, 对加密后 payload 链算)
 *                  或 data[0]=1 (无新固件)
 *    3. 擦除后发 0x0A 请求第 0 包; ESP 发 0x0B: 包号(LE) + TEA 加密 payload
 *       第 0 包 payload = 16B 传输头 + 前 1024B 应用
 *    4. 每包回 0x0B ACK; 最后一包 ACK status=0 表示整包 CRC 通过
 *
 *  DSP-only: 仍触发 ARMU 进 IAP; 对 0x09/p1=1 回"无", 对 0x09/p1=2 回有货。
 *  ARM-only: 对 0x09/p1=2 回"无", 让 boot 退出升级链。
 *
 *  使用方法:
 *    1. arm_ota_init() 初始化
 *    2. arm_ota_set_callback()/arm_ota_set_cancel_check() (可选)
 *    3. arm_ota_start_streaming_for(target, size) → arm_ota_feed() 写入
 *    4. arm_ota_end_streaming_for(target) 在 OTA 任务内同步完成 PULL 传输
 *    5. arm_ota_rx_byte() 由串口 RX 任务在本模块激活期间逐字节喂入
 * ================================================================
 */

#ifndef ARM_OTA_H
#define ARM_OTA_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define ARM_OTA_MAX_IMAGE_SIZE (512U * 1024U)

/* Collector IAP 参数1: 1=ARM 镜像, 2=DSP 镜像, 3=BMS 镜像
 * (与 Update.c FirmwareInfoHandler 一致)。BMS 走同一 PULL 协议,
 * 由 ARM boot 中转: 握手结果经 0x0A 通知, 每块结果经 0x0B 回传,
 * 块失败(status=1)时 boot 不递增包号, 本侧重发同一包。 */
typedef enum {
    IAP_TARGET_ARM = 1,
    IAP_TARGET_DSP = 2,
    IAP_TARGET_BMS = 3,
} IapTarget;

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

/* staged 数据写入 (HTTP 下载路径调用)。_for 指定 IAP 目标并选择对应暂存路径 */
bool arm_ota_start_streaming_for(IapTarget target, size_t total_size);
bool arm_ota_start_streaming(size_t total_size); /* 默认 ARM, 兼容旧调用 */
bool arm_ota_feed(const uint8_t* data, size_t len);
/* 同步执行 Collector PULL 传输; 返回 false=失败(会话已释放回 IDLE) */
bool arm_ota_end_streaming_for(IapTarget target, const volatile bool* cancel_flag);
bool arm_ota_end_streaming(void);
bool arm_ota_end_streaming_cancelable(const volatile bool* cancel_flag);
bool arm_ota_is_streaming_mode(void);
IapTarget arm_ota_get_iap_target(void);

void arm_ota_init(void);
void arm_ota_set_callback(ArmOtaCallback cb);
void arm_ota_set_cancel_check(ArmOtaCancelCheck cb);
void arm_ota_process(void);
ArmOtaState arm_ota_get_state(void);
int arm_ota_get_progress(void);
const char* arm_ota_get_message(void);
const char* arm_ota_get_last_error(void);
bool arm_ota_last_error_is_storage(void);
void arm_ota_reset(void);
void arm_ota_cancel(void);
void arm_ota_rx_byte(uint8_t byte);
void arm_ota_rx_reset(void);
/* 归还 ARM IAP 工作区 (~20KB 堆)。只允许在没有 ARM 会话时调用。
 * ota_service 会在 ARM/DSP/BMS 会话统一出口释放；ESP 目标下载开始前也会释放，
 * 确保大块工作区不跨 OTA 会话常驻。 */
void arm_ota_workspace_release(void);

#endif // ARM_OTA_H
