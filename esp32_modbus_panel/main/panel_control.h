#pragma once
#include "panel_types.h"

typedef enum { PANEL_IO_OK, PANEL_IO_EXCEPTION, PANEL_IO_ERROR, PANEL_IO_NAK } panel_io_result_t;
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
               PANEL_CONTROL_ACTUAL, PANEL_CONTROL_FLAGS, PANEL_CONTROL_POWER_OUTPUT } panel_control_stage_t;
typedef struct {
    uint32_t last_write_ms, started_ms, next_io_ms,context_generation;
    uint32_t output_first_ms,output_last_ms;
    uint16_t reg, value, actual_reg,marker_raw,actual_raw,readback_value;
    uint8_t output_samples;
    bool have_write,marker_seen,actual_seen,readback_valid,require_opposite_output,output_started_opposite;
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
/* Called only by the UART owner. Never retransmit an uncertain write. Power
 * on/off uses fresh output-voltage samples from normal polling, even without ACK.
 * For other commands after ACK,
 * compare one independent readback, including any required companion flags.
 * Use the command completion marker if independent data is unavailable.
 * Same-address command values alone cannot confirm adoption. */
/* Blocking compatibility wrapper; the live UART owner uses begin/tick. */
void panel_control_write(panel_control_t *, panel_snapshot_t *, uint16_t reg,
                         uint16_t value, const panel_control_io_t *);

/* -1 when output voltage cannot establish state, otherwise 0=off or 1=on. */
int panel_power_output_state(const panel_snapshot_t *snapshot);
