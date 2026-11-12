#pragma once
#include <stddef.h>
#include <stdint.h>

typedef enum {
    BMS_DIAG_NO_RX, BMS_DIAG_ECHO_ONLY, BMS_DIAG_SHORT_HEADER,
    BMS_DIAG_BAD_HEADER, BMS_DIAG_BAD_ADDRESS, BMS_DIAG_BAD_FUNCTION,
    BMS_DIAG_LENGTH_MISMATCH, BMS_DIAG_BAD_CRC, BMS_DIAG_BAD_CELLS,
    BMS_DIAG_BAD_TEMPS, BMS_DIAG_SHORT_PACK, BMS_DIAG_BAD_VALUES,
    BMS_DIAG_VALID, BMS_DIAG_FRAME_IN_BUFFER
} bms_diag_code_t;
typedef struct {
    bms_diag_code_t code;
    uint32_t expected_length;
    uint16_t wire_crc, calculated_crc;
    uint16_t candidate_offset, candidate_length;
} bms_diag_result_t;
bms_diag_result_t bms_diag_inspect(const uint8_t *request,size_t request_length,
                                 const uint8_t *received,size_t received_length);
const char *bms_diag_reason(bms_diag_code_t code);
