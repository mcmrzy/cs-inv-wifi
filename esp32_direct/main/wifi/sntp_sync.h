/*
 * sntp_sync.h — SNTP 校时 + UTC 下发 ARM
 *
 * 流程: SNTP 同步成功后, 通过 WRITE_R_W_DATA 0x00CA 写入 4 字节 UTC 给 ARM,
 *       ARM 端检测差值 >4s 自动校时; 之后每天校时一次。
 */
#ifndef SNTP_SYNC_H
#define SNTP_SYNC_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 初始化 SNTP (由 app_main 在 WiFi 就绪后调用) */
void sntp_sync_init(void);

/* 主循环周期调用 (每 100ms): 检测同步完成 + 每天校时一次 */
void sntp_sync_tick(void);

/* SNTP 是否已同步 */
bool sntp_sync_is_synced(void);

#ifdef __cplusplus
}
#endif

#endif /* SNTP_SYNC_H */
