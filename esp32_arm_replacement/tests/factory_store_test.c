#include "factory_store.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
static unsigned char slots[2][FACTORY_RECORD_WIRE_SIZE];
static size_t sizes[2];static bool badwrite;
static bool read_slot(void *ctx,int i,uint8_t *out,size_t *n){(void)ctx;if(!sizes[i]){*n=0;return true;}if(*n<sizes[i])return false;memcpy(out,slots[i],sizes[i]);*n=sizes[i];return true;}
static bool write_slot(void *ctx,int i,const uint8_t *in,size_t n){(void)ctx;memcpy(slots[i],in,n);sizes[i]=badwrite?17:n;return !badwrite;}
static factory_record_t record(void){factory_record_t r={0};uint8_t mac[]={2,3,4,5,6,7};memcpy(r.mac,mac,6);strcpy(r.sn,"H1ZZX0013900001P");strcpy(r.product,"CSERGY");strcpy(r.model,"CS-L10-6K2");strcpy(r.hw_system,"UNSET");strcpy(r.hw_controller,"UNSET");strcpy(r.batch,"test");strcpy(r.production_time,"2026-10-12T00:00:00Z");return r;}
int main(void){factory_store_io_t io={read_slot,write_slot,NULL};factory_record_t r=record(),out;assert(factory_store_read(&io,r.mac,&out,NULL)==FACTORY_UNPROVISIONED);assert(factory_store_provision(&io,r.mac,&r)==FACTORY_OK);assert(factory_store_read(&io,r.mac,&out,NULL)==FACTORY_OK&&out.generation==1);assert(factory_store_provision(&io,r.mac,&r)==FACTORY_OK);assert(factory_store_read(&io,r.mac,&out,NULL)==FACTORY_OK&&out.generation==1);strcpy(r.batch,"new");badwrite=true;assert(factory_store_provision(&io,r.mac,&r)==FACTORY_STORAGE_ERROR);assert(factory_store_read(&io,r.mac,&out,NULL)==FACTORY_OK&&out.generation==1);badwrite=false;assert(factory_store_provision(&io,r.mac,&r)==FACTORY_OK);assert(factory_store_lock(&io,r.mac)==FACTORY_OK);assert(factory_store_provision(&io,r.mac,&r)==FACTORY_OK);strcpy(r.sn,"OTHER");assert(factory_store_provision(&io,r.mac,&r)==FACTORY_LOCKED);r.mac[1]++;assert(factory_store_provision(&io,out.mac,&r)==FACTORY_MAC_MISMATCH);slots[0][22]^=0x20;slots[1][22]^=0x20;assert(factory_store_read(&io,out.mac,&out,NULL)==FACTORY_CORRUPT);strcpy(r.production_time,"2026-02-30T00:00:00Z");assert(factory_record_validate(&r)==FACTORY_INVALID_RECORD);
 memset(r.sn,'X',sizeof(r.sn));assert(factory_record_validate(&r)==FACTORY_INVALID_RECORD);
 puts("PASS factory atomic readback/idempotency/lock/corruption/MAC/power-cut");}
