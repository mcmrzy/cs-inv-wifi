#include "direct_dsp_stream.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned received;
static void accept(void *context,const uint8_t *frame,size_t length) {
    assert(context==&received);
    assert(direct_dsp_validate(frame,length));
    assert(length==69 && frame[2]==4);
    received++;
}
static void crc(uint8_t *frame,size_t length) {
    uint16_t sum=direct_crc16(frame,length-2);
    frame[length-2]=(uint8_t)sum;frame[length-1]=(uint8_t)(sum>>8);
}
int main(void) {
    uint8_t frame[69]={1,65,4},pair[138];crc(frame,sizeof(frame));
    memcpy(pair,frame,69);memcpy(pair+69,frame,69);
    for(size_t split=0;split<=sizeof(pair);split++) {
        direct_dsp_stream_t stream={0};received=0;
        direct_dsp_stream_feed(&stream,pair,split,100,accept,&received);
        direct_dsp_stream_feed(&stream,pair+split,sizeof(pair)-split,101,accept,&received);
        assert(received==2 && stream.stats.frames==2 && !stream.used);
        assert(stream.stats.rx_bytes==sizeof(pair));
    }
    /* Payload can coincidentally contain a CRC-valid short frame. It must
     * never replace an intact outer frame that arrived in several chunks. */
    uint8_t nested[69];memcpy(nested,frame,69);
    nested[10]=1;nested[11]=1;nested[12]=0;crc(nested+10,5);crc(nested,69);
    direct_dsp_stream_t nested_stream={0};received=0;
    direct_dsp_stream_feed(&nested_stream,nested,15,150,accept,&received);
    assert(received==0);
    direct_dsp_stream_feed(&nested_stream,nested+15,54,151,accept,&received);
    assert(received==1 && nested_stream.stats.frames==1);
    direct_dsp_stream_t stream={0};received=0;
    const uint8_t noise[]={0xff,0,1,0,1,0xff,0x99};
    direct_dsp_stream_feed(&stream,noise,sizeof(noise),200,accept,&received);
    direct_dsp_stream_feed(&stream,frame,69,201,accept,&received);
    assert(received==1 && stream.stats.length_errors==2);
    uint8_t corrupt[69];memcpy(corrupt,frame,69);corrupt[8]^=0x80;
    direct_dsp_stream_feed(&stream,corrupt,69,300,accept,&received);
    direct_dsp_stream_feed(&stream,frame,69,301,accept,&received);
    assert(received==2 && stream.stats.crc_errors>=1);
    /* A plausible corrupt length must not swallow the following valid frame. */
    const uint8_t false_header[]={1,100,4,0x99};
    direct_dsp_stream_feed(&stream,false_header,sizeof(false_header),400,accept,&received);
    direct_dsp_stream_feed(&stream,frame,69,401,accept,&received);
    assert(received==2);
    direct_dsp_stream_feed(&stream,NULL,0,602,accept,&received);
    assert(received==3 && !stream.used);
    /* Timed out prefixes are dropped before accepting new data. */
    direct_dsp_stream_feed(&stream,frame,7,700,accept,&received);
    direct_dsp_stream_expire(&stream,901);
    assert(stream.stats.timeouts==2 && !stream.used);
    direct_dsp_stream_feed(&stream,frame,69,902,accept,&received);
    assert(received==4 && stream.stats.frames==4);
    for(unsigned i=0;i<100;i++)direct_dsp_stream_feed(&stream,pair,138,1000+i,accept,&received);
    assert(received==204 && stream.used<=DIRECT_DSP_MAX_FRAME);
    puts("PASS DSP stream: every split, concatenation, noise, CRC/length recovery, swallowed-frame recovery, timeout, bounded storage");
    return 0;
}
