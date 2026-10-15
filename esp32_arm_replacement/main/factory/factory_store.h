#pragma once
#include "factory_record.h"
typedef struct {
 bool (*read)(void*,int,uint8_t*,size_t*); /* size=0 only when slot missing */
 bool (*write)(void*,int,const uint8_t*,size_t); /* commit must finish before return */
 void *context;
} factory_store_io_t;
factory_result_t factory_store_read(const factory_store_io_t*,const uint8_t mac[6],factory_record_t*,int*);
factory_result_t factory_store_provision(const factory_store_io_t*,const uint8_t mac[6],const factory_record_t*);
factory_result_t factory_store_lock(const factory_store_io_t*,const uint8_t mac[6]);
