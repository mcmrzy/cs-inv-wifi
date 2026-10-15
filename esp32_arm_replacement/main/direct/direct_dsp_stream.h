#ifndef DIRECT_DSP_STREAM_H
#define DIRECT_DSP_STREAM_H
#include "direct_protocol.h"

#define DIRECT_DSP_RX_TIMEOUT_MS 200U
#define DIRECT_DSP_RX_CAPTURE_AVAILABLE 1
enum { DIRECT_DSP_CAPTURE_NONE, DIRECT_DSP_CAPTURE_CRC,
       DIRECT_DSP_CAPTURE_TIMEOUT, DIRECT_DSP_CAPTURE_LENGTH };
typedef struct {
    uint8_t bytes[DIRECT_DSP_MAX_FRAME];
    uint16_t length, expected_length;
    uint32_t at_ms, sequence;
    uint8_t reason;
} direct_dsp_rx_capture_t;
typedef struct {
    uint32_t rx_bytes, frames, crc_errors, length_errors, timeouts, noise_bytes;
} direct_dsp_rx_stats_t;
typedef struct {
    uint8_t bytes[DIRECT_DSP_MAX_FRAME];
    size_t used;
    uint32_t last_byte_ms;
    direct_dsp_rx_stats_t stats;
    direct_dsp_rx_capture_t fault; /* Latest candidate, not a proven wire frame. */
} direct_dsp_stream_t;
typedef void (*direct_dsp_frame_cb_t)(void *context,const uint8_t *frame,size_t length);
/* UART owner only. Callback consumes a frame before the buffer is reused. */
void direct_dsp_stream_expire(direct_dsp_stream_t *stream,uint32_t now_ms);
void direct_dsp_stream_feed(direct_dsp_stream_t *stream,const uint8_t *bytes,
                           size_t length,uint32_t now_ms,
                           direct_dsp_frame_cb_t accept,void *context);
#endif
