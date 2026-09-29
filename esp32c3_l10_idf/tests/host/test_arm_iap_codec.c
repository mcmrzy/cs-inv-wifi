#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "arm_iap_codec.h"

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

int main(void) {
    /* 5000B: 8 字节对齐, 且跨两个 4096B 包 —— 同时覆盖首包(带 16B 头)与续包。
     * 包尺寸 4096 是 BMS 对齐修复后的值 (ARM_IAP_BMS_PACKET_SIZE), 见
     * cs-inv-esp-bms-ota-relay: 包 1024B 曾致 BMS 地址 4KB 步进、镜像稀疏错位。 */
    uint8_t image[5000];
    uint8_t packet[ARM_IAP_MAX_PAYLOAD_SIZE];
    uint8_t plain[ARM_IAP_MAX_PAYLOAD_SIZE];
    uint32_t crc = ARM_IAP_CRC_INIT;
    for (size_t i = 0; i < sizeof(image); ++i) image[i] = (uint8_t)(i * 37u + 11u);

    /* 尺寸语义: transport = image + 16B 头; 包数 = ceil(image / 4096);
     * 非 8 字节对齐的镜像直接拒绝 (boot 侧按 8 字节对齐补零约定)。 */
    CHECK(arm_iap_transport_size(sizeof(image)) == sizeof(image) + ARM_IAP_HEADER_SIZE);
    CHECK(arm_iap_packet_count(sizeof(image), ARM_IAP_BMS_PACKET_SIZE) == 2);
    CHECK(arm_iap_transport_size(2057) == 0);
    CHECK(arm_iap_packet_count(7, ARM_IAP_BMS_PACKET_SIZE) == 0);

    /* 首包: 16B 零头 + 前 4096B 应用数据, 整包加密 */
    size_t n = arm_iap_build_packet(image, sizeof(image), ARM_IAP_BMS_PACKET_SIZE,
                                    0, packet, sizeof(packet));
    CHECK(n == ARM_IAP_HEADER_SIZE + ARM_IAP_BMS_PACKET_SIZE);
    crc = arm_iap_crc32_update(packet, n, crc);
    memcpy(plain, packet, n);
    arm_iap_crypt_payload(plain, n, 0);
    for (size_t i = 0; i < ARM_IAP_HEADER_SIZE; ++i) CHECK(plain[i] == 0);
    CHECK(memcmp(plain + ARM_IAP_HEADER_SIZE, image, ARM_IAP_BMS_PACKET_SIZE) == 0);

    /* 续包: 直写余下 904B */
    n = arm_iap_build_packet(image, sizeof(image), ARM_IAP_BMS_PACKET_SIZE,
                             1, packet, sizeof(packet));
    CHECK(n == sizeof(image) - ARM_IAP_BMS_PACKET_SIZE);
    crc = arm_iap_crc32_update(packet, n, crc);
    memcpy(plain, packet, n);
    arm_iap_crypt_payload(plain, n, 0);
    CHECK(memcmp(plain, image + ARM_IAP_BMS_PACKET_SIZE, n) == 0);

    /* 越界包号: 0 */
    CHECK(arm_iap_build_packet(image, sizeof(image), ARM_IAP_BMS_PACKET_SIZE,
                               2, packet, sizeof(packet)) == 0);
    /* 分包链式 CRC 与整镜像 CRC 一致 */
    CHECK(crc == arm_iap_image_crc(image, sizeof(image), ARM_IAP_BMS_PACKET_SIZE));

    printf("arm_iap_codec: %s\n", failures ? "FAIL" : "PASS");
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
