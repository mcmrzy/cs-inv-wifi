#include "ota_transfer_policy.h"

#include <stdio.h>

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "failed at line %d: %s\n", __LINE__, #expr); \
        return 1; \
    } \
} while (0)

int main(void)
{
    OtaManifest manifest = { .source = OTA_SOURCE_LOCAL, .target = OTA_TARGET_ESP };
    OtaTransferPolicy policy = ota_transfer_policy(&manifest);
    CHECK(policy.buffer_size >= 496U);
    CHECK(policy.sequential_erase);

    manifest.source = OTA_SOURCE_MQTT;
    policy = ota_transfer_policy(&manifest);
    CHECK(policy.buffer_size == 4608U);
    CHECK(!policy.sequential_erase);

    manifest.source = OTA_SOURCE_LOCAL;
    manifest.target = OTA_TARGET_ARM;
    policy = ota_transfer_policy(&manifest);
    CHECK(policy.buffer_size == 4608U);
    CHECK(!policy.sequential_erase);
    return 0;
}
