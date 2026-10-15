#ifndef DIRECT_LCD_CODEC_H
#define DIRECT_LCD_CODEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static inline bool direct_lcd_cs_toggles(int pin, bool fixed_low)
{
    return pin >= 0 && !fixed_low;
}

/* Constant-low CS must never be owned/toggled by the SPI peripheral. */
static inline int direct_lcd_spi_cs(int pin, bool fixed_low)
{
    return direct_lcd_cs_toggles(pin,fixed_low) ? pin : -1;
}

static inline int direct_lcd_spi_mode(bool reset_first)
{
    return reset_first ? 3 : 0;
}

/* ST7789 3-line SPI: each transfer byte is prefixed by a command/data bit.
 * Returns the number of valid output bits, or zero for invalid arguments. */
size_t direct_lcd_pack9(const uint8_t *input, size_t input_len, bool data,
                        uint8_t *output, size_t output_capacity);

#endif
