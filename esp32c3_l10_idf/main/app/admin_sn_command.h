#ifndef ADMIN_SN_COMMAND_H
#define ADMIN_SN_COMMAND_H
#include <stddef.h>
#include <stdint.h>
#define ADMIN_SN_LENGTH 16
typedef enum {
    ADMIN_SN_NONE, ADMIN_SN_VALID, ADMIN_SN_AUTH_FAILED,
    ADMIN_SN_INVALID_ARGS, ADMIN_SN_TOO_LONG
} admin_sn_result_t;
typedef struct {
    char line[128];
    size_t used;
    admin_sn_result_t discard;
} admin_sn_input_t;
/* No echo/logging. Complete lines and discarded secrets are wiped. */
admin_sn_result_t admin_sn_input_byte(admin_sn_input_t *input, uint8_t byte,
                                     const char *password, char sn[17]);
#endif
