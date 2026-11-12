#pragma once
#include "panel_ui.h"
#include "esp_err.h"
/* Hold RESET and backlight inactive before any application SPI clocks. */
void panel_lcd_prepare(void);
esp_err_t panel_lcd_init(void);
esp_err_t panel_lcd_render(const panel_view_t *view);
