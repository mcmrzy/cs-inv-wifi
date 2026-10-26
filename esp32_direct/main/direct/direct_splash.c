/* Boot splash: CSERGY / 辰烁科技 mark centred on screen with a fade-in and a fade-out.
 * All pixel work borrows the panel task's 320x16 DMA band and pushes through the
 * panel task's existing window()+tx() stream (direct_panel_show_band); nothing
 * here touches SPI init, so the panel stays the only owner of the bus. */
#include "direct_splash.h"
#include "direct_panel.h"
#include "direct_boot_logo.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define SPLASH_W 320
#define SPLASH_H 240
#define BAND_H 16
#define FADE_STEPS 8
#define FADE_OUT_STEPS 3
#define HOLD_MS 500

/* RGB565. Dark theme lightens the brand blue so it reads on black; light theme
 * uses the exact 5-6-5 of brand #1b3f8f (the task text quoted 0x19e7). */
#define SPLASH_BLUE_DARK  0x4c3fU /* #4a86ff */
#define SPLASH_BLUE_LIGHT 0x19f1U /* #1b3f8f */
#define SPLASH_WHITE      0xf7bfU
#define SPLASH_BLACK      0x0085U

static const char *TAG = "direct_panel";
extern const char direct_boot_logo_marker[];

typedef struct {
    uint16_t bg;
    uint16_t logo[2];
} splash_palette_t;

typedef struct {
    int logo_x, logo_y, logo_h, logo_w;
} splash_layout_t;

static uint16_t blend565(uint16_t bg,uint16_t ink,unsigned level)
{
    unsigned r = (((bg>>11)&31U)*(15-level)+((ink>>11)&31U)*level)/15;
    unsigned g = (((bg>>5)&63U)*(15-level)+((ink>>5)&63U)*level)/15;
    unsigned b = ((bg&31U)*(15-level)+(ink&31U)*level)/15;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

/* Build one band containing only the background and the stacked brand mark. */
static void render_band(const splash_layout_t *l, const splash_palette_t *p,
                        int y0, unsigned level, uint16_t *band)
{
    for (size_t i = 0; i < (size_t)SPLASH_W * BAND_H; ++i) band[i] = p->bg;
    if (level) {
        const uint16_t ink[2] = {blend565(p->bg,p->logo[0],level),
                                 blend565(p->bg,p->logo[1],level)};
        for (unsigned part = 0; part < 2; ++part) {
            const direct_boot_logo_part_t *q = &direct_boot_logo_parts[part];
            for (unsigned row = 0; row < q->height; ++row) {
                const int y = l->logo_y + (int)q->y_offset + (int)row - y0;
                if (y < 0 || y >= BAND_H) continue;
                for (unsigned col = 0; col < q->width; ++col) {
                    if (!(q->data[(size_t)row * q->stride + (col >> 3)] & (0x80 >> (col & 7)))) continue;
                    const int x = l->logo_x + (int)q->x_offset + (int)col;
                    if (x >= 0 && x < SPLASH_W) band[(size_t)y * SPLASH_W + x] = ink[part];
                }
            }
        }
    }

}

static void push_band(int y0, uint16_t *band)
{
    direct_panel_show_band(y0, band);
}

void direct_splash_run(bool light_theme)
{
    ESP_LOGW(TAG, "LCD_BOOT_SPLASH begin theme=%s", light_theme ? "light" : "dark");
    /* The caller is the panel task, before its normal render loop. Reuse its
     * scratch band rather than permanently reserving another 10,240 bytes. */
    uint16_t *band = direct_panel_band_buffer();
    const splash_palette_t dark = {SPLASH_BLACK, {SPLASH_BLUE_DARK, SPLASH_WHITE}};
    const splash_palette_t light = {SPLASH_WHITE, {SPLASH_BLUE_LIGHT, SPLASH_BLACK}};
    const splash_palette_t *p = light_theme ? &light : &dark;
    splash_layout_t l;
    l.logo_w = (int)direct_boot_logo_width;
    l.logo_h = (int)direct_boot_logo_height;
    l.logo_x = (SPLASH_W - l.logo_w) / 2;
    l.logo_y = (SPLASH_H - l.logo_h) / 2;
    const int logo_band_first = (l.logo_y / BAND_H) * BAND_H;
    const int logo_band_last = ((l.logo_y + l.logo_h - 1) / BAND_H) * BAND_H;
    const int64_t start = esp_timer_get_time();
    unsigned frames = 0;

    /* 1) theme background, so the panel never shows the wrong base colour. */
    for (int y = 0; y < SPLASH_H; y += BAND_H) {
        render_band(&l, p, y, 0, band);
        push_band(y, band);
        ++frames;
    }
    /* DISPON happens only after all 240 rows contain a known background. */
    direct_panel_enable_display();

    /* 2) fade the mark in over 8 brightness steps (1/15 .. 15/15). */
    static const unsigned levels[FADE_STEPS] = {1, 3, 5, 7, 9, 11, 13, 15};
    for (unsigned i = 0; i < FADE_STEPS; ++i) {
        for (int y = logo_band_first; y <= logo_band_last; y += BAND_H) {
            render_band(&l, p, y, levels[i], band);
            push_band(y, band);
            ++frames;
        }
    }

    /* 3) hold at full brightness. */
    ESP_LOGW(TAG, "LCD_BOOT_SPLASH hold");
    vTaskDelay(pdMS_TO_TICKS(HOLD_MS));

    /* 4) fade out over 3 steps. */
    ESP_LOGW(TAG, "LCD_BOOT_SPLASH fade_out");
    static const unsigned out[FADE_OUT_STEPS] = {10, 5, 0};
    for (unsigned i = 0; i < FADE_OUT_STEPS; ++i) {
        for (int y = logo_band_first; y <= logo_band_last; y += BAND_H) {
            render_band(&l, p, y, out[i], band);
            push_band(y, band);
            ++frames;
        }

    }

    const uint32_t total_ms = (uint32_t)((esp_timer_get_time() - start) / 1000);
    ESP_LOGW(TAG, "LCD_BOOT_SPLASH frames=%u total_ms=%u asset=%s",
             frames, (unsigned)total_ms, direct_boot_logo_marker);
}
