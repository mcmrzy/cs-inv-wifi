#include "factory_store.h"
#include <string.h>
#include <limits.h>
factory_result_t factory_store_read(const factory_store_io_t *io,const uint8_t mac[6],factory_record_t *out,int *selected){
 if(!io||!io->read||!mac||!out)return FACTORY_INVALID_ARGUMENT;
 uint8_t slots[2][FACTORY_RECORD_WIRE_SIZE];size_t size[2]={sizeof(slots[0]),sizeof(slots[1])};
 for(int i=0;i<2;i++)if(!io->read(io->context,i,slots[i],&size[i]))return FACTORY_STORAGE_ERROR;
 int index=-1;factory_result_t res=factory_record_select(size[0]?slots[0]:NULL,size[0],size[1]?slots[1]:NULL,size[1],out,&index);
 if(res==FACTORY_OK&&memcmp(out->mac,mac,6))return FACTORY_MAC_MISMATCH;
 if(res==FACTORY_OK&&selected)*selected=index;
 return res;
}
static factory_result_t save(const factory_store_io_t *io,const uint8_t mac[6],factory_record_t *next,bool lock){
 factory_record_t old;int selected=-1;factory_result_t res=factory_store_read(io,mac,&old,&selected);
 if(res!=FACTORY_OK&&res!=FACTORY_UNPROVISIONED)return res;
 if(res==FACTORY_OK){
  if(factory_record_same_identity(&old,next)&&(!lock||old.locked))return FACTORY_OK;
  if(old.locked)return FACTORY_LOCKED;
  if(old.generation==UINT32_MAX)return FACTORY_GENERATION_EXHAUSTED;
  next->generation=old.generation+1;
 }else next->generation=1;
 next->locked=lock;
 uint8_t wire[FACTORY_RECORD_WIRE_SIZE],check[FACTORY_RECORD_WIRE_SIZE];size_t n=sizeof(check);
 res=factory_record_encode(next,wire,sizeof(wire));if(res!=FACTORY_OK)return res;
 int slot=selected==0?1:0;
 if(!io->write||!io->write(io->context,slot,wire,sizeof(wire)))return FACTORY_STORAGE_ERROR;
 if(!io->read(io->context,slot,check,&n)||n!=sizeof(wire)||memcmp(check,wire,n))return FACTORY_READBACK_ERROR;
 return FACTORY_OK;
}
factory_result_t factory_store_provision(const factory_store_io_t *io,const uint8_t mac[6],const factory_record_t *r){
 factory_result_t res=factory_record_validate(r);if(res!=FACTORY_OK)return res;
 if(!mac||memcmp(mac,r->mac,6))return FACTORY_MAC_MISMATCH;
 if(r->locked)return FACTORY_INVALID_ARGUMENT;
 factory_record_t next=*r;return save(io,mac,&next,false);
}
factory_result_t factory_store_lock(const factory_store_io_t *io,const uint8_t mac[6]){
 factory_record_t next;factory_result_t res=factory_store_read(io,mac,&next,NULL);
 return res==FACTORY_OK?save(io,mac,&next,true):res;
}
