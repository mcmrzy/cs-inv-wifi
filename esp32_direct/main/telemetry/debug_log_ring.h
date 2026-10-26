/*
 * debug_log_ring.h — 调试日志环形缓冲 + JSON 行转义 (纯 C, 不依赖 ESP-IDF)
 *
 * 供 debug_log.c (串口日志上云) 使用; 单独拆出便于宿主回归
 * (tests/host/test_debug_log_ring.c)。
 */
#ifndef DEBUG_LOG_RING_H
#define DEBUG_LOG_RING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 缓冲容量: 环形, 写满丢最旧字节 (dropped 累计丢弃量, 上行时一并告知云端) */
#define DEBUG_LOG_RING_SIZE 2048

typedef struct {
    char     buf[DEBUG_LOG_RING_SIZE];
    size_t   head;      /* 写入位置 */
    size_t   tail;      /* 读出位置 */
    size_t   used;      /* 有效字节数 */
    uint32_t dropped;   /* 溢出丢弃字节数 (累计, 取走后清零) */
} debug_log_ring_t;

void debug_log_ring_init(debug_log_ring_t *r);

/* 追加一行 (自动补 '\n'; 行内 \r\n 归一为空格, 保证缓冲严格按行组织) */
void debug_log_ring_append(debug_log_ring_t *r, const char *line, size_t len);

size_t   debug_log_ring_used(const debug_log_ring_t *r);
uint32_t debug_log_ring_dropped(const debug_log_ring_t *r);
void     debug_log_ring_clear_dropped(debug_log_ring_t *r);

/* 取出最多 max_len 字节 (按行边界截断, 不拆行); 返回实际取出字节数 */
size_t debug_log_ring_drain(debug_log_ring_t *r, char *out, size_t max_len);

/* 把 text (换行分隔) 转成 JSON 数组元素串, 形如 "a","b"。
 * 转义 " 与 \ ; 其余控制字符转空格; 去掉结尾不完整的 UTF-8 序列。
 * 返回写入 out 的字节数 (0=一条都放不下), 行数写入 *out_lines。 */
size_t debug_log_json_lines(const char *text, size_t len, char *out,
                            size_t out_len, int *out_lines);

#endif /* DEBUG_LOG_RING_H */
