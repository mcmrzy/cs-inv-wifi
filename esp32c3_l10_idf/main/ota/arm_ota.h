/**
 * ================================================================
 *  ARM OTA 模块 — Collector PULL 协议给 GD32F303 ARM 芯片做固件升级
 *
 *  流程与 ARM 端 (external/driver_arm 的 Collector.c/Update.c) 对齐:
 *    1. App 触发:   ESP 发 0x07/param1=0x0001/param2=4/"ARMU"
 *                   App 回 status 0 后延时复位进 bootloader
 *    2. 信息查询:   boot 发 0x09/param1=1 (空 payload, 3s 窗口)
 *                   ESP 回 0x09: data[0]=0 + transport size(u32LE)
 *                                + 链式 CRC32(u32LE, 对加密后 payload 链算)
 *    3. boot 擦除 APP 区后发 0x0A/param1=1 请求第 0 包 (每包窗口 5s)
 *       ESP 发 0x0B: data[0..1]=包号(LE) + FwKey TEA-CBC(零 IV) 加密 payload
 *                    第 0 包 payload = 16B 传输头 + 前 1024B 应用
 *    4. boot 每包回 0x0B ACK (data[0]=status); 最后一包 ACK 带整包校验结果
 *    5. 成功后 boot 自动转查 DSP: 0x09/param1=2 → ESP 回 data[0]=1 (无新固件)
 *
 *  使用方法:
 *    1. arm_ota_init() 初始化
 *    2. arm_ota_set_callback()/arm_ota_set_cancel_check() (可选)
 *    3. arm_ota_start_streaming(size) → arm_ota_feed() 写入 staged 数据
 *    4. arm_ota_end_streaming() 在 OTA 任务内同步完成整个 PULL 传输
 *    5. arm_ota_rx_byte() 由串口 RX 任务在本模块激活期间逐字节喂入
 * ================================================================
 */

#ifndef ARM_OTA_H
#define ARM_OTA_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define ARM_OTA_MAX_IMAGE_SIZE (256U * 1024U)

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

/* staged 数据写入 (HTTP 下载路径调用) */
bool arm_ota_start_streaming(size_t total_size);
bool arm_ota_feed(const uint8_t* data, size_t len);
/* 同步执行 Collector PULL 传输; 返回 false=失败(会话已释放回 IDLE, 原因见 get_last_error) */
bool arm_ota_end_streaming(void);
bool arm_ota_end_streaming_cancelable(const volatile bool* cancel_flag);
bool arm_ota_is_streaming_mode(void);

void arm_ota_init(void);
void arm_ota_set_callback(ArmOtaCallback cb);
void arm_ota_set_cancel_check(ArmOtaCancelCheck cb);
/* 兼容保留: PULL 流程在 end_streaming 内同步执行, 无需外部驱动 */
void arm_ota_process(void);
ArmOtaState arm_ota_get_state(void);
int arm_ota_get_progress(void);
const char* arm_ota_get_message(void);
/* 最近一次失败的具体原因 (暂存或传输阶段), 供上层填入上报 message; 空串=无 */
const char* arm_ota_get_last_error(void);
/* 传输成功(或会话结束)后回到 IDLE, 让串口 RX 路由恢复 CS 轮询 */
void arm_ota_reset(void);
/* 取消: 清理 staged 文件并回到 IDLE */
void arm_ota_cancel(void);
/* RX 喂入 (arm_poll 的 uart_rx_task 在 arm_ota 激活期间调用) */
void arm_ota_rx_byte(uint8_t byte);
void arm_ota_rx_reset(void);

#endif // ARM_OTA_H
