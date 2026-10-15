#ifndef OTA_TRANSFER_POLICY_H
#define OTA_TRANSFER_POLICY_H

#include <stdbool.h>
#include <stddef.h>

#include "ota_service.h"

typedef struct {
    size_t buffer_size;
    bool sequential_erase;
} OtaTransferPolicy;

OtaTransferPolicy ota_transfer_policy(const OtaManifest *manifest);

#endif
