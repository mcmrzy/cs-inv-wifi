#pragma once
#include "factory_record.h"
#define FACTORY_PAIRING_WIRE_SIZE 48u
typedef struct { uint32_t generation; uint8_t mac[6]; char sn[17]; char pin[7]; } factory_pairing_record_t;
bool factory_pairing_values_valid(const char *sn,const char *pin);
factory_result_t factory_pairing_encode(const factory_pairing_record_t *,uint8_t *,size_t);
factory_result_t factory_pairing_decode(const uint8_t *,size_t,factory_pairing_record_t *);
factory_result_t factory_pairing_select(const uint8_t *,size_t,const uint8_t *,size_t,factory_pairing_record_t *,int *);
