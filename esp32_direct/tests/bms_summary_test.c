#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "direct_protocol.h"

static void put16(uint8_t *p, uint16_t n){p[0]=(uint8_t)n;p[1]=(uint8_t)(n>>8);}
static uint16_t get16(const uint8_t *p){return (uint16_t)(p[0]|((uint16_t)p[1]<<8));}
static void crc(uint8_t *f,size_t n){put16(f+1,direct_crc16(f+3,n-3));}

int main(int argc, char **argv)
{
    uint8_t f[110]={0x7c,0,0,0x80,1,103,0,16};
    for(unsigned i=0;i<16;i++)put16(f+8+i*2,(uint16_t)(3300+i));
    put16(f+8,0x8000|3300);put16(f+38,0x8000|3315);
    f[40]=4;
    f[41]=30; f[42]=0;       /* cell -10 C */
    f[43]=65; f[44]=0;       /* cell 25 C */
    f[45]=85; f[46]=3<<5;    /* MOS 45 C */
    f[47]=75; f[48]=2<<5;    /* PCB 35 C */
    put16(f+49,(uint16_t)-123);put16(f+51,520);
    put16(f+53,805);put16(f+55,990);
    put16(f+57,1234);put16(f+59,2345);put16(f+61,3456);
    f[63]=2;f[64]=5;put16(f+77,4321);
    f[79]=0x78;f[80]=0x56;f[81]=0x34;f[82]=0x12;
    f[83]=0xef;f[84]=0xcd;f[85]=0xab;f[86]=0x89;
    put16(f+105,150);put16(f+107,560);f[109]=3;
    crc(f,sizeof(f));
    uint8_t b[100];
    assert(direct_bms_encode_summary_7c(f,sizeof(f),b));
    assert(b[0]==16 && get16(b+1)==5200);
    assert((int32_t)((uint32_t)b[3]|((uint32_t)b[4]<<8)|((uint32_t)b[5]<<16)|((uint32_t)b[6]<<24))==-1230);
    assert(get16(b+7)==805 && get16(b+9)==990);
    assert(get16(b+11)==1234 && get16(b+13)==2345);
    assert(get16(b+15)==3456 && !b[17] && !b[18]);
    assert(get16(b+25)==0x8001);
    for(unsigned i=0;i<16;i++)assert(get16(b+27+i*2)==3300+i);
    assert((int16_t)get16(b+59)==-10 && get16(b+61)==25);
    assert(get16(b+67)==4321 && get16(b+69)==3315 && get16(b+71)==3300);
    assert((int16_t)get16(b+73)==25 && (int16_t)get16(b+75)==-10);
    assert(get16(b+77)==45 && get16(b+79)==35);
    assert(b[83]==2 && b[84]==5 && !memcmp(b+85,f+79,8));
    assert(get16(b+93)==150 && get16(b+95)==560 && b[97]==3);
    for(unsigned i=19;i<25;i++)assert(b[i]==0); /* No guessed flag conversion. */
    assert(get16(b+98)==0); /* BMS has no independent charging_voltage. */
    uint8_t saved[100];memcpy(saved,b,sizeof(b));
    f[50]^=1;assert(!direct_bms_encode_summary_7c(f,sizeof(f),b));
    assert(!memcmp(b,saved,sizeof(b)));f[50]^=1;
    for(size_t n=0;n<sizeof(f);n++)assert(!direct_bms_encode_summary_7c(f,n,b));
    f[3]=0x81;crc(f,sizeof(f));assert(!direct_bms_encode_summary_7c(f,sizeof(f),b));f[3]=0x80;
    f[4]=8;crc(f,sizeof(f));assert(!direct_bms_encode_summary_7c(f,sizeof(f),b));f[4]=1;
    f[5]=102;crc(f,sizeof(f));assert(!direct_bms_encode_summary_7c(f,sizeof(f),b));f[5]=103;
    f[7]=17;crc(f,sizeof(f));assert(!direct_bms_encode_summary_7c(f,sizeof(f),b));f[7]=16;
    f[40]=17;crc(f,sizeof(f));assert(!direct_bms_encode_summary_7c(f,sizeof(f),b));f[40]=4;
    put16(f+51,65535);crc(f,sizeof(f));assert(!direct_bms_encode_summary_7c(f,sizeof(f),b));
    assert(!direct_bms_encode_summary_7c(NULL,sizeof(f),b));
    assert(!direct_bms_encode_summary_7c(f,sizeof(f),NULL));
    puts("PASS BBJ to L10 BMS layout: signed units, SOC, capacities, cells/balance, typed temperatures, counters, strict full-frame validation, no overwrite on invalid data");
    if(argc==2 && !strcmp(argv[1],"--json")) {
        printf("{\"layout\":0,\"age_ms\":0,\"bytes\":[");
        for(unsigned i=0;i<100;i++)printf(i?",%u":"%u",b[i]);
        puts("]}");
    }
    return 0;
}
