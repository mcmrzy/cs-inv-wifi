#pragma once
#include "panel_types.h"

typedef enum { PANEL_IO_OK, PANEL_IO_EXCEPTION, PANEL_IO_ERROR } panel_io_result_t;
typedef struct {
    void *context;
    uint32_t (*now)(void *);
    void (*sleep)(void *, uint32_t ms);
    panel_io_result_t (*read)(void *, uint16_t start, uint16_t count,
                             uint16_t *values, uint8_t *exception);
    panel_io_result_t (*write)(void *, uint16_t reg, uint16_t value, uint8_t *exception);
    void (*publish)(void *, const panel_snapshot_t *);
} panel_control_io_t;

typedef enum { PANEL_CONTROL_IDLE, PANEL_CONTROL_WRITE, PANEL_CONTROL_MARKER,
               PANEL_CONTROL_ACTUAL, PANEL_CONTROL_FLAGS } panel_control_stage_t;
typedef struct {
    uint32_t last_write_ms, started_ms, next_io_ms,context_generation;
    uint16_t reg, value, actual_reg,marker_raw,actual_raw;
    bool have_write, processed,marker_seen,actual_seen,actual_conflict;
    panel_control_stage_t stage;
} panel_control_t;
/* begin performs admission only; each tick performs at most one UART request
 * and never sleeps. Keep normal polling between ticks. True means terminal. */
bool panel_control_begin(panel_control_t *,panel_snapshot_t *,uint16_t reg,
                         uint16_t value,const panel_control_io_t *);
bool panel_control_tick(panel_control_t *,panel_snapshot_t *,const panel_control_io_t *);
bool panel_control_active(const panel_control_t *);
/* Stop this request before a quiet/context transition. An already-sent write
 * stays uncertain; cancellation never retransmits or invents adoption. */
bool panel_control_cancel(panel_control_t *,panel_snapshot_t *,const panel_control_io_t *);
/* Called only by the UART owner. Never retransmit an uncertain write. After ACK,
 * query both completion and independent value; a missing marker must not block
 * a valid independent read. Same-address command values alone cannot confirm
 * adoption. FFFF is protocol success; CONFIRMED needs matching independent data.
 * PROCESSED is reserved for a completed write with conflicting actual data;
 * PROTOCOL_ACCEPTED has a completion receipt and no observed actual conflict. */
/* Blocking compatibility wrapper; the live UART owner uses begin/tick. */
void panel_control_write(panel_control_t *, panel_snapshot_t *, uint16_t reg,
                         uint16_t value, const panel_control_io_t *);
