#include "direct_dsp_stream.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#ifndef DIRECT_DSP_RX_CAPTURE_AVAILABLE
int main(void) { assert(!"DSP diagnostics must preserve the full failed candidate"); }
#else
static unsigned accepted;
static void accept(void *context,const uint8_t *frame,size_t length) {
    (void)context;assert(length==69 && direct_dsp_validate(frame,length));accepted++;
}
static void seal(uint8_t *frame,size_t length) {
    uint16_t crc=direct_crc16(frame,length-2);
    frame[length-2]=(uint8_t)crc;frame[length-1]=(uint8_t)(crc>>8);
}
static void accept_sync(void *context,const uint8_t *frame,size_t length) {
    assert(length==5 && frame[2]==0 && direct_dsp_validate(frame,length));
    (*(unsigned *)context)++;
}
int main(void) {
    direct_dsp_stream_t stream={0};
    const uint8_t observed[]={1,0x12,0,0x21,0x90};
    direct_dsp_stream_feed(&stream,observed,sizeof(observed),47000,accept,NULL);
    assert(!accepted && !stream.fault.sequence);
    direct_dsp_stream_feed(&stream,NULL,0,47201,accept,NULL);
    assert(stream.fault.reason==DIRECT_DSP_CAPTURE_TIMEOUT);
    assert(stream.fault.length==5 && stream.fault.expected_length==22);
    assert(stream.fault.at_ms==47201 && stream.fault.sequence==1);
    assert(!memcmp(stream.fault.bytes,observed,5) && !accepted);

    uint8_t frame[69]={1,65,4};seal(frame,sizeof(frame));
    uint8_t corrupt[69];memcpy(corrupt,frame,69);corrupt[8]^=0x80;
    direct_dsp_stream_feed(&stream,corrupt,sizeof(corrupt),48000,accept,NULL);
    assert(stream.fault.reason==DIRECT_DSP_CAPTURE_CRC && stream.fault.length==69);
    assert(stream.fault.expected_length==69 && stream.fault.sequence==2);
    assert(stream.fault.at_ms==48000 && !memcmp(stream.fault.bytes,corrupt,69));
    direct_dsp_stream_feed(&stream,frame,sizeof(frame),48001,accept,NULL);
    assert(accepted==1 && stream.fault.sequence==2);

    const uint8_t invalid_length[]={1,255};
    direct_dsp_stream_feed(&stream,invalid_length,2,49000,accept,NULL);
    assert(stream.fault.reason==DIRECT_DSP_CAPTURE_LENGTH && stream.fault.length==2);
    assert(stream.fault.expected_length==259 && stream.fault.sequence==3);
    uint8_t maximum[128]={1,124,4};seal(maximum,sizeof(maximum));maximum[7]^=0x20;
    direct_dsp_stream_feed(&stream,maximum,sizeof(maximum),50000,NULL,NULL);
    assert(stream.fault.length==128 && !memcmp(stream.fault.bytes,maximum,128));
    /* Actual device transcripts differ from a valid short CMD00 in byte1.
     * Never normalize this byte or treat the stale checksum as acceptance. */
    const uint8_t requests[][5]={{1,0x41,0,0x21,0x90},{1,0x12,0,0x21,0x90}};
    for(unsigned i=0;i<2;i++){
        direct_dsp_stream_t observed_stream={0};unsigned sync_count=0;
        assert(!direct_dsp_validate(requests[i],5));
        direct_dsp_stream_feed(&observed_stream,requests[i],5,51000,accept_sync,&sync_count);
        direct_dsp_stream_feed(&observed_stream,NULL,0,51201,accept_sync,&sync_count);
        assert(sync_count==0 && observed_stream.stats.frames==0 && observed_stream.stats.timeouts==1);
        assert(observed_stream.fault.length==5 && observed_stream.fault.expected_length==(size_t)requests[i][1]+4U);
        assert(!memcmp(observed_stream.fault.bytes,requests[i],5));
        const uint8_t valid_request[]={1,1,0,0x21,0x90};
        assert(direct_dsp_validate(valid_request,5));
        direct_dsp_stream_feed(&observed_stream,valid_request,5,52000,accept_sync,&sync_count);
        assert(sync_count==1 && observed_stream.stats.frames==1);
    }
    puts("PASS DSP capture: observed partial prefix stays unaccepted; timeout/CRC/length preserve exact bytes, time, size and sequence; recovery and 128B bounds");
    puts("PASS observed CMD00 candidates: 01 41/12 00 21 90 rejected; exact valid 01 01 00 21 90 accepted without normalization");
    return 0;
}
#endif
