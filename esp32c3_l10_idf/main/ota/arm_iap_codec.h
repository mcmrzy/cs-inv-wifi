#ifndef ARM_IAP_CODEC_H
#define ARM_IAP_CODEC_H

#include <stddef.h>
#include <stdint.h>

#define ARM_IAP_HEADER_SIZE          16u
#define ARM_IAP_APP_PACKET_SIZE      1024u
#define ARM_IAP_FIRST_PAYLOAD_SIZE   (ARM_IAP_HEADER_SIZE + ARM_IAP_APP_PACKET_SIZE)
#define ARM_IAP_CRC_INIT             0xFFFFFFFFu

size_t arm_iap_transport_size(size_t image_size);
uint16_t arm_iap_packet_count(size_t image_size);
uint32_t arm_iap_crc32_update(const uint8_t *data, size_t len, uint32_t initial);
uint32_t arm_iap_image_crc(const uint8_t *image, size_t image_size);
void arm_iap_crypt_payload(uint8_t *data, size_t len, int encrypt);
size_t arm_iap_build_packet(const uint8_t *image, size_t image_size,
                            uint16_t packet_id, uint8_t *output,
                            size_t output_capacity);

#endif
