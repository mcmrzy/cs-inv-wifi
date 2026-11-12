#pragma once
#define LV_CONF_H
#ifdef PANEL_LVGL_HOST_TEST
void panel_lvgl_host_assert(const char *file,int line);
#define LV_ASSERT_HANDLER panel_lvgl_host_assert(__FILE__,__LINE__);
#endif
#define LV_COLOR_DEPTH 16
#define LV_COLOR_16_SWAP 0
#define LV_MEM_SIZE (20U * 1024U)
/* All product text uses the board's licensed subset. Avoid linking a second,
 * unused built-in Latin font through LVGL's default-style fallback. */
#define LV_FONT_MONTSERRAT_14 0
#define LV_FONT_CUSTOM_DECLARE LV_FONT_DECLARE(panel_lvgl_body_font)
#define LV_FONT_DEFAULT &panel_lvgl_body_font
/* Product scenes use solid rectangles and orthogonal wires. No shadow,
 * gradient, transformed image, or complex mask engine is needed. */
#define LV_DRAW_COMPLEX 0
#define LV_DISP_DEF_REFR_PERIOD 20
#define LV_USE_THEME_DEFAULT 0
#define LV_USE_THEME_BASIC 0
#define LV_USE_THEME_MONO 0
#define LV_USE_LOG 0
#define LV_USE_LABEL 1
#define LV_USE_LINE 0
#define LV_USE_ARC 0
#define LV_USE_BAR 0
#define LV_USE_BTN 0
#define LV_USE_BTNMATRIX 0
#define LV_USE_CANVAS 0
#define LV_USE_CHECKBOX 0
#define LV_USE_DROPDOWN 0
#define LV_USE_IMG 0
#define LV_USE_ROLLER 0
#define LV_USE_SLIDER 0
#define LV_USE_SWITCH 0
#define LV_USE_TABLE 0
#define LV_USE_TEXTAREA 0
#define LV_USE_ANIMIMG 0
#define LV_USE_CALENDAR 0
#define LV_USE_CHART 0
#define LV_USE_COLORWHEEL 0
#define LV_USE_IMGBTN 0
#define LV_USE_KEYBOARD 0
#define LV_USE_LED 0
#define LV_USE_LIST 0
#define LV_USE_MENU 0
#define LV_USE_METER 0
#define LV_USE_MSGBOX 0
#define LV_USE_SPAN 0
#define LV_USE_SPINBOX 0
#define LV_USE_SPINNER 0
#define LV_USE_TABVIEW 0
#define LV_USE_TILEVIEW 0
#define LV_USE_WIN 0
#define LV_USE_FLEX 0
#define LV_USE_GRID 0
