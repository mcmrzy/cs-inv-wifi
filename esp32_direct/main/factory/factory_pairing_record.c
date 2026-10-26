#include "factory_pairing_record.h"
#include <string.h>
static void put32(uint8_t *p,uint32_t v){for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(v>>(i*8));}
static uint32_t get32(const uint8_t *p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static uint32_t crc(const uint8_t *p,size_t n){uint32_t c=~0u;while(n--){c^=*p++;for(unsigned i=0;i<8;i++)c=(c>>1)^(0xedb88320u&(0u-(c&1)));}return ~c;}
bool factory_pairing_values_valid(const char *sn,const char *pin){
 if(!sn||!pin)return false;
 for(unsigned i=0;i<16;i++)if(!((sn[i]>='A'&&sn[i]<='Z')||(sn[i]>='0'&&sn[i]<='9')))return false;
 if(sn[16])return false;
 for(unsigned i=0;i<6;i++)if(pin[i]<'0'||pin[i]>'9')return false;
 return !pin[6];
}
factory_result_t factory_pairing_encode(const factory_pairing_record_t *r,uint8_t *w,size_t n){
 if(!r||!w||n!=FACTORY_PAIRING_WIRE_SIZE)return FACTORY_INVALID_ARGUMENT;
 if(!factory_pairing_values_valid(r->sn,r->pin)||!r->generation)return FACTORY_INVALID_RECORD;
 uint8_t any=0,all=255;for(unsigned i=0;i<6;i++){any|=r->mac[i];all&=r->mac[i];}
 if(!any||all==255||(r->mac[0]&1))return FACTORY_INVALID_RECORD;
 memset(w,0,n);memcpy(w,"PR01",4);put32(w+4,r->generation);memcpy(w+8,r->mac,6);memcpy(w+14,r->sn,16);memcpy(w+30,r->pin,6);put32(w+44,crc(w,44));return FACTORY_OK;
}
factory_result_t factory_pairing_decode(const uint8_t *w,size_t n,factory_pairing_record_t *r){
 if(!w||!r||n!=FACTORY_PAIRING_WIRE_SIZE)return FACTORY_CORRUPT;
 if(memcmp(w,"PR01",4)||get32(w+44)!=crc(w,44))return FACTORY_CORRUPT;
 for(unsigned i=36;i<44;i++)if(w[i])return FACTORY_CORRUPT;
 memset(r,0,sizeof(*r));r->generation=get32(w+4);memcpy(r->mac,w+8,6);memcpy(r->sn,w+14,16);memcpy(r->pin,w+30,6);
 uint8_t check[FACTORY_PAIRING_WIRE_SIZE];return factory_pairing_encode(r,check,sizeof(check))==FACTORY_OK?FACTORY_OK:FACTORY_CORRUPT;
}
factory_result_t factory_pairing_select(const uint8_t *a,size_t na,const uint8_t *b,size_t nb,factory_pairing_record_t *out,int *selected){
 if(!out)return FACTORY_INVALID_ARGUMENT;
 factory_pairing_record_t r[2];bool va=na&&factory_pairing_decode(a,na,&r[0])==FACTORY_OK,vb=nb&&factory_pairing_decode(b,nb,&r[1])==FACTORY_OK;
 if(!va&&!vb)return (na||nb)?FACTORY_CORRUPT:FACTORY_UNPROVISIONED;
 if(va&&vb&&r[0].generation==r[1].generation&&memcmp(a,b,FACTORY_PAIRING_WIRE_SIZE))return FACTORY_CORRUPT;
 int s=(!va||(vb&&r[1].generation>r[0].generation))?1:0;*out=r[s];if(selected)*selected=s;return FACTORY_OK;
}
