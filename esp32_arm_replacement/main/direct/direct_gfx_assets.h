#ifndef DIRECT_GFX_ASSETS_H
#define DIRECT_GFX_ASSETS_H
#include <stdint.h>
/* Static native-size glyphs. bpp=4: MS nibble first coverage;
   bpp=0/1: legacy MSB-left monochrome. No runtime scaling. */
typedef struct { uint8_t width,height,stride,advance; const uint8_t *data; uint8_t bpp; } direct_gfx_glyph_t;
const direct_gfx_glyph_t *direct_gfx_glyph(uint32_t codepoint, int size);
int direct_gfx_has_glyph(uint32_t codepoint);
/* Consume one Unicode scalar. An invalid completed sequence returns U+FFFD;
   a bad/truncated byte advances once, so decoding always makes progress. */
uint32_t direct_gfx_utf8_next(const char **text);
#endif
