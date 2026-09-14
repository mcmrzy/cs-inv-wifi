#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "arm_iap_codec.h"

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

int main(void) {
    uint8_t image[2056];
    uint8_t packet[ARM_IAP_FIRST_PAYLOAD_SIZE];
    uint8_t plain[ARM_IAP_FIRST_PAYLOAD_SIZE];
    uint32_t crc = ARM_IAP_CRC_INIT;
    for (size_t i = 0; i < sizeof(image); ++i) image[i] = (uint8_t)(i * 37u + 11u);

    CHECK(arm_iap_transport_size(sizeof(image)) == sizeof(image) + ARM_IAP_HEADER_SIZE);
    CHECK(arm_iap_packet_count(sizeof(image)) == 3);

    size_t n = arm_iap_build_packet(image, sizeof(image), 0, packet, sizeof(packet));
    CHECK(n == ARM_IAP_FIRST_PAYLOAD_SIZE);
    crc = arm_iap_crc32_update(packet, n, crc);
    memcpy(plain, packet, n);
    arm_iap_crypt_payload(plain, n, 0);
    for (size_t i = 0; i < ARM_IAP_HEADER_SIZE; ++i) CHECK(plain[i] == 0);
    CHECK(memcmp(plain + ARM_IAP_HEADER_SIZE, image, 1024) == 0);

    n = arm_iap_build_packet(image, sizeof(image), 1, packet, sizeof(packet));
    CHECK(n == 1024);
    crc = arm_iap_crc32_update(packet, n, crc);
    memcpy(plain, packet, n);
    arm_iap_crypt_payload(plain, n, 0);
    CHECK(memcmp(plain, image + 1024, 1024) == 0);

    n = arm_iap_build_packet(image, sizeof(image), 2, packet, sizeof(packet));
    CHECK(n == 8);
    crc = arm_iap_crc32_update(packet, n, crc);
    memcpy(plain, packet, n);
    arm_iap_crypt_payload(plain, n, 0);
    CHECK(memcmp(plain, image + 2048, 8) == 0);
    CHECK(crc == arm_iap_image_crc(image, sizeof(image)));
    CHECK(arm_iap_build_packet(image, sizeof(image), 3, packet, sizeof(packet)) == 0);
    CHECK(arm_iap_packet_count(7) == 0);

    printf("arm_iap_codec: %s\n", failures ? "FAIL" : "PASS");
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
