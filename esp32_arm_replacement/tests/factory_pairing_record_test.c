#include "factory_pairing_record.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
int main(void){
 factory_pairing_record_t a={.generation=1,.mac={2,3,4,5,6,7},.sn="H1ZZX0013900001P",.pin="000001"},out;
 uint8_t w[FACTORY_PAIRING_WIRE_SIZE],v[FACTORY_PAIRING_WIRE_SIZE];int selected;
 assert(factory_pairing_encode(&a,w,sizeof(w))==FACTORY_OK);
 assert(factory_pairing_decode(w,sizeof(w),&out)==FACTORY_OK&&!strcmp(out.pin,"000001"));
 assert(!factory_pairing_values_valid("SHORT","000001"));assert(!factory_pairing_values_valid("h1ZZX0013900001P","000001"));
 assert(!factory_pairing_values_valid(a.sn,"12345"));assert(!factory_pairing_values_valid(a.sn,"12345x"));
 assert(factory_pairing_select(NULL,0,NULL,0,&out,NULL)==FACTORY_UNPROVISIONED);
 memcpy(v,w,sizeof(v));v[20]^=1;
 assert(factory_pairing_decode(v,sizeof(v),&out)==FACTORY_CORRUPT);
 assert(factory_pairing_select(w,sizeof(w),v,sizeof(v),&out,&selected)==FACTORY_OK&&selected==0);
 strcpy(a.pin,"000002");assert(factory_pairing_encode(&a,v,sizeof(v))==FACTORY_OK);
 assert(factory_pairing_select(w,sizeof(w),v,sizeof(v),&out,&selected)==FACTORY_CORRUPT);
 a.generation=2;assert(factory_pairing_encode(&a,v,sizeof(v))==FACTORY_OK);
 assert(factory_pairing_select(w,sizeof(w),v,sizeof(v),&out,&selected)==FACTORY_OK&&selected==1);
 a.mac[0]=3;assert(factory_pairing_encode(&a,v,sizeof(v))==FACTORY_INVALID_RECORD);
 a.mac[0]=2;a.generation=0;assert(factory_pairing_encode(&a,v,sizeof(v))==FACTORY_INVALID_RECORD);
 puts("PASS explicit pairing codec, leading zero, strict format, CRC, partial peer and conflict");
}
