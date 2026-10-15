/*
 * debug_log_ring.c — 调试日志环形缓冲 + JSON 行转义
 *
 * 缓冲内始终是完整的 "行\n行\n" 文本; 溢出丢最旧字节, 丢掉的量记在 dropped
 * (云端据此判断有丢失, 不必猜)。
 */
#include <string.h>
#include "debug_log_ring.h"

void debug_log_ring_init(debug_log_ring_t *r)
{
    if (!r) {
        return;
    }
    memset(r, 0, sizeof(*r));
}

size_t debug_log_ring_used(const debug_log_ring_t *r)
{
    return r ? r->used : 0;
}

uint32_t debug_log_ring_dropped(const debug_log_ring_t *r)
{
    return r ? r->dropped : 0;
}

void debug_log_ring_clear_dropped(debug_log_ring_t *r)
{
    if (r) {
        r->dropped = 0;
    }
}

void debug_log_ring_append(debug_log_ring_t *r, const char *line, size_t len)
{
    size_t i;

    if (!r || !line || len == 0) {
        return;
    }

    /* 腾出 len+1 字节空间: 从最旧一端丢 (单行超长时截断该行) */
    if (len + 1 > DEBUG_LOG_RING_SIZE) {
        line += len - (DEBUG_LOG_RING_SIZE - 1);
        len = DEBUG_LOG_RING_SIZE - 1;
    }
    while (r->used + len + 1 > DEBUG_LOG_RING_SIZE) {
        r->tail = (r->tail + 1) % DEBUG_LOG_RING_SIZE;
        r->used--;
        r->dropped++;
    }

    for (i = 0; i < len; i++) {
        char c = line[i];
        if (c == '\r' || c == '\n') {
            c = ' ';    /* 行内换行归一, 缓冲严格一行一条 */
        }
        r->buf[r->head] = c;
        r->head = (r->head + 1) % DEBUG_LOG_RING_SIZE;
        r->used++;
    }
    r->buf[r->head] = '\n';
    r->head = (r->head + 1) % DEBUG_LOG_RING_SIZE;
    r->used++;
}

size_t debug_log_ring_drain(debug_log_ring_t *r, char *out, size_t max_len)
{
    size_t n, cut = 0, i;

    if (!r || !out || max_len == 0 || r->used == 0) {
        return 0;
    }

    n = (r->used < max_len) ? r->used : max_len;
    for (i = 0; i < n; i++) {
        char c = r->buf[(r->tail + i) % DEBUG_LOG_RING_SIZE];
        out[i] = c;
        if (c == '\n') {
            cut = i + 1;    /* 最后一个完整行边界 */
        }
    }
    if (cut > 0) {
        n = cut;            /* 不把半行发出去 */
    }

    r->tail = (r->tail + n) % DEBUG_LOG_RING_SIZE;
    r->used -= n;
    return n;
}

/* 去掉结尾不完整的 UTF-8 序列 (截断日志行可能落在多字节字符中间),
 * 返回可安全输出的字节数 */
static size_t utf8_safe_len(const char *s, size_t len)
{
    size_t i = len;
    size_t cont = 0;
    size_t need;

    while (i > 0 && cont < 3) {
        unsigned char c = (unsigned char)s[--i];
        if ((c & 0xC0) == 0x80) {
            cont++;                     /* 续字节: 继续往前找首字节 */
            continue;
        }
        if (c < 0x80) {
            return len;                 /* ASCII 收尾: 完整 */
        }
        need = (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : 2;
        return (cont + 1 >= need) ? len : i;    /* 续字节不够 → 截到首字节前 */
    }
    return len;
}

size_t debug_log_json_lines(const char *text, size_t len, char *out,
                            size_t out_len, int *out_lines)
{
    size_t i = 0, w = 0;
    int lines = 0;

    if (out_lines) {
        *out_lines = 0;
    }
    if (!text || !out || out_len == 0) {
        return 0;
    }

    while (i < len) {
        size_t start = i, llen, safe, k;

        while (i < len && text[i] != '\n') {
            i++;
        }
        llen = i - start;
        if (i < len) {
            i++;                        /* 跳过换行 */
        }
        if (llen == 0) {
            continue;                   /* 空行不上行 */
        }
        safe = utf8_safe_len(text + start, llen);
        if (safe == 0) {
            continue;
        }
        if (w + safe * 2 + 3 > out_len) {
            break;                      /* 放不下: 整行留给下一批 */
        }

        if (lines) {
            out[w++] = ',';
        }
        out[w++] = '"';
        for (k = 0; k < safe; k++) {
            unsigned char c = (unsigned char)text[start + k];
            if (c == '"' || c == '\\') {
                out[w++] = '\\';        /* JSON 里不能裸传引号/反斜杠 */
                out[w++] = (char)c;
            } else if (c < 0x20) {
                out[w++] = ' ';         /* 其余控制字符转空格, 保持单行 */
            } else {
                out[w++] = (char)c;     /* ASCII 与 UTF-8 原样保留 */
            }
        }
        out[w++] = '"';
        lines++;
    }

    if (out_lines) {
        *out_lines = lines;
    }
    return w;
}
