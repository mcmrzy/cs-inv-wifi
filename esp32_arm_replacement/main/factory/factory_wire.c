#include "factory_wire.h"
#include <string.h>
uint32_t factory_wire_crc32(const uint8_t *p,size_t n){uint32_t c=~0u;while(n--){c^=*p++;for(int b=0;b<8;b++)c=(c>>1)^(0xedb88320u&(0u-(c&1)));}return ~c;}
static uint32_t get32(const uint8_t*p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void put32(uint8_t*p,uint32_t n){for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(n>>(8*i));}
size_t factory_frame_encode(const factory_frame_t*f,uint8_t*out,size_t cap){
 if(!f||!out||f->length>FACTORY_PAYLOAD_MAX||cap<(size_t)f->length+14)return 0;
 out[0]=0xc5;out[1]=0x46;out[2]=1;out[3]=f->command;put32(out+4,f->id);out[8]=(uint8_t)f->length;out[9]=(uint8_t)(f->length>>8);
 memcpy(out+10,f->payload,f->length);put32(out+10+f->length,factory_wire_crc32(out+2,8+f->length));return f->length+14;
}
bool factory_parser_feed(factory_parser_t*p,uint8_t byte,factory_frame_t*out){
 if(!p||!out)return false;
 if(!p->used){if(byte==0xc5)p->bytes[p->used++]=byte;return false;}
 if(p->used==1&&byte!=0x46){p->used=byte==0xc5?1:0;return false;}
 p->bytes[p->used++]=byte;
 if(p->used==3&&byte!=1){p->used=0;return false;}
 if(p->used<10)return false;
 size_t n=p->bytes[8]|(size_t)p->bytes[9]<<8;
 if(n>FACTORY_PAYLOAD_MAX){p->used=0;return false;}
 if(p->used<n+14)return false;
 bool valid=get32(p->bytes+10+n)==factory_wire_crc32(p->bytes+2,8+n);
 if(valid){out->command=p->bytes[3];out->id=get32(p->bytes+4);out->length=(uint16_t)n;memcpy(out->payload,p->bytes+10,n);}
 p->used=0;return valid;
}
