/*
 * debug_log.c — 调试模式串口日志上云
 *
 * 串口日志经 esp_log_set_vprintf 钩子进环形缓冲 (始终捕获, 溢出丢最旧),
 * 仅在调试模式生效 (telemetry_debug_active) 且 MQTT 在线时按批上行到
 * cs_inv/{sn}/debug/log。
 *
 * 设计要点:
 *  - 钩子里只做一次有界 vsnprintf + memcpy (自旋锁保护), 网络 IO 全部放到
 *    主循环 debug_log_tick(): 日志可能来自任意任务/上下文, 不能在那里发布。
 *  - 发布期间置位 s_publishing, 发布路径自身产生的日志不再入缓冲, 防回环。
 *  - 上行 payload: {"lines":["行1","行2"],"n":2[,"dropped":N]}
 *    (信封 {"t","v":2,"data":...} 由 mqtt_publish 统一拼接)
 */
#include <stdarg.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cs_protocol_defs.h"
#include "mqtt_handler.h"
#include "telemetry.h"
#include "debug_log.h"
#include "debug_log_ring.h"

/* 单行上限 (超长截断; ESP-IDF 默认日志行远短于此) */
#define DEBUG_LOG_LINE_MAX   256
/* 单批上行字节数 (整行边界截断) */
#define DEBUG_LOG_CHUNK      512
/* 不满一批时最长攒批时间 */
#define DEBUG_LOG_FLUSH_MS   1000

static debug_log_ring_t s_ring;
static portMUX_TYPE     s_lock = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t   s_prev_vprintf;
static volatile bool    s_publishing;   /* 发布路径内不再捕获 */
static uint32_t         s_last_flush_ms;
static char             s_chunk[DEBUG_LOG_CHUNK];
static char             s_payload[DEBUG_LOG_CHUNK * 2 + 128];

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* ---- 串口日志钩子: 旁路复制一行, 原样继续输出 ---- */
static int debug_log_vprintf(const char *fmt, va_list ap)
{
    char line[DEBUG_LOG_LINE_MAX];
    va_list ap_copy;
    int n;

    va_copy(ap_copy, ap);
    n = vsnprintf(line, sizeof(line), fmt, ap_copy);
    va_end(ap_copy);

    if (n > 0 && !s_publishing) {
        size_t len = ((size_t)n < sizeof(line)) ? (size_t)n : sizeof(line) - 1;
        portENTER_CRITICAL(&s_lock);
        debug_log_ring_append(&s_ring, line, len);
        portEXIT_CRITICAL(&s_lock);
    }

    if (s_prev_vprintf) {
        return s_prev_vprintf(fmt, ap);     /* 交给原处理器 (IDF 默认: 控制台) */
    }
    /* 兜底: 无前序处理器时输出已格式化文本 (字面量格式串, 不转发 fmt) */
    if (n > 0) {
        return printf("%s", line);
    }
    return 0;
}

void debug_log_init(void)
{
    debug_log_ring_init(&s_ring);
    s_last_flush_ms = 0;
    s_publishing = false;
    s_prev_vprintf = esp_log_set_vprintf(debug_log_vprintf);
}

void debug_log_tick(void)
{
    uint32_t now, dropped;
    size_t   pending, n, w, hdr;
    int      lines = 0;

    /* 仅调试模式 + 已连接时上行; 关闭时缓冲继续滚动 (开启后可回看一小段历史) */
    if (!telemetry_debug_active() || !mqtt_is_connected()) {
        return;
    }

    portENTER_CRITICAL(&s_lock);
    pending = debug_log_ring_used(&s_ring);
    portEXIT_CRITICAL(&s_lock);
    if (pending == 0) {
        return;
    }

    now = now_ms();
    if (pending < sizeof(s_chunk) &&
        (now - s_last_flush_ms) < DEBUG_LOG_FLUSH_MS) {
        return;             /* 攒批: 满一批立即发, 否则最多每秒一批 */
    }
    s_last_flush_ms = now;

    portENTER_CRITICAL(&s_lock);
    n = debug_log_ring_drain(&s_ring, s_chunk, sizeof(s_chunk));
    dropped = debug_log_ring_dropped(&s_ring);
    if (dropped) {
        debug_log_ring_clear_dropped(&s_ring);
    }
    portEXIT_CRITICAL(&s_lock);
    if (n == 0) {
        return;
    }

    hdr = (size_t)snprintf(s_payload, sizeof(s_payload), "{\"lines\":[");
    if (hdr >= sizeof(s_payload) - 1) {
        return;
    }
    w = debug_log_json_lines(s_chunk, n, s_payload + hdr,
                             sizeof(s_payload) - hdr - 48, &lines);
    if (lines == 0) {
        return;             /* 缓冲异常: 本批放弃 (下批重来) */
    }
    w += hdr;
    /* 尾部字段逐段追加, 任一段放不下就放弃本批 (不做静默截断) */
    {
        int r;
        if (w >= sizeof(s_payload) - 1) {
            return;
        }
        r = snprintf(s_payload + w, sizeof(s_payload) - w, "],\"n\":%d", lines);
        if (r < 0 || (size_t)r >= sizeof(s_payload) - w) {
            return;
        }
        w += (size_t)r;
    }
    if (dropped) {
        int r;
        if (w >= sizeof(s_payload) - 1) {
            return;
        }
        r = snprintf(s_payload + w, sizeof(s_payload) - w, ",\"dropped\":%lu",
                     (unsigned long)dropped);
        if (r < 0 || (size_t)r >= sizeof(s_payload) - w) {
            return;
        }
        w += (size_t)r;
    }
    s_payload[w++] = '}';
    s_payload[w] = '\0';

    s_publishing = true;
    mqtt_publish_volatile(PROTO_TOPIC_DEBUG_LOG, s_payload, (int)w, 0);
    s_publishing = false;
}
