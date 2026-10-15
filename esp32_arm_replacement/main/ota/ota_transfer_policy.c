#include "ota_transfer_policy.h"

OtaTransferPolicy ota_transfer_policy(const OtaManifest *manifest)
{
    if (manifest && manifest->source == OTA_SOURCE_LOCAL &&
        manifest->target == OTA_TARGET_ESP) {
        return (OtaTransferPolicy){ .buffer_size = 496U, .sequential_erase = true };
    }
    if (manifest && manifest->source == OTA_SOURCE_MQTT) {
        /* HTTP RX + TLS RX already coexist here. Do not consume another
         * 4.5KB block before the next encrypted record is received. */
        return (OtaTransferPolicy){ .buffer_size = 1024U,
            .sequential_erase = manifest->target == OTA_TARGET_ESP };
    }
    return (OtaTransferPolicy){ .buffer_size = 4608U, .sequential_erase = false };
}
