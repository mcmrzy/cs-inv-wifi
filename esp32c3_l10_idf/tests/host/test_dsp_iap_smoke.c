/* Host-side compile smoke check for Collector IAP dual-target API surface.
 * Not a full ESP-IDF unit test — verifies symbols/type shape used by ota_service. */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "arm_iap_codec.h"

int main(void)
{
    /* Same FwKey layout as arm_iap_codec.c / ARM Update.c */
    uint8_t image[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    uint8_t packet[ARM_IAP_FIRST_PAYLOAD_SIZE];
    size_t n = arm_iap_build_packet(image, sizeof(image),
                                    ARM_IAP_LEGACY_PACKET_SIZE,
                                    0, packet, sizeof(packet));
    assert(n == ARM_IAP_HEADER_SIZE + 8);
    assert(packet[0] != 1); /* encrypted, not plaintext header zeros+image */

    uint32_t crc = arm_iap_image_crc(image, sizeof(image),
                                     ARM_IAP_LEGACY_PACKET_SIZE);
    assert(crc != 0);

    /* DSP packets use the same codec: only Collector param1 differs. */
    assert(arm_iap_packet_count(sizeof(image), ARM_IAP_LEGACY_PACKET_SIZE) == 1);
    assert(arm_iap_transport_size(sizeof(image)) == 8 + ARM_IAP_HEADER_SIZE);

    /* Regression: ARM/DSP bootloader receives at most a 1024-byte application
     * chunk (FRAMLEN=1088), while BMS uses its native 4096-byte flash block. */
    assert(arm_iap_packet_count(1368, ARM_IAP_LEGACY_PACKET_SIZE) == 2);
    assert(arm_iap_packet_count(1368, ARM_IAP_BMS_PACKET_SIZE) == 1);

    uint8_t dsp_image[1368];
    uint8_t dsp_packet[ARM_IAP_MAX_PAYLOAD_SIZE];
    memset(dsp_image, 0x5a, sizeof(dsp_image));
    assert(arm_iap_build_packet(dsp_image, sizeof(dsp_image),
                                ARM_IAP_LEGACY_PACKET_SIZE,
                                0, dsp_packet, sizeof(dsp_packet)) ==
           ARM_IAP_HEADER_SIZE + ARM_IAP_LEGACY_PACKET_SIZE);
    assert(arm_iap_build_packet(dsp_image, sizeof(dsp_image),
                                ARM_IAP_LEGACY_PACKET_SIZE,
                                1, dsp_packet, sizeof(dsp_packet)) == 344);

    printf("dsp_iap_smoke ok crc=0x%08lx\n", (unsigned long)crc);
    return 0;
}
