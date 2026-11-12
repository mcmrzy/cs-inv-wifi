#include "diag_core.h"
#include "panel_modbus.h"
#include "direct_protocol.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static size_t finish(uint8_t *p,size_t n)
{uint16_t c=panel_mb_crc(p,n);p[n]=(uint8_t)c;p[n+1]=(uint8_t)(c>>8);return n+2;}
static void commands(void)
{
    diag_command_t c;
    assert(diag_command("read 0042 1 247",&c));
    assert(c.kind==CMD_READ && c.arg[0]==0x42 && c.arg[1]==1 && c.arg[2]==247);
    assert(diag_command("read 0x0042",&c) && c.arg[1]==1 && c.arg[2]==0);
    assert(diag_command("baud 115200",&c) && c.arg[0]==115200);
    assert(diag_command("pins 0 1",&c) && c.arg[0]==0 && c.arg[1]==1);
    assert(diag_command("pins 1 0",&c));
    assert(diag_command("format 8e2",&c) && c.parity=='E' && c.arg[0]==2);
    assert(diag_command("scan",&c) && c.arg[0]==1 && c.arg[1]==247 && c.arg[2]==0x42);
    assert(diag_command("scan 1 247 0000 4",&c) && c.arg[3]==4);
    assert(diag_command("hunt",&c) && c.arg[0]==1 && c.arg[1]==16);
    assert(diag_command("poll 0042 2 1000",&c) && c.arg[2]==1000);
    assert(diag_command("invert both",&c) && c.arg[0]==3);
    assert(diag_command("txcrc 01 03 00 42 00 01",&c) && c.length==8);
    uint8_t expected[8]={1,3,0,0x42,0,1};finish(expected,6);
    assert(!memcmp(c.data,expected,8));
    assert(diag_command("tx 010300420001",&c) && c.length==6);
    assert(diag_command("crc 01 03 00 42 00 01",&c) && c.length==6);
    const char *bad[]={"","baud 0","baud 9600junk","baud -9600","pins 0 0","pins 18 19",
        "addr 0","addr 248","format 7N1","format 8X1","format 8N9","fc 16",
        "read 10000","read 0000 126","read FFFF 2","read 0042 0","read 0042 1 0",
        "scan 2 1","scan 0 247","scan 1 248","scan 1 2 0042 16","hunt 0 16",
        "poll 0042 1 0","timeout 0","listen 0","tx 1","tx GG","tx","swap junk",
        "verbose 2","invert random","levels 10001"};
    for(size_t i=0;i<sizeof(bad)/sizeof(bad[0]);i++)assert(!diag_command(bad[i],&c));
    char big[DIAG_LINE_MAX+2];memset(big,'A',sizeof(big));big[sizeof(big)-1]=0;
    assert(!diag_command(big,&c));
    char max[800]="tx ";for(unsigned i=0;i<256;i++)strcat(max,"AA ");
    assert(diag_command(max,&c) && c.length==256);
    max[0]=' '; /* malformed command, never silently send partial data */
    assert(!diag_command(max,&c));
    puts("PASS commands: pin/baud/format/address bounds, hex registers, raw CRC, no implicit writes");
}
static void replies(void)
{
    uint8_t tx[8],rx[1024];diag_reply_t r;
    assert(panel_mb_crc((const uint8_t *)"123456789",9)==0x4b37);
    assert(diag_read_request(1,3,0x42,1,tx)==8 && panel_mb_crc(tx,8)==0);
    assert(tx[2]==0 && tx[3]==0x42);
    assert(diag_read_request(0,3,0,1,tx)==0);
    assert(diag_read_request(1,16,0,1,tx)==0);
    assert(diag_read_request(1,3,65535,2,tx)==0);
    assert(diag_read_request(1,3,0x42,1,tx)==8);
    r=diag_analyze(tx,8,rx,0,1,3,1,false);assert(r.result==DIAG_NO_RX);
    memcpy(rx,tx,8);r=diag_analyze(tx,8,rx,8,1,3,1,false);assert(r.result==DIAG_ECHO && r.echo);
    uint8_t valid[7]={1,3,2,0,5};finish(valid,5);
    for(size_t n=1;n<7;n++){r=diag_analyze(tx,8,valid,n,1,3,1,false);assert(r.result!=DIAG_READ_OK);}
    memcpy(rx,tx,8);rx[8]=0xff;memcpy(rx+9,valid,7);
    r=diag_analyze(tx,8,rx,16,1,3,1,false);
    assert(r.result==DIAG_READ_OK && r.echo && r.offset==9 && r.values[0]==5);
    valid[6]^=1;r=diag_analyze(tx,8,valid,7,1,3,1,false);assert(r.result==DIAG_CRC_BAD);
    valid[6]^=1;valid[0]=2;finish(valid,5);
    r=diag_analyze(tx,8,valid,7,1,3,1,false);assert(r.result==DIAG_UNMATCHED);
    valid[0]=1;valid[1]=4;finish(valid,5);
    r=diag_analyze(tx,8,valid,7,1,3,1,false);assert(r.result==DIAG_UNMATCHED);
    assert(diag_read_request(247,4,0x42,1,tx)==8);valid[0]=247;finish(valid,5);
    r=diag_analyze(tx,8,valid,7,247,4,1,false);assert(r.result==DIAG_READ_OK);
    uint8_t ex[5]={247,0x84,2};finish(ex,3);
    r=diag_analyze(tx,8,ex,5,247,4,1,false);assert(r.result==DIAG_EXCEPTION && r.exception==2);
    ex[2]=0;finish(ex,3);r=diag_analyze(tx,8,ex,5,247,4,1,false);assert(r.result!=DIAG_EXCEPTION);
    assert(diag_read_request(1,3,0,125,tx)==8);
    rx[0]=1;rx[1]=3;rx[2]=250;for(unsigned i=0;i<250;i++)rx[3+i]=(uint8_t)i;
    size_t n=finish(rx,253);r=diag_analyze(tx,8,rx,n,1,3,125,false);
    assert(r.result==DIAG_READ_OK && r.count==125 && r.values[124]==0xf8f9);
    uint8_t legacy[9];assert(diag_legacy_request(legacy)==9 && panel_mb_crc(legacy,9)==0);
    uint8_t original[9];assert(direct_dsp_make_request(4,0,0,0,original)==9);
    assert(!memcmp(original,legacy,9) && DIRECT_DSP_CMD04_LEN==69 && DIRECT_DSP_MAX_FRAME==128);
    assert(legacy[0]==1 && legacy[1]==5 && legacy[2]==4);
    r=diag_analyze(legacy,9,legacy,9,1,0,0,true);assert(r.result==DIAG_ECHO);
    memset(rx,0,69);rx[0]=1;rx[1]=65;rx[2]=4;finish(rx,67);
    r=diag_analyze(legacy,9,rx,69,1,0,0,true);assert(r.result==DIAG_LEGACY_OK);
    memset(rx,0,129);rx[0]=1;rx[1]=125;rx[2]=4;finish(rx,127);
    r=diag_analyze(legacy,9,rx,129,1,0,0,true);assert(r.result!=DIAG_LEGACY_OK);
    puts("PASS replies: silence/echo/noise/partial/CRC/address/function/count/exception/max frame/legacy");
}
int main(void){commands();replies();return 0;}
