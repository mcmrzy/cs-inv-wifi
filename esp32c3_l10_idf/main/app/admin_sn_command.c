#include "admin_sn_command.h"
#include <string.h>

static void wipe(char *buffer, size_t size)
{
    volatile char *p = buffer;
    while (size--) *p++ = 0;
}

static char *token(char **cursor)
{
    char *p = *cursor;
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) { *cursor = p; return NULL; }
    char *start = p;
    while (*p && *p != ' ' && *p != '\t') p++;
    if (*p) *p++ = 0;
    *cursor = p;
    return start;
}

static admin_sn_result_t parse(char *line, const char *password, char sn[17])
{
    char *cursor = line;
    char *command = token(&cursor), *given = token(&cursor), *value = token(&cursor);
    if (!command || strcmp(command, "set_sn") || !given || !value || token(&cursor))
        return ADMIN_SN_INVALID_ARGS;
    if (!password || !password[0]) return ADMIN_SN_AUTH_FAILED;
    size_t expected_len = strlen(password), given_len = strlen(given);
    size_t diff = expected_len ^ given_len;
    for (size_t i = 0; i < expected_len; i++)
        diff |= (unsigned char)password[i] ^ (i < given_len ? (unsigned char)given[i] : 0u);
    if (diff) return ADMIN_SN_AUTH_FAILED;
    size_t n = strlen(value);
    if (!n || n > ADMIN_SN_LENGTH) return ADMIN_SN_INVALID_ARGS;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)value[i];
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')))
            return ADMIN_SN_INVALID_ARGS;
    }
    memcpy(sn, value, n + 1);
    return ADMIN_SN_VALID;
}

admin_sn_result_t admin_sn_input_byte(admin_sn_input_t *s, uint8_t b,
                                     const char *password, char sn[17])
{
    if (b == '\r' || b == '\n') {
        admin_sn_result_t result = s->discard;
        if (result == ADMIN_SN_NONE && s->used) result = parse(s->line, password, sn);
        wipe(s->line, sizeof(s->line));
        s->used = 0;
        s->discard = ADMIN_SN_NONE;
        return result;
    }
    if (s->discard != ADMIN_SN_NONE) return ADMIN_SN_NONE;
    if (b == '\b' || b == 0x7f) {
        if (s->used) s->line[--s->used] = 0;
        return ADMIN_SN_NONE;
    }
    if ((b < 0x20 && b != '\t') || b > 0x7e) {
        wipe(s->line, sizeof(s->line));
        s->used = 0;
        s->discard = ADMIN_SN_INVALID_ARGS;
    } else if (s->used + 1 >= sizeof(s->line)) {
        wipe(s->line, sizeof(s->line));
        s->used = 0;
        s->discard = ADMIN_SN_TOO_LONG;
    } else {
        s->line[s->used++] = (char)b;
        s->line[s->used] = 0;
    }
    return ADMIN_SN_NONE;
}
