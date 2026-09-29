#include "ota_transfer_policy.h"

OtaTransferPolicy ota_transfer_policy(const OtaManifest *manifest)
{
    if (manifest && manifest->source == OTA_SOURCE_LOCAL &&
        manifest->target == OTA_TARGET_ESP) {
        return (OtaTransferPolicy){ .buffer_size = 496U, .sequential_erase = true };
    }
    return (OtaTransferPolicy){ .buffer_size = 4608U, .sequential_erase = false };
}
