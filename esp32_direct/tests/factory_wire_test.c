#include "factory_wire.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
static void view_roundtrip_and_aliasing(void){
 uint8_t wire[FACTORY_FRAME_MAX],expected[FACTORY_PAYLOAD_MAX];
 for(size_t i=0;i<sizeof(expected);i++)expected[i]=(uint8_t)(i*37U);
 factory_frame_view_t f={.command=2,.id=0x12345678,.length=sizeof(expected),.payload=expected};
 size_t n=factory_frame_view_encode(&f,wire,sizeof(wire));assert(n==sizeof(wire));
 factory_parser_t parser={0};factory_frame_view_t got={0};bool done=false;
 for(size_t i=0;i<n;i++)done=factory_parser_feed_view(&parser,wire[i],&got);
 assert(done&&parser.used==0&&got.payload==parser.bytes+10);
 assert(got.command==2&&got.id==f.id&&got.length==sizeof(expected));
 assert(!memcmp(got.payload,expected,sizeof(expected)));
 got.command|=0x80;
 assert(factory_frame_view_encode(&got,parser.bytes,sizeof(parser.bytes))==n);
 factory_parser_t peer={0};factory_frame_t copied={0};
 for(size_t i=0;i<n;i++)done=factory_parser_feed(&peer,parser.bytes[i],&copied);
 assert(done&&copied.command==0x82&&!memcmp(copied.payload,expected,sizeof(expected)));

 /* In-place response sources may start before, at, or after the output payload. */
 for(unsigned offset=0;offset<=12;offset++){
  uint8_t shared[96];memcpy(shared+offset,expected,64);
  f.length=64;f.payload=shared+offset;
  assert(factory_frame_view_encode(&f,shared,sizeof(shared))==78);
  memset(&peer,0,sizeof(peer));
  for(unsigned i=0;i<78;i++)done=factory_parser_feed(&peer,shared[i],&copied);
  assert(done&&!memcmp(copied.payload,expected,64));
 }
 f.length=0;f.payload=NULL;assert(factory_frame_view_encode(&f,wire,sizeof(wire))==14);
 f.length=1;assert(!factory_frame_view_encode(&f,wire,sizeof(wire)));
 f.payload=expected;f.length=FACTORY_PAYLOAD_MAX+1;assert(!factory_frame_view_encode(&f,wire,sizeof(wire)));
 f.length=64;assert(!factory_frame_view_encode(&f,wire,77));
}
static void view_corruption_and_resync(void){
 uint8_t payload[]={0xc5,0x46,1,0,0xc5},wire[FACTORY_FRAME_MAX];
 factory_frame_view_t f={.command=4,.id=9,.length=sizeof(payload),.payload=payload},got={0};
 size_t n=factory_frame_view_encode(&f,wire,sizeof(wire));factory_parser_t p={0};bool done=false;
 wire[n-1]^=1;
 for(size_t i=0;i<n;i++)assert(!factory_parser_feed_view(&p,wire[i],&got));
 assert(p.used==0);wire[n-1]^=1;
 uint8_t invalid_version[]={0xc5,0x46,2},invalid_length[]={0xc5,0x46,1,0,0,0,0,0,1,0x10};
 for(size_t i=0;i<sizeof(invalid_version);i++)assert(!factory_parser_feed_view(&p,invalid_version[i],&got));
 for(size_t i=0;i<sizeof(invalid_length);i++)assert(!factory_parser_feed_view(&p,invalid_length[i],&got));
 assert(p.used==0);
 assert(!factory_parser_feed_view(&p,0xc5,&got));
 for(size_t i=0;i<n;i++)done=factory_parser_feed_view(&p,wire[i],&got);
 assert(done&&got.id==9&&got.length==sizeof(payload)&&!memcmp(got.payload,payload,sizeof(payload)));
 for(size_t i=0;i<n;i++)done=factory_parser_feed_view(&p,wire[i],&got);
 assert(done&&got.id==9);
}
int main(void){
 view_roundtrip_and_aliasing();view_corruption_and_resync();
 uint8_t out[FACTORY_FRAME_MAX];factory_frame_t f={.command=2,.id=123,.length=3};
 memcpy(f.payload,"abc",3);size_t n=factory_frame_encode(&f,out,sizeof(out));assert(n==17);
 factory_parser_t p={0};factory_frame_t got;bool done=false;
 for(size_t i=0;i<n;i++)done=factory_parser_feed(&p,out[i],&got);
 assert(done&&got.id==123&&got.command==2&&got.length==3&&!memcmp(got.payload,"abc",3));
 out[12]^=1;for(size_t i=0;i<n;i++)assert(!factory_parser_feed(&p,out[i],&got));
 for(unsigned i=0;i<8;i++)factory_parser_feed(&p,0x99,&got);
 out[12]^=1;for(size_t i=0;i<n;i++)done=factory_parser_feed(&p,out[i],&got);assert(done);
 f.length=FACTORY_PAYLOAD_MAX;memset(f.payload,'x',f.length);n=factory_frame_encode(&f,out,sizeof(out));
 assert(n==FACTORY_PAYLOAD_MAX+14);
 for(unsigned round=0;round<2;round++){
  for(size_t i=0;i<n;i++)done=factory_parser_feed(&p,out[i],&got);
  assert(done&&p.used==0&&got.length==FACTORY_PAYLOAD_MAX);
  got.command|=0x80;
  size_t reply_size=factory_frame_encode(&got,p.bytes,sizeof(p.bytes));
  factory_parser_t peer={0};factory_frame_t reply;
  for(size_t i=0;i<reply_size;i++)done=factory_parser_feed(&peer,p.bytes[i],&reply);
  assert(done&&reply.command==0x82&&reply.id==123&&!memcmp(reply.payload,f.payload,f.length));
 }
 puts("PASS factory framing CRC/noise/corruption/recovery, 4096-byte payload and reused request/parser response buffers");
}
