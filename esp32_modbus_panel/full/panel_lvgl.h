#pragma once
#include "direct_ui.h"
#include "onboarding_qr.h"
#include "panel_ui.h"
#include "panel_weather.h"

typedef void (*panel_lvgl_flush_fn)(int x,int y,int width,int height,uint16_t *pixels);
enum { PANEL_SPLASH_DURATION_MS=1700 };
bool panel_lvgl_init(uint16_t *buffer,panel_lvgl_flush_fn flush,uint32_t now);
void panel_lvgl_splash(bool light);
void panel_lvgl_weather(const panel_weather_snapshot_t *);
void panel_lvgl_show_modbus(const panel_view_t *);
void panel_lvgl_show_local(const direct_ui_state_t *,const direct_ui_data_t *,
                           const direct_ui_view_t *,const onboarding_qr_t *,
                           const char *pin,bool has_wifi,bool bound);
void panel_lvgl_refresh(uint32_t now,bool redraw);
