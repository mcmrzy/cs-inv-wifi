#ifndef DIRECT_DSP_LOG_GATE_H
#define DIRECT_DSP_LOG_GATE_H

#include <string.h>
#include "direct_dsp_stream.h"

/* Consumer-only state. Keep evidence exact instead of hashing it: a hash
 * collision must not hide a changed diagnostic frame. */
typedef struct {
    direct_dsp_rx_capture_t previous;
    uint32_t logged_ms;
    bool printed;
} direct_dsp_log_gate_t;

/* Normal frames ignore receive time/sequence; faults are new events even
 * when their bytes match. Coalesce changes for at most one dump per 5s.
 * A suppressed change is never consumed, so the latest evidence can print
 * on a later consumer tick. Unsigned subtraction handles uptime wrap. */
static inline bool direct_dsp_log_due(direct_dsp_log_gate_t *gate,
                                      const direct_dsp_rx_capture_t *capture,
                                      uint32_t now, bool fault)
{
    if(!capture->sequence || capture->length>sizeof(capture->bytes))return false;
    if(gate->printed){
        if(now-gate->logged_ms<5000U)return false;
        const direct_dsp_rx_capture_t *previous=&gate->previous;
        if(capture->length==previous->length &&
           capture->expected_length==previous->expected_length &&
           capture->reason==previous->reason &&
           (!fault || capture->sequence==previous->sequence) &&
           !memcmp(capture->bytes,previous->bytes,capture->length))return false;
    }
    gate->previous=*capture;gate->logged_ms=now;gate->printed=true;
    return true;
}
#endif
