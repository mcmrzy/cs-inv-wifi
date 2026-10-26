#include "direct_lcd_codec.h"

#include <string.h>

size_t direct_lcd_pack9(const uint8_t *input, size_t input_len, bool data,
                        uint8_t *output, size_t output_capacity)
{
    if (!input || !output || input_len == 0 || input_len > (SIZE_MAX - 7U) / 9U)
        return 0;
    const size_t bits = input_len * 9U;
    const size_t bytes = (bits + 7U) / 8U;
    if (bytes > output_capacity) return 0;
    memset(output, 0, bytes);
    for (size_t i = 0; i < input_len; ++i) {
        const uint16_t word = (uint16_t)((data ? 0x100U : 0U) | input[i]);
        for (unsigned bit = 0; bit < 9U; ++bit) {
            const size_t pos = i * 9U + bit;
            output[pos / 8U] |=
                (uint8_t)(((word >> (8U - bit)) & 1U) << (7U - pos % 8U));
        }
    }
    return bits;
}
