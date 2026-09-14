/*
 * sntp_sync.c — SNTP 校时 + UTC 下发 ARM
 *
 * 校时协议: WRITE_R_W_DATA, param1=0x00CA, param2=4, data=le32(UTC秒)
 * (ARM 端 INDEX_UTC_L/INDEX_UTC_H 在 0x00CA/0x00CB, 差值 >4s 自动校时)
 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_sntp.h"
#include "config.h"
#include "cs_protocol.h"
#include "arm_param.h"
#include "arm_poll.h"
#include "sntp_sync.h"

static const char *TAG = "SNTP";

#define UTC_WRITE_INTERVAL_MS  (24UL * 3600UL * 1000UL)  /* 每天一次 */
#define UTC_WRITE_RETRY_MS     (5UL * 60UL * 1000UL)    /* 失败后 5 分钟重试 */

static bool     s_synced;            /* SNTP 已同步 */
static bool     s_utc_written;       /* UTC 曾成功写入 ARM */
static uint32_t s_last_utc_write_ms; /* 最近一次成功写入时刻 */
static uint32_t s_last_attempt_ms;   /* 最近一次发起写入时刻 (防重试风暴) */
static uint32_t s_now_ms;
static uint32_t s_fail_count;        /* 连续失败次数 (用于节流日志) */

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* ARM 写应答回调: 确认 UTC 写入结果 */
static void on_write_done(uint8_t cmd, uint16_t param1, uint16_t param2, int err)
{
    (void)param2;
    if (cmd == CS_CMD_WRITE_R_W_DATA && param1 == INDEX_UTC_L) {
        if (err == 0) {
            s_utc_written = true;
            s_last_utc_write_ms = s_now_ms;
            s_fail_count = 0;
            arm_poll_mark_utc_done();
            ESP_LOGI(TAG, "UTC written to ARM ok");
        } else {
            s_fail_count++;
            /* 首次失败 + 每 10 次打一次 WARN, 其余静默 */
            if (s_fail_count <= 1 || s_fail_count % 10 == 0) {
                ESP_LOGW(TAG, "UTC write failed err=%d (fail#%lu, retry in 5min)",
                         err, (unsigned long)s_fail_count);
            }
        }
    }
}

void sntp_sync_init(void)
{
    s_synced = false;
    s_utc_written = false;
    s_last_utc_write_ms = 0;
    s_last_attempt_ms = 0;
    s_now_ms = 0;
    s_fail_count = 0;

    arm_poll_set_write_cb(on_write_done);

    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_setservername(1, "pool.ntp.org");
    esp_sntp_init();
    setenv("TZ", "CST-8", 1);
    tzset();
    ESP_LOGI(TAG, "SNTP initialized");
}

bool sntp_sync_is_synced(void)
{
    return s_synced;
}

void sntp_sync_tick(void)
{
    s_now_ms = now_ms();

    /* 1. 检测 SNTP 同步完成 */
    if (!s_synced) {
        if (esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
            s_synced = true;
            ESP_LOGI(TAG, "SNTP synced, epoch=%ld", (long)time(NULL));
        }
    }

    /* 2. UTC 下发调度: 首次同步后立即写, 之后每 24h 写一次 */
    if (!s_synced) {
        return;
    }
    if (s_utc_written &&
        (s_now_ms - s_last_utc_write_ms) < UTC_WRITE_INTERVAL_MS) {
        return;
    }
    /* 失败重试节流 */
    if (s_last_attempt_ms && (s_now_ms - s_last_attempt_ms) < UTC_WRITE_RETRY_MS) {
        return;
    }
    /* 链路忙则等待 */
    if (arm_poll_write_pending()) {
        return;
    }

    uint32_t utc = (uint32_t)time(NULL);
    if (utc < 1000000000) {
        return;    /* 时钟未就绪 */
    }
    uint8_t data[4];
    data[0] = (uint8_t)(utc & 0xFF);
    data[1] = (uint8_t)((utc >> 8) & 0xFF);
    data[2] = (uint8_t)((utc >> 16) & 0xFF);
    data[3] = (uint8_t)((utc >> 24) & 0xFF);

    s_last_attempt_ms = s_now_ms;
    if (arm_poll_send_write(CS_CMD_WRITE_R_W_DATA, INDEX_UTC_L, 4, data) == 0) {
        /* 首次或成功后首次打印, 重试期间静默 */
        if (s_fail_count == 0) {
            ESP_LOGI(TAG, "UTC %lu queued to ARM", (unsigned long)utc);
        }
    }
}
