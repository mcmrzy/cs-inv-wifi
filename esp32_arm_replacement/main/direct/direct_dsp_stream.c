#include "direct_dsp_stream.h"
#include <string.h>

static void capture_fault(direct_dsp_stream_t *stream,uint8_t reason,
                          size_t length,size_t expected,uint32_t now) {
    direct_dsp_rx_capture_t *capture=&stream->fault;
    if(length>sizeof(capture->bytes))length=sizeof(capture->bytes);
    memcpy(capture->bytes,stream->bytes,length);
    capture->length=(uint16_t)length;capture->expected_length=(uint16_t)expected;
    capture->at_ms=now;capture->reason=reason;capture->sequence++;
}
static void remove_prefix(direct_dsp_stream_t *stream,size_t length) {
    stream->used-=length;
    memmove(stream->bytes,stream->bytes+length,stream->used);
}
void direct_dsp_stream_expire(direct_dsp_stream_t *stream,uint32_t now) {
    if(stream->used && now-stream->last_byte_ms>DIRECT_DSP_RX_TIMEOUT_MS) {
        stream->stats.timeouts++;
        capture_fault(stream,DIRECT_DSP_CAPTURE_TIMEOUT,stream->used,
            stream->used>=2?(size_t)stream->bytes[1]+4U:0,now);
        /* Wait for timeout before looking inside an unfinished frame: payload
         * bytes can themselves accidentally form a CRC-valid short frame. */
        for(size_t offset=1;offset+5<=stream->used;offset++) {
            size_t candidate=(size_t)stream->bytes[offset+1]+4U;
            if(stream->bytes[offset]==1 && candidate<=stream->used-offset &&
               direct_dsp_validate(stream->bytes+offset,candidate)) {
                stream->stats.length_errors++;
                stream->stats.noise_bytes+=(uint32_t)offset;
                remove_prefix(stream,offset);
                return;
            }
        }
        stream->stats.noise_bytes+=(uint32_t)stream->used;
        stream->used=0;
    }
}
static void consume(direct_dsp_stream_t *stream,direct_dsp_frame_cb_t accept,void *context) {
    while(stream->used) {
        if(stream->bytes[0]!=1) {
            stream->stats.noise_bytes++;remove_prefix(stream,1);continue;
        }
        if(stream->used<2)return;
        size_t length=(size_t)stream->bytes[1]+4U;
        if(length<5 || length>sizeof(stream->bytes)) {
            capture_fault(stream,DIRECT_DSP_CAPTURE_LENGTH,stream->used,length,stream->last_byte_ms);
            stream->stats.length_errors++;remove_prefix(stream,1);continue;
        }
        if(stream->used<length)return;
        if(!direct_dsp_validate(stream->bytes,length)) {
            capture_fault(stream,DIRECT_DSP_CAPTURE_CRC,length,length,stream->last_byte_ms);
            stream->stats.crc_errors++;remove_prefix(stream,1);continue;
        }
        stream->stats.frames++;
        if(accept)accept(context,stream->bytes,length);
        remove_prefix(stream,length);
    }
}
void direct_dsp_stream_feed(direct_dsp_stream_t *stream,const uint8_t *bytes,
                           size_t length,uint32_t now,
                           direct_dsp_frame_cb_t accept,void *context) {
    direct_dsp_stream_expire(stream,now);
    consume(stream,accept,context);
    for(size_t i=0;i<length;i++) {
        /* consume() leaves only an incomplete frame, always smaller than max. */
        stream->bytes[stream->used++]=bytes[i];
        stream->stats.rx_bytes++;
        stream->last_byte_ms=now;
        consume(stream,accept,context);
    }
}
