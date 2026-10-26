#include "admin_sn_command.h"
#include <string.h>

static void wipe(void *buffer, size_t size)
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

static int unhex(char c){return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1;}
static admin_sn_result_t parse(char *line, bool (*authenticate)(const char *), char sn[17], char pin[7], uint8_t bundle[64])
{
    if (!strcmp(line, "status")) return ADMIN_SN_STATUS;
    if (!strcmp(line, "dsp_params")) return ADMIN_SN_DSP_STATUS;
    /* Explicit authenticated control; never echo/log the supplied password. */
    if(!strncmp(line,"dsp_boot ",9) || !strncmp(line,"dsp_run ",8) || !strncmp(line,"dsp_reboot ",11)){
        char *control_cursor=line;
        char *control=token(&control_cursor),*secret=token(&control_cursor);
        if(!secret || token(&control_cursor))return ADMIN_SN_INVALID_ARGS;
        if(!authenticate || !authenticate(secret))return ADMIN_SN_AUTH_FAILED;
        return !strcmp(control,"dsp_boot")?ADMIN_SN_DSP_BOOT:
            !strcmp(control,"dsp_run")?ADMIN_SN_DSP_RUN:ADMIN_SN_DSP_REBOOT;
    }
    char *cursor = line;
    char *command = token(&cursor), *given = token(&cursor), *value = token(&cursor);
    bool set_pin = command && !strcmp(command,"set_pin");
    bool set_dsp = command && !strcmp(command,"set_dsp");
    char *pin_value = (set_pin || set_dsp) ? token(&cursor) : NULL;
    if (!command || (strcmp(command, "set_sn") && !set_pin && !set_dsp) || !given || !value || token(&cursor))
        return ADMIN_SN_INVALID_ARGS;
    if (!authenticate || !authenticate(given)) return ADMIN_SN_AUTH_FAILED;
    size_t n = strlen(value);
    if (!n || n > ADMIN_SN_LENGTH) return ADMIN_SN_INVALID_ARGS;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)value[i];
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')))
            return ADMIN_SN_INVALID_ARGS;
    }
    memcpy(sn, value, n + 1);
    if(set_dsp){
        if(n!=16 || !pin_value || strlen(pin_value)!=128)return ADMIN_SN_INVALID_ARGS;
        for(unsigned i=0;i<64;i++){
            int h=unhex(pin_value[2*i]),l=unhex(pin_value[2*i+1]);
            if(h<0 || l<0)return ADMIN_SN_INVALID_ARGS;
            bundle[i]=(uint8_t)((h<<4)|l);
        }
        return ADMIN_SN_DSP_VALID;
    }
    if(set_pin){
        if(!pin_value || strlen(pin_value)!=6)return ADMIN_SN_INVALID_ARGS;
        for(unsigned i=0;i<6;i++)if(pin_value[i]<'0'||pin_value[i]>'9')return ADMIN_SN_INVALID_ARGS;
        memcpy(pin,pin_value,7);return ADMIN_SN_PIN_VALID;
    }
    return ADMIN_SN_VALID;
}

admin_sn_result_t admin_sn_input_byte(admin_sn_input_t *s, uint8_t b,
                                     bool (*authenticate)(const char *), char sn[17])
{
    if (b == '\r' || b == '\n') {
        admin_sn_result_t result = s->discard;
        wipe(s->pin,sizeof(s->pin));
        wipe(s->dsp_bundle,sizeof(s->dsp_bundle));
        if (result == ADMIN_SN_NONE && s->used) result = parse(s->line, authenticate, sn, s->pin,s->dsp_bundle);
        if(result!=ADMIN_SN_DSP_VALID)wipe(s->dsp_bundle,sizeof(s->dsp_bundle));
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
