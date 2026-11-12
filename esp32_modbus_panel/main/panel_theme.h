#pragma once
#include "direct_gfx.h"

#define PANEL_RGB(r,g,b) ((uint16_t)(((r)>>3)<<11 | ((g)>>2)<<5 | ((b)>>3)))

typedef struct {
    uint16_t background,surface,selected,text,muted,line,accent,warning,warning_surface;
} panel_theme_t;

static inline panel_theme_t panel_theme(bool light)
{
    if(light)return (panel_theme_t){
        .background=PANEL_RGB(247,246,240),.surface=PANEL_RGB(255,254,249),
        .selected=PANEL_RGB(220,237,226),.text=PANEL_RGB(32,38,34),
        .muted=PANEL_RGB(82,93,86),.line=PANEL_RGB(210,217,208),
        .accent=PANEL_RGB(24,100,74),.warning=PANEL_RGB(133,77,25),
        .warning_surface=PANEL_RGB(247,231,202),
    };
    return (panel_theme_t){
        .background=PANEL_RGB(22,24,23),.surface=PANEL_RGB(35,39,37),
        .selected=PANEL_RGB(43,66,55),.text=PANEL_RGB(241,243,237),
        .muted=PANEL_RGB(169,182,173),.line=PANEL_RGB(63,75,68),
        .accent=PANEL_RGB(135,220,174),.warning=PANEL_RGB(244,188,101),
        .warning_surface=PANEL_RGB(66,49,31),
    };
}

/* Four short steps, then a stationary frame. Timing is independent of SPI. */
static inline uint8_t panel_focus_frame(uint32_t elapsed)
{
    return elapsed<112U?(uint8_t)(4U-elapsed/28U):0;
}

/* Three-pixel rounded corners, clipped by the same strip primitive as text. */
static inline void panel_card(direct_gfx_t *g,int x,int y,int w,int h,uint16_t color)
{
    direct_gfx_rect(g,x,y+3,w,h-6,color);
    direct_gfx_rect(g,x+1,y+1,w-2,2,color);
    direct_gfx_rect(g,x+1,y+h-3,w-2,2,color);
    direct_gfx_rect(g,x+3,y,w-6,1,color);
    direct_gfx_rect(g,x+3,y+h-1,w-6,1,color);
}
