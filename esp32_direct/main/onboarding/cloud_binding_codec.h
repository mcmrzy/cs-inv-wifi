#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "cloud_binding.h"
bool cloud_binding_verify(const char *sn,const char *state,const char *revision,
 const char *nonce,const char *signature,const char *public_key,uint64_t *out_revision);
