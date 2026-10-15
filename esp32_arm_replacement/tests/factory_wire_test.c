#include "factory_wire.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
int main(void){uint8_t out[FACTORY_FRAME_MAX];factory_frame_t f={.command=2,.id=123,.length=3};memcpy(f.payload,"abc",3);size_t n=factory_frame_encode(&f,out,sizeof(out));assert(n==17);factory_parser_t p={0};factory_frame_t got;bool done=false;for(size_t i=0;i<n;i++)done=factory_parser_feed(&p,out[i],&got);assert(done&&got.id==123&&got.command==2&&got.length==3&&!memcmp(got.payload,"abc",3));out[12]^=1;for(size_t i=0;i<n;i++)assert(!factory_parser_feed(&p,out[i],&got));for(unsigned i=0;i<8;i++)factory_parser_feed(&p,0x99,&got);out[12]^=1;for(size_t i=0;i<n;i++)done=factory_parser_feed(&p,out[i],&got);assert(done);puts("PASS factory framing CRC/noise/corruption/recovery");}
