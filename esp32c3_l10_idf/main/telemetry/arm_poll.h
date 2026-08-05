/*
 * arm_poll.h — ARM 串口轮询调度
 *
 *  - 5s 轮询运行参数 (READ_RUNPARAM 0x03E8, 176B)
 *  - 上电读取一次只读信息 (READ_R_DATA 0x0190, 44B)
 *  - SNTP 校时后写入 UTC (WRITE_R_W_DATA 0x00CA, 4B), 每天一次
 *  - 空闲期可插入云端命令 (见 cs_cmd_poll_try_send)
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

/* 最近一次 可读写数据 (含 UTC 回读) */
extern ReadWiteDataDef g_read_write;

/* 与 ARM 通信状态 */
typedef struct {
    bool    connected;      /* 最近 120s 内有有效应答 */
    uint32_t last_rx_ms;    /* 最近一次收到有效帧的时刻 */
    uint32_t poll_count;    /* 成功轮询次数 */
    uint32_t err_count;     /* 连续失败次数 */
    uint32_t ctrl_err;      /* 写命令 err!=0 次数 */
    uint32_t last_info_ms;  /* 最近一次成功读 info 的时刻 */
} arm_link_stat_t;

extern arm_link_stat_t g_arm_stat;

/* 初始化: 清零状态 (由 app_main 调用) */
void arm_poll_init(void);

/* 主循环周期调用 (每 100ms), 内部调度 5s 轮询/校时/超时判定 */
void arm_poll_tick(void);

/* 立即请求重读 info (如 MQTT 连接建立后) */
void arm_poll_request_info(void);

/*
 * 写命令执行 (供 cmd_handler 调用):
 *  cmd       CS_CMD_WRITE_CTRLPARAM / CS_CMD_WRITE_R_W_DATA
 *  param1    起始地址
 *  param2    数据长度
 *  data      数据
 * 返回 0=已排队发送, -1=忙
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

#ifdef __cplusplus
}
#endif

#endif /* ARM_POLL_H */
