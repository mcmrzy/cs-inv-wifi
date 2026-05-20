// 测试 CRC-16 MODBUS 计算
#include <stdio.h>
#include <stdint.h>
#include <string.h>

static uint16_t crc16_modbus(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 0x0001) {
                crc = (crc >> 1) ^ 0xA001;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

static char value_to_char(uint8_t v) {
    if (v < 10) return '0' + v;
    return 'A' + (v - 10);
}

int main() {
    const char* prefix = "H1DEA0012A00001";
    uint16_t crc = crc16_modbus((const uint8_t*)prefix, strlen(prefix));
    uint8_t lo = crc & 0xFF;
    uint8_t mod = lo % 36;
    char check = value_to_char(mod);
    
    printf("前缀: %s\n", prefix);
    printf("CRC-16: 0x%04X\n", crc);
    printf("低8位: 0x%02X (%d)\n", lo, lo);
    printf("mod 36: %d\n", mod);
    printf("校验位: %c\n", check);
    printf("完整SN: %s%c\n", prefix, check);
    
    return 0;
}
