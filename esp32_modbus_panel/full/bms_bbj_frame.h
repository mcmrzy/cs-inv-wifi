#pragma once
#include <stdbool.h>
#include <stddef.h>
/* Find a complete, strict BBJ 0x80/0x01 response in an accumulated UART
 * buffer. Echo/noise/incomplete headers never become a live battery sample.
 * Input and output offsets remain unchanged on failure. */
bool bms_bbj_find_response(const unsigned char *bytes,size_t count,
                           size_t *offset,size_t *length);
