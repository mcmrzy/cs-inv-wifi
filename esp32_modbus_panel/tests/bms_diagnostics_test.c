#include "bms_diagnostics.h"
#include "direct_protocol.h"
#include "bms_bbj_frame.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static void put16(uint8_t *p,unsigned value){p[0]=(uint8_t)value;p[1]=(uint8_t)(value>>8);}
static void crc(uint8_t *p,size_t n){put16(p+1,direct_crc16(p+3,n-3));}
static void expect(const uint8_t *p,size_t n,bms_diag_code_t code)
{
    uint8_t request[7];direct_bms_make_7c_query(request);
    bms_diag_result_t r=bms_diag_inspect(request,sizeof(request),p,n);
    if(r.code!=code)fprintf(stderr,"expected %s, got %s\n",bms_diag_reason(code),bms_diag_reason(r.code));
    assert(r.code==code);
}
int main(void)
{
    uint8_t f[110]={0x7c,0,0,0x80,1,103,0,16},request[7],joined[256]={0};
    for(unsigned i=0;i<16;i++)put16(f+8+i*2,3300+i);
    f[40]=4;put16(f+49,(uint16_t)-123);put16(f+51,520);put16(f+53,805);put16(f+55,990);crc(f,sizeof(f));
    assert(direct_bms_make_7c_query(request)==7);
    expect(NULL,0,BMS_DIAG_NO_RX);expect(request,7,BMS_DIAG_ECHO_ONLY);
    for(size_t n=1;n<7;n++)expect(f,n,BMS_DIAG_SHORT_HEADER);
    for(size_t n=8;n<110;n++)expect(f,n,BMS_DIAG_LENGTH_MISMATCH);
    expect(f,sizeof(f),BMS_DIAG_VALID);
    f[109]^=1;expect(f,sizeof(f),BMS_DIAG_BAD_CRC);f[109]^=1;
    f[0]=0xff;expect(f,sizeof(f),BMS_DIAG_BAD_HEADER);f[0]=0x7c;
    f[3]=0x81;expect(f,sizeof(f),BMS_DIAG_BAD_ADDRESS);f[3]=0x80;
    f[4]=8;expect(f,sizeof(f),BMS_DIAG_BAD_FUNCTION);f[4]=1;
    f[7]=17;crc(f,sizeof(f));expect(f,sizeof(f),BMS_DIAG_BAD_CELLS);f[7]=16;
    f[40]=17;crc(f,sizeof(f));expect(f,sizeof(f),BMS_DIAG_BAD_TEMPS);f[40]=4;
    put16(f+51,65535);crc(f,sizeof(f));expect(f,sizeof(f),BMS_DIAG_BAD_VALUES);put16(f+51,520);crc(f,sizeof(f));
    uint8_t short_pack[50];memcpy(short_pack,f,50);put16(short_pack+5,43);crc(short_pack,50);
    expect(short_pack,50,BMS_DIAG_SHORT_PACK);
    memcpy(joined,request,7);memcpy(joined+7,f,sizeof(f));
    for(size_t n=0;n<117;n++){
        size_t offset=999,length=999;
        assert(!bms_bbj_find_response(joined,n,&offset,&length));
        assert(offset==999 && length==999);
    }
    size_t offset=999,length=999;
    assert(bms_bbj_find_response(joined,117,&offset,&length) && offset==7 && length==110);
    bms_diag_result_t r=bms_diag_inspect(request,7,joined,117);
    assert(r.code==BMS_DIAG_FRAME_IN_BUFFER && r.candidate_offset==7 && r.candidate_length==110);
    /* Every possible prefix offset in the actual 256-byte receive buffer. */
    for(unsigned offset=0;offset<=146;offset++){
        memset(joined,0xff,sizeof(joined));memcpy(joined+offset,f,110);
        r=bms_diag_inspect(request,7,joined,256);
        assert(r.code==BMS_DIAG_FRAME_IN_BUFFER && r.candidate_offset==offset && r.candidate_length==110);
    }
    r=bms_diag_inspect(request,7,f,110);assert(r.wire_crc==r.calculated_crc && r.expected_length==110);
    puts("PASS BBJ diagnostics: no RX, echo, every truncation, CRC/header/address/function/cell/temp/pack/value errors, valid frame and every embedded offset");
}
