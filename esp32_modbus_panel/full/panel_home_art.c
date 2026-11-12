#include "panel_home_art.h"
#include <string.h>

enum { ART_BLOCK_ROWS = 16, ART_MAX_WIDTH = 116 };
_Static_assert(LV_COLOR_DEPTH==16 && !LV_COLOR_16_SWAP,"Art uses native RGB565");
#include "panel_home_art.inc"

void panel_indexed_art_draw(lv_draw_ctx_t *ctx, const panel_indexed_art_t *art, int x, int y)
{
    if(art->width > ART_MAX_WIDTH)return;
    int left = LV_MAX(ctx->clip_area->x1 - x, 0);
    int right = LV_MIN(ctx->clip_area->x2 - x + 1, art->width);
    int first = LV_MAX(ctx->clip_area->y1 - y, 0);
    int last = LV_MIN(ctx->clip_area->y2 - y, (int)art->height - 1);
    if(left >= right || first > last)return;

    uint8_t indices[ART_MAX_WIDTH];
    unsigned block = (unsigned)first / ART_BLOCK_ROWS;
    const uint8_t *data = art->pixels + art->blocks[block];
    const lv_area_t *buffer=ctx->buf_area;
    int stride=lv_area_get_width(buffer);

    for(int row = (int)block * ART_BLOCK_ROWS; row <= last; row++) {
        for(unsigned col = 0; col < art->width;) {
            unsigned token = *data++;
            unsigned length = (token & 63U) + 1U;
            if(token < 64) {
                indices[col++] = (uint8_t)token;
                continue;
            }
            if(token < 128) {
                memcpy(indices + col, data, length);
                data += length;
            } else if(token < 192) {
                memset(indices + col, *data++, length);
            }
            /* The final token class reuses this span of the preceding row. */
            col += length;
        }
        if(row < first)continue;
        lv_color_t *pixels=(lv_color_t *)ctx->buf+(y+row-buffer->y1)*stride+(x+left-buffer->x1);
        for(int col = left; col < right;) {
            unsigned index = indices[col];
            int end = col + 1;
            while(end < right && indices[end] == index)end++;
            const uint8_t *entry=art->palette+3*index;
            unsigned opacity=entry[2];
            if(opacity>LV_OPA_MIN){
                lv_color_t color={.full=(uint16_t)(entry[0]|(entry[1]<<8))};
                if(opacity>=252)lv_color_fill(pixels,color,end-col);
                else for(int i=0;i<end-col;i++)pixels[i]=lv_color_mix(color,pixels[i],(lv_opa_t)opacity);
            }
            pixels+=end-col;
            col = end;
        }
    }
}

void panel_home_art_draw(lv_draw_ctx_t *ctx, int x, int y)
{
    panel_indexed_art_draw(ctx, &house_art, x, y);
}
