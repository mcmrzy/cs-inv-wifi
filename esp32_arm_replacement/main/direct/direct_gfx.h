#ifndef DIRECT_GFX_H
#define DIRECT_GFX_H
#include <stdint.h>
#include <stdbool.h>
typedef struct { uint16_t *pixels; int width, height, y0; bool theme_light, language_zh; } direct_gfx_t;
typedef enum { DIRECT_ICON_GRID, DIRECT_ICON_SOLAR, DIRECT_ICON_INVERTER,
 DIRECT_ICON_LOAD, DIRECT_ICON_BATTERY, DIRECT_ICON_GENERATOR,
 DIRECT_ICON_WARNING } direct_icon_t;
void direct_gfx_begin(direct_gfx_t *, uint16_t *, int, int, int, uint16_t);
void direct_gfx_rect(direct_gfx_t *, int, int, int, int, uint16_t);
void direct_gfx_line(direct_gfx_t *, int, int, int, int, uint16_t);
void direct_gfx_text(direct_gfx_t *, int, int, const char *, int, uint16_t);
int direct_gfx_text_width(const char *, int);
void direct_gfx_icon(direct_gfx_t *, direct_icon_t, int, int, uint16_t);
#endif
