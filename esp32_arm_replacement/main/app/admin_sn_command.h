#ifndef ADMIN_SN_COMMAND_H
#define ADMIN_SN_COMMAND_H
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#define ADMIN_SN_LENGTH 16
typedef enum {
    ADMIN_SN_NONE, ADMIN_SN_VALID, ADMIN_SN_STATUS, ADMIN_SN_PIN_VALID, ADMIN_SN_AUTH_FAILED,
    ADMIN_SN_INVALID_ARGS, ADMIN_SN_TOO_LONG, ADMIN_SN_DSP_VALID, ADMIN_SN_DSP_STATUS,
    ADMIN_SN_DSP_BOOT, ADMIN_SN_DSP_RUN, ADMIN_SN_DSP_REBOOT
} admin_sn_result_t;
typedef struct {
    char line[256];
    size_t used;
    admin_sn_result_t discard;
    char pin[7];
    uint8_t dsp_bundle[64];
} admin_sn_input_t;
/* No echo/logging. Complete lines and discarded secrets are wiped. */
admin_sn_result_t admin_sn_input_byte(admin_sn_input_t *input, uint8_t byte,
                                     bool (*authenticate)(const char *), char sn[17]);
#endif
