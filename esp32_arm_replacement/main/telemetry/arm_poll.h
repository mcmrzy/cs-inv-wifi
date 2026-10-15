/*
 * arm_poll.h — 旧 L10 调用方的过渡接口
 *
 * direct_poll.c 通过 DSP/BMS 直连填充运行参数。用户设置保存在ESP的NVS，
 * 云端、BLE及按键统一排队保存，成功后周期发送00/01/02给DSP。
 * 写回调成功表示本地已保存且UART已提交三包，不表示DSP应用已确认。
 * ARM 专有信息、未映射本地参数和旧 ARM OTA 不伪造 DSP 响应。
 */
#ifndef ARM_POLL_H
#define ARM_POLL_H

#include <stdint.h>
#include <stdbool.h>
#include "arm_param.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 最近一次运行参数 (解析后, 主循环共享) */
extern RunParamDef g_run_param;

/* 最近一次只读信息 */
extern OnlyReadDataDef g_only_read;

/* 只读信息 SN (Inverter_SN1..4, 4×u32, 16B ASCII 自然序) → 字符串; 非法返回 false */
bool onlyread_sn_to_str(const OnlyReadDataDef *o, char *out, size_t len);

/* ARM 只读信息 SN 与 NVS 不一致 (待重启生效) */
bool arm_poll_sn_changed(void);

/* 最近一次 可读写数据 (含 UTC 回读) */
extern ReadWiteDataDef g_read_write;

/* 最近一次控制参数 (READ_CTRLPARAM 0x01, 42 个 u16, 索引 0x0000-0x0029) */
extern uint16_t g_ctrl_param[CTRL_PARAM_COUNT];

/* 兼容旧遥测代码的直连 DSP 状态 */
typedef struct {
    bool    connected;      /* 最近 120s 内有有效应答 */
    uint32_t last_rx_ms;    /* 最近一次收到有效帧的时刻 */
    uint32_t poll_count;    /* 成功轮询次数 */
    uint32_t err_count;     /* 连续失败次数 */
    uint32_t ctrl_err;      /* 写命令 err!=0 次数 */
    uint32_t last_info_ms;  /* 最近一次成功读 info 的时刻 */
    uint32_t last_ctrl_ms;  /* 最近一次成功读控制参数的时刻 */
} arm_link_stat_t;

extern arm_link_stat_t g_arm_stat;

/* 初始化: 清零状态 (由 app_main 调用) */
void arm_poll_init(void);

/* 主循环周期调用 (每 100ms)，更新直连链路超时状态 */
void arm_poll_tick(void);
/* OTA 静默窗口：true 期间停发 DSP/BMS 周期轮询 */
void arm_poll_set_quiet(bool quiet);

/* Compatibility hook: supplied DSP protocol has no identity query. ESP info
 * is published directly by telemetry, with unavailable DSP fields as null. */
void arm_poll_request_info(void);

/* 发布当前已确认的控制参数快照；DSP 25 同步为异步推送。 */
void arm_poll_request_config(void);

/*
 * 写命令执行 (供 cmd_handler 调用):
 *  cmd       CS_CMD_WRITE_CTRLPARAM / CS_CMD_WRITE_R_W_DATA
 *  param1    起始地址
 *  param2    数据长度
 *  data      数据
 * 返回0表示排队成功；仅已知、可映射、在线且空闲时接受。
 * 只支持单个u16控制参数，结果由写完成回调报告。
 */
int arm_poll_send_write(uint8_t cmd, uint16_t param1, uint16_t param2,
                        const uint8_t *data);

/* 当前是否有写命令在等待应答 */
bool arm_poll_write_pending(void);

/* 标记 UTC 已成功写入 (供 sntp_sync 更新最后校时时间) */
void arm_poll_mark_utc_done(void);

/* 获取最近校时时间戳 (由 sntp_sync 记录) */
uint32_t arm_poll_last_utc_ms(void);

/* 参数共享锁: 读取 g_run_param/g_only_read/g_read_write 前 take, 读完 give */
bool arm_poll_take_param(void);
void arm_poll_give_param(void);

/*
 * 写命令完成回调 (任意上下文调用):
 *  cmd/param1/param2  写命令信息
 *  err                0=ARM 应答成功; 非 0=ARM 应答 err 值; -1=应答超时
 */
typedef void (*arm_write_result_cb_t)(uint8_t cmd, uint16_t param1,
                                      uint16_t param2, int err);
void arm_poll_set_write_cb(arm_write_result_cb_t cb);

/* 控制参数快照更新回调；values 仅在回调期间有效。 */
typedef void (*arm_ctrl_snapshot_cb_t)(const uint16_t *values, size_t count,
                                       uint32_t parameter_revision);
void arm_poll_set_ctrl_snapshot_cb(arm_ctrl_snapshot_cb_t cb);

#ifdef __cplusplus
}
#endif

#endif /* ARM_POLL_H */
