#ifndef ARM_IAP_CODEC_H
#define ARM_IAP_CODEC_H

#include <stddef.h>
#include <stdint.h>

#define ARM_IAP_HEADER_SIZE          16u
/* ARM/DSP 的 Collector bootloader 固定按 1024B 分包，接收帧上限为 1088B。
 * BMS 则按 4096B flash block 编址，必须保持 4096B。二者不能共用一个包长。 */
#define ARM_IAP_LEGACY_PACKET_SIZE   1024u
#define ARM_IAP_BMS_PACKET_SIZE      4096u
#define ARM_IAP_MAX_PACKET_SIZE      ARM_IAP_BMS_PACKET_SIZE
#define ARM_IAP_MAX_PAYLOAD_SIZE     (ARM_IAP_HEADER_SIZE + ARM_IAP_MAX_PACKET_SIZE)
/* 兼容仅用于容量声明的旧名称；实际分包必须显式传入 packet_size。 */
#define ARM_IAP_FIRST_PAYLOAD_SIZE   ARM_IAP_MAX_PAYLOAD_SIZE
#define ARM_IAP_CRC_INIT             0xFFFFFFFFu

size_t arm_iap_transport_size(size_t image_size);
uint16_t arm_iap_packet_count(size_t image_size, size_t packet_size);
uint32_t arm_iap_crc32_update(const uint8_t *data, size_t len, uint32_t initial);
uint32_t arm_iap_image_crc(const uint8_t *image, size_t image_size,
                           size_t packet_size);
void arm_iap_crypt_payload(uint8_t *data, size_t len, int encrypt);
size_t arm_iap_build_packet(const uint8_t *image, size_t image_size,
                            size_t packet_size, uint16_t packet_id, uint8_t *output,
                            size_t output_capacity);

#endif
