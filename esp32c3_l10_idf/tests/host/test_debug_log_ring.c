/*
 * test_debug_log_ring.c — 调试日志缓冲/转义宿主回归
 *
 * 覆盖: 按行往返、行内换行归一、drain 不拆行、溢出丢最旧并计数、
 * JSON 转义(引号/反斜杠/控制字符)、UTF-8 截断保护、放不下时整行推迟。
 */
#include <stdio.h>
#include <string.h>
#include "debug_log_ring.h"

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

static debug_log_ring_t ring;
static char out[DEBUG_LOG_RING_SIZE * 2];

static void reset(void)
{
    debug_log_ring_init(&ring);
    memset(out, 0, sizeof(out));
}

int main(void)
{
    /* 1. 追加/取出往返: 行尾自动补 '\n' */
    reset();
    debug_log_ring_append(&ring, "I (123) ARMPOLL: TX cmd=0x03", 28);
    CHECK(debug_log_ring_used(&ring) == 29);
    CHECK(debug_log_ring_drain(&ring, out, sizeof(out)) == 29);
    CHECK(memcmp(out, "I (123) ARMPOLL: TX cmd=0x03\n", 29) == 0);
    CHECK(debug_log_ring_used(&ring) == 0);
    CHECK(debug_log_ring_drain(&ring, out, sizeof(out)) == 0);

    /* 2. 行内 \r\n 归一为空格 (缓冲严格一行一条) */
    reset();
    debug_log_ring_append(&ring, "a\r\nb", 4);
    debug_log_ring_drain(&ring, out, sizeof(out));
    CHECK(strcmp(out, "a  b\n") == 0);

    /* 3. drain 不拆行: 窗口落在行中间时只取到最后一个完整行 */
    reset();
    debug_log_ring_append(&ring, "line-one", 8);    /* 9 字节 */
    debug_log_ring_append(&ring, "line-two", 8);    /* 9 字节 */
    CHECK(debug_log_ring_drain(&ring, out, 10) == 9);
    CHECK(memcmp(out, "line-one\n", 9) == 0);
    CHECK(debug_log_ring_used(&ring) == 9);
    CHECK(debug_log_ring_drain(&ring, out, sizeof(out)) == 9);
    CHECK(memcmp(out, "line-two\n", 9) == 0);

    /* 4. 溢出丢最旧并累计 dropped */
    reset();
    for (int i = 0; i < 300; i++) {                 /* 300×11B > 2048B */
        debug_log_ring_append(&ring, "0123456789", 10);
    }
    CHECK(debug_log_ring_used(&ring) <= DEBUG_LOG_RING_SIZE);
    CHECK(debug_log_ring_dropped(&ring) > 0);
    {
        size_t n = debug_log_ring_drain(&ring, out, sizeof(out));
        CHECK(n > 0 && out[n - 1] == '\n');
        CHECK(memcmp(out + n - 11, "0123456789\n", 11) == 0);   /* 最新一行保留 */
    }
    CHECK(debug_log_ring_dropped(&ring) > 0);       /* 取走后 dropped 仍可读 */
    debug_log_ring_clear_dropped(&ring);
    CHECK(debug_log_ring_dropped(&ring) == 0);

    /* 5. JSON 转义: 引号、反斜杠转义, 控制字符转空格 */
    reset();
    {
        static const char raw[] = {'a', '"', 'b', '\\', 'c', '\n',
                                   'd', 0x01, 'e', '\n'};
        const char *want = "\"a\\\"b\\\\c\",\"d e\"";
        int lines = -1;
        size_t w = debug_log_json_lines(raw, sizeof(raw), out, sizeof(out), &lines);
        CHECK(lines == 2);
        CHECK(w == strlen(want));
        CHECK(strcmp(out, want) == 0);
    }

    /* 6. UTF-8: 完整汉字原样保留; 结尾被截断的多字节序列整段丢弃 */
    reset();
    {
        static const char ok[] = {(char)0xE4, (char)0xB8, (char)0xAD, '\n',
                                  'A', '\n'};                   /* "中" + A */
        static const char cut[] = {(char)0xE4, (char)0xB8, '\n'}; /* "中" 少了末字节 */
        int lines = -1;
        size_t w = debug_log_json_lines(ok, sizeof(ok), out, sizeof(out), &lines);
        CHECK(lines == 2);
        CHECK(w == strlen("\"\xE4\xB8\xAD\",\"A\""));
        CHECK(strcmp(out, "\"\xE4\xB8\xAD\",\"A\"") == 0);

        memset(out, 0, sizeof(out));
        w = debug_log_json_lines(cut, sizeof(cut), out, sizeof(out), &lines);
        CHECK(lines == 0);
        CHECK(w == 0);
    }

    /* 7. 放不下时整行推迟 (不半行输出), 空行跳过, 末尾无换行也算一行 */
    reset();
    {
        int lines = -1;
        size_t w = debug_log_json_lines("abcdefghij\n", 11, out, 8, &lines);
        CHECK(lines == 0);
        CHECK(w == 0);

        w = debug_log_json_lines("\n\n", 2, out, sizeof(out), &lines);
        CHECK(lines == 0);
        CHECK(w == 0);

        w = debug_log_json_lines("tail", 4, out, sizeof(out), &lines);
        CHECK(lines == 1);
        CHECK(w == strlen("\"tail\""));
        CHECK(strcmp(out, "\"tail\"") == 0);
    }

    printf("debug_log_ring: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
