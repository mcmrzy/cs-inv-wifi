#ifndef DIRECT_BMS_CALIBRATION_H
#define DIRECT_BMS_CALIBRATION_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Source PC_485_protocol.C:843-1021. Pure codec: no UART, writes, fixture,
 * or capability assumptions. Target support and safe fixture conditions
 * must be confirmed before any caller enables a live write. */
typedef struct {
    uint8_t command;      /* 0x0A cell voltage, 0x0B charge, 0x0C discharge */
    uint8_t point;        /* 0 low reference, 1 high reference */
    uint8_t cell_count;   /* 0x0A: first N cells; current commands: must be 0 */
    uint32_t reference;   /* 0x0A: 1..3999 mV; current: 0..65535 x 100 mA */
} direct_bms_calibration_request_t;

/* The source CELL_VOL_NUM is runtime para_read(PID_FUN_CELL_NUM). For cell
 * voltage require an independently confirmed 1..16 cell count matching
 * this board's AFE and 16 calibration slots. No guessed default is used.
 * Source allows zero volts/count; reject those ineffective writes here.
 * Source imposes no current limit beyond uint16: an approved fixture must
 * impose a much lower physical limit. 0x0D is not implemented upstream. */
size_t direct_bms_calibration_make_request(
    const direct_bms_calibration_request_t *request, uint8_t confirmed_cells,
    uint8_t *out, size_t capacity);

/* True means a well-formed reply exactly echoes this request. *stored is
 * true only for upstream result=1 (parameter flash commit succeeded).
 * That acknowledgement does not prove measurement accuracy or readback.
 * False leaves *stored unchanged. */
bool direct_bms_calibration_parse_ack(
    const uint8_t *frame, size_t length,
    const direct_bms_calibration_request_t *expected, bool *stored);
#endif
