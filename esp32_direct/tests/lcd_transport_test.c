#include <assert.h>
#include <stdio.h>
#include "direct_lcd_codec.h"
int main(void)
{
    assert(direct_lcd_spi_cs(4,true)==-1);
    assert(direct_lcd_spi_cs(-1,false)==-1);
    assert(direct_lcd_spi_cs(4,false)==4);
    assert(direct_lcd_spi_mode(true)==3);
    assert(direct_lcd_spi_mode(false)==0);
    puts("PASS: hardware SPI preserves fixed-low CS and reset-first mode3");
}
