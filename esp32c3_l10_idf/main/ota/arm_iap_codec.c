#include "arm_iap_codec.h"

#include <string.h>

static const uint32_t fw_key[4] = {
    0xfe23213fu, 0xac89cd44u, 0x12bcfdccu, 0xede19348u
};

static uint32_t load32_le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void store32_le(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static void tea_encrypt(uint32_t block[2])
{
    uint32_t v0 = block[0], v1 = block[1], sum = 0;
    unsigned i;
    for (i = 0; i < 32; ++i) {
        sum += 0x9e3779b9u;
        v0 += ((v1 << 4) + fw_key[0]) ^ (v1 + sum) ^ ((v1 >> 5) + fw_key[1]);
        v1 += ((v0 << 4) + fw_key[2]) ^ (v0 + sum) ^ ((v0 >> 5) + fw_key[3]);
    }
    block[0] = v0;
    block[1] = v1;
}

static void tea_decrypt(uint32_t block[2])
{
    uint32_t v0 = block[0], v1 = block[1], sum = 0xc6ef3720u;
    unsigned i;
    for (i = 0; i < 32; ++i) {
        v1 -= ((v0 << 4) + fw_key[2]) ^ (v0 + sum) ^ ((v0 >> 5) + fw_key[3]);
        v0 -= ((v1 << 4) + fw_key[0]) ^ (v1 + sum) ^ ((v1 >> 5) + fw_key[1]);
        sum -= 0x9e3779b9u;
    }
    block[0] = v0;
    block[1] = v1;
}

size_t arm_iap_transport_size(size_t image_size)
{
    return image_size == 0 || (image_size & 7u) != 0 ? 0 : image_size + ARM_IAP_HEADER_SIZE;
}

static int valid_packet_size(size_t packet_size)
{
    return packet_size != 0 && packet_size <= ARM_IAP_MAX_PACKET_SIZE &&
           (packet_size & 7u) == 0;
}

uint16_t arm_iap_packet_count(size_t image_size, size_t packet_size)
{
    if (arm_iap_transport_size(image_size) == 0 || !valid_packet_size(packet_size)) return 0;
    size_t count = (image_size + packet_size - 1u) / packet_size;
    return count > UINT16_MAX ? 0 : (uint16_t)count;
}

uint32_t arm_iap_crc32_update(const uint8_t *data, size_t len, uint32_t crc)
{
    size_t i;
    unsigned bit;
    if (!data || (len & 3u) != 0) return 0;
    for (i = 0; i < len; i += 4) {
        crc ^= load32_le(data + i);
        for (bit = 0; bit < 32; ++bit) {
            crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04c11db7u : crc << 1;
        }
    }
    return crc;
}

void arm_iap_crypt_payload(uint8_t *data, size_t len, int encrypt)
{
    uint32_t previous[2] = {0, 0};
    size_t i;
    if (!data || (len & 7u) != 0) return;
    for (i = 0; i < len; i += 8) {
        uint32_t block[2] = {load32_le(data + i), load32_le(data + i + 4)};
        if (encrypt) {
            block[0] ^= previous[0];
            block[1] ^= previous[1];
            tea_encrypt(block);
            previous[0] = block[0];
            previous[1] = block[1];
        } else {
            uint32_t cipher[2] = {block[0], block[1]};
            tea_decrypt(block);
            block[0] ^= previous[0];
            block[1] ^= previous[1];
            previous[0] = cipher[0];
            previous[1] = cipher[1];
        }
        store32_le(data + i, block[0]);
        store32_le(data + i + 4, block[1]);
    }
}

size_t arm_iap_build_packet(const uint8_t *image, size_t image_size,
                            size_t packet_size, uint16_t packet_id, uint8_t *output,
                            size_t output_capacity)
{
    size_t offset, length;
    if (!image || !output || arm_iap_packet_count(image_size, packet_size) == 0) return 0;
    offset = (size_t)packet_id * packet_size;
    if (offset >= image_size) return 0;
    length = image_size - offset;
    if (length > packet_size) length = packet_size;
    if (packet_id == 0) {
        if (output_capacity < length + ARM_IAP_HEADER_SIZE) return 0;
        memset(output, 0, ARM_IAP_HEADER_SIZE);
        memcpy(output + ARM_IAP_HEADER_SIZE, image, length);
        length += ARM_IAP_HEADER_SIZE;
    } else {
        if (output_capacity < length) return 0;
        memcpy(output, image + offset, length);
    }
    arm_iap_crypt_payload(output, length, 1);
    return length;
}

uint32_t arm_iap_image_crc(const uint8_t *image, size_t image_size,
                           size_t packet_size)
{
    uint8_t packet[ARM_IAP_MAX_PAYLOAD_SIZE];
    uint16_t count = arm_iap_packet_count(image_size, packet_size), packet_id;
    uint32_t crc = ARM_IAP_CRC_INIT;
    if (count == 0) return 0;
    for (packet_id = 0; packet_id < count; ++packet_id) {
        size_t length = arm_iap_build_packet(image, image_size, packet_size, packet_id,
                                             packet, sizeof(packet));
        if (length == 0) return 0;
        crc = arm_iap_crc32_update(packet, length, crc);
    }
    return crc;
}
