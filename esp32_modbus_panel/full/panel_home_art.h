#pragma once
#include "lvgl.h"

typedef struct {
    uint16_t width, height;
    const uint8_t *palette; /* RGB565 low byte, high byte, opacity. */
    const uint16_t *blocks;
    const uint8_t *pixels;
} panel_indexed_art_t;

void panel_indexed_art_draw(lv_draw_ctx_t *ctx, const panel_indexed_art_t *art, int x, int y);
void panel_home_art_draw(lv_draw_ctx_t *ctx, int x, int y);
