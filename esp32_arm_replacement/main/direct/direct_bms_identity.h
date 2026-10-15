#ifndef DIRECT_BMS_IDENTITY_H
#define DIRECT_BMS_IDENTITY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Optional PC_485_protocol.C read-only 0x08 capability. The available BMS
 * Boot Keil project does not compile that source, so a timeout is not proof
 * of a faulty BMS. These values describe the responding BMS, not inverter
 * model compatibility. Bit n in valid_mask means type n was read. */
typedef struct {
    char software[25];
    char compile_time[25];
    char cpuid[25];
    char hardware[25];
    char module_sn[25];
    char pcb_sn[25];
    char product[25];
    uint32_t app_crc32;
    uint8_t valid_mask;
} direct_bms_identity_t;

/* Types: 0 software, 1 compile time, 2 CPUID, 3 hardware, 4 module SN,
 * 5 PCB SN, 6 BMS product text, 7 application CRC32. */
size_t direct_bms_identity_make_query(uint8_t type, uint8_t out[8]);
/* Exactly 32 bytes required. On failure out is unchanged. On success only
 * the requested field and its validity bit are updated. Text uses original
 * wire byte order; the BMS parameter words must not be swapped again. */
bool direct_bms_identity_parse_reply(const uint8_t *frame, size_t len,
                                    uint8_t expected_type,
                                    direct_bms_identity_t *out);

#endif
