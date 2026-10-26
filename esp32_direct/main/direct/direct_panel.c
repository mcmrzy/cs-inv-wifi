#include "direct_panel.h"
#include "direct_splash.h"
#include "arm_poll.h"
#include "direct_lcd_codec.h"
#include "direct_lcd_recovery.h"

#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#if CONFIG_DIRECT_LCD_ISOLATION_TEST
#include "esp_ota_ops.h"
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifndef CONFIG_DIRECT_LCD_CS_FIXED_LOW
#define CONFIG_DIRECT_LCD_CS_FIXED_LOW 0
#endif
#ifndef CONFIG_DIRECT_LCD_NOCS_BOOT
#define CONFIG_DIRECT_LCD_NOCS_BOOT 0
#endif
#ifndef CONFIG_DIRECT_LCD_GPIO_SPI
#define CONFIG_DIRECT_LCD_GPIO_SPI 0
#endif

#define LCD_W 320
#define LCD_H 240
#define LCD_HOST SPI2_HOST
#define RGB565(r,g,b) (uint16_t)((((r)&31U)<<11)|(((g)&63U)<<5)|((b)&31U))
#define BLACK 0x0000U
#define WHITE 0xffffU
#define CYAN  0x07ffU
#define AMBER 0xffc0U
#define RED   0xf800U

static const char *TAG = "direct_panel";
static atomic_uint s_lcd_tx_errors;
static atomic_bool s_first_frame_done;
static atomic_uint s_recovery_error_baseline;
static bool s_panel_initialized;
static atomic_uint s_panel_tick;
typedef enum {
    PANEL_UNINITIALIZED, PANEL_INIT, PANEL_TASK_WAIT, PANEL_LOCAL,
    PANEL_ENERGY, PANEL_SPLASH, PANEL_SNAPSHOT, PANEL_INPUT,
    PANEL_RENDER, PANEL_WAIT, PANEL_FAILED
} panel_stage_t;
static atomic_uint s_panel_stage, s_panel_stage_ms, s_panel_row;
static atomic_bool s_spi_active;
static atomic_uint s_spi_started_ms, s_spi_bytes, s_spi_transfers;
static atomic_uint s_panel_stack_free;
static atomic_uint s_panel_page, s_panel_key_events, s_panel_frames;
#if !CONFIG_DIRECT_LCD_GPIO_SPI
static spi_device_handle_t s_lcd;
#endif



#if CONFIG_DIRECT_BOOT_DIAGNOSTICS && !CONFIG_DIRECT_LCD_NOCS_BOOT
static bool panel_gpio_probe(int pin, const char *signal)
{
    if (pin < 0) return true;
    ESP_ERROR_CHECK(gpio_input_enable(pin));
    ESP_ERROR_CHECK(gpio_set_level(pin, 0));
    esp_rom_delay_us(20);
    const int low = gpio_get_level(pin);
    ESP_ERROR_CHECK(gpio_set_level(pin, 1));
    esp_rom_delay_us(20);
    const int high = gpio_get_level(pin);
    ESP_LOGI(TAG, "LCD GPIO test %s GPIO%d: set0/read%d set1/read%d",
             signal, pin, low, high);
    if (low != 0 || high != 1) {
        ESP_LOGE(TAG, "LCD %s cannot reach requested level; check pull resistors and wiring", signal);
        return false;
    }
    return true;
}
#endif

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void panel_stage(panel_stage_t stage)
{
    atomic_store(&s_panel_stage_ms, now_ms());
    atomic_store(&s_panel_stage, stage);
}

void direct_panel_diagnostic_tick(void)
{
    static uint32_t next_report_ms;
    const uint32_t now = now_ms();
    if ((int32_t)(now - next_report_ms) < 0) return;
    const bool first = atomic_load(&s_first_frame_done);
    const uint32_t age = now - atomic_load(&s_panel_tick);
    const unsigned errors = atomic_load(&s_lcd_tx_errors);
    next_report_ms = now + (first && age <= 2000U && !errors ? 30000U : 5000U);
    static const char *const names[] = {
        "NOT_STARTED", "INIT", "TASK_WAIT", "LOCAL_SETTINGS", "ENERGY",
        "SPLASH", "SNAPSHOT", "INPUT", "RENDER", "WAIT", "FAILED"
    };
    const unsigned stage = atomic_load(&s_panel_stage);
    const bool spi = atomic_load(&s_spi_active);
    ESP_LOGW(TAG, "LCD_DIAG stage=%s stage_age=%lu first=%u loop_age=%lu "
             "spi=%u spi_age=%lu bytes=%u row=%u tx=%u errors=%u stack_free=%u "
             "page=%u keys=%u frames=%u",
             stage < sizeof(names)/sizeof(names[0]) ? names[stage] : "UNKNOWN",
             (unsigned long)(now - atomic_load(&s_panel_stage_ms)), first,
             (unsigned long)age, spi,
             (unsigned long)(spi ? now - atomic_load(&s_spi_started_ms) : 0),
             atomic_load(&s_spi_bytes), atomic_load(&s_panel_row),
             atomic_load(&s_spi_transfers), errors, atomic_load(&s_panel_stack_free),
             atomic_load(&s_panel_page), atomic_load(&s_panel_key_events),
             atomic_load(&s_panel_frames));
}

static uint64_t pin_mask(int pin)
{
    return pin >= 0 && pin < 64 ? (1ULL << (unsigned)pin) : 0;
}

void direct_panel_prepare_boot(void)
{
#if CONFIG_DIRECT_LCD_NOCS_BOOT
    /* Reset must be asserted before any clock-pin mux/level change. */
    ESP_ERROR_CHECK(gpio_set_level(CONFIG_DIRECT_LCD_RST_PIN, 0));
    gpio_config_t reset = {
        .pin_bit_mask = pin_mask(CONFIG_DIRECT_LCD_RST_PIN),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&reset));
#if CONFIG_DIRECT_LCD_CS_FIXED_LOW
    ESP_ERROR_CHECK(gpio_set_level(CONFIG_DIRECT_LCD_CS_PIN,0));
    gpio_config_t cs={.pin_bit_mask=pin_mask(CONFIG_DIRECT_LCD_CS_PIN),.mode=GPIO_MODE_OUTPUT};
    ESP_ERROR_CHECK(gpio_config(&cs));
    ESP_LOGI(TAG,"LCD CS_FIXED_LOW: GPIO%d output low, never deselected",CONFIG_DIRECT_LCD_CS_PIN);
#endif
    ESP_ERROR_CHECK(gpio_set_level(CONFIG_DIRECT_LCD_SCLK_PIN, 1));
    ESP_ERROR_CHECK(gpio_set_level(CONFIG_DIRECT_LCD_MOSI_PIN, 0));
    ESP_ERROR_CHECK(gpio_set_level(CONFIG_DIRECT_LCD_DC_PIN, 1));
    gpio_config_t bus = {
        .pin_bit_mask = pin_mask(CONFIG_DIRECT_LCD_SCLK_PIN) |
                        pin_mask(CONFIG_DIRECT_LCD_MOSI_PIN) |
                        pin_mask(CONFIG_DIRECT_LCD_DC_PIN),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&bus));
    ESP_LOGI(TAG, "LCD NO_CS_BOOT: RESET held low, SCL high, write-only mode3");
#endif
}

static void tx(bool data, const void *bytes, size_t count)
{
    atomic_store(&s_spi_bytes, (unsigned)count);
    atomic_store(&s_spi_started_ms, now_ms());
    atomic_store(&s_spi_active, true);
#if CONFIG_DIRECT_LCD_GPIO_SPI
    const uint8_t *src = bytes;
    if (direct_lcd_cs_toggles(CONFIG_DIRECT_LCD_CS_PIN,CONFIG_DIRECT_LCD_CS_FIXED_LOW))
        gpio_set_level(CONFIG_DIRECT_LCD_CS_PIN, 0);
    gpio_set_level(CONFIG_DIRECT_LCD_DC_PIN, data);
    esp_rom_delay_us(2);
    for (size_t i = 0; i < count; ++i) {
        for (unsigned bit = 0; bit < 8; ++bit) {
#if CONFIG_DIRECT_LCD_NOCS_BOOT
            gpio_set_level(CONFIG_DIRECT_LCD_SCLK_PIN, 0);
#endif
            gpio_set_level(CONFIG_DIRECT_LCD_MOSI_PIN,
                           (src[i] >> (7U - bit)) & 1U);
            esp_rom_delay_us(2);
            gpio_set_level(CONFIG_DIRECT_LCD_SCLK_PIN, 1);
            esp_rom_delay_us(2);
#if !CONFIG_DIRECT_LCD_NOCS_BOOT
            gpio_set_level(CONFIG_DIRECT_LCD_SCLK_PIN, 0);
#endif
        }
    }
    if (direct_lcd_cs_toggles(CONFIG_DIRECT_LCD_CS_PIN,CONFIG_DIRECT_LCD_CS_FIXED_LOW))
        gpio_set_level(CONFIG_DIRECT_LCD_CS_PIN, 1);
    /* Pixel batches can take tens of milliseconds; let the idle task run. */
    if (count >= 64) vTaskDelay(1);
#else
#if CONFIG_DIRECT_LCD_3WIRE
    uint8_t packed[(512U * 9U + 7U) / 8U];
    const size_t bits = direct_lcd_pack9(bytes, count, data,
                                         packed, sizeof(packed));
    if (bits == 0) {
        atomic_store(&s_spi_active, false);
        ESP_LOGE(TAG, "LCD 9-bit transfer too large: %u", (unsigned)count);
        return;
    }
    spi_transaction_t t = {.length = bits, .tx_buffer = packed};
#else
    gpio_set_level(CONFIG_DIRECT_LCD_DC_PIN, data);
    esp_rom_delay_us(2);
    spi_transaction_t t = {.length = count * 8U, .tx_buffer = bytes};
#endif
    esp_err_t err = spi_device_polling_transmit(s_lcd, &t);
    if (err != ESP_OK) {
        s_lcd_tx_errors++;
        ESP_LOGE(TAG, "LCD SPI: %s", esp_err_to_name(err));
    }
#endif
    atomic_store(&s_spi_active, false);
    atomic_fetch_add(&s_spi_transfers, 1);
}

static void command(uint8_t c) { tx(false, &c, 1); }
static void data8(uint8_t v) { tx(true, &v, 1); }

#if CONFIG_DIRECT_LCD_READBACK_TERMINAL
/* ST7789P3 serial-interface I: one byte of MADCTL read data, no dummy
 * cycle. SDA must become Hi-Z before the final command falling edge.
 * With fixed CS this is terminal. With GPIO CS, deselect before restoring
 * SDA output so the display releases its return-data driver first. */
static uint8_t panel_terminal_read8(uint8_t cmd)
{
    if (CONFIG_DIRECT_LCD_CS_PIN >= 0)
        gpio_set_level(CONFIG_DIRECT_LCD_CS_PIN, 0);
    gpio_set_level(CONFIG_DIRECT_LCD_DC_PIN, 0);
    esp_rom_delay_us(2);
    for (unsigned bit = 0; bit < 8; ++bit) {
        gpio_set_level(CONFIG_DIRECT_LCD_MOSI_PIN,
                       (cmd >> (7U - bit)) & 1U);
        esp_rom_delay_us(2);
        gpio_set_level(CONFIG_DIRECT_LCD_SCLK_PIN, 1);
        esp_rom_delay_us(2);
        if (bit == 7) {
            ESP_ERROR_CHECK(gpio_set_direction(CONFIG_DIRECT_LCD_MOSI_PIN,
                                                GPIO_MODE_INPUT));
            ESP_ERROR_CHECK(gpio_set_pull_mode(CONFIG_DIRECT_LCD_MOSI_PIN,
                                               GPIO_FLOATING));
            gpio_set_level(CONFIG_DIRECT_LCD_DC_PIN, 1);
        }
        gpio_set_level(CONFIG_DIRECT_LCD_SCLK_PIN, 0);
    }
    uint8_t result = 0;
    for (unsigned bit = 0; bit < 8; ++bit) {
        esp_rom_delay_us(2);
        gpio_set_level(CONFIG_DIRECT_LCD_SCLK_PIN, 1);
        esp_rom_delay_us(2);
        result = (uint8_t)((result << 1) |
            (unsigned)gpio_get_level(CONFIG_DIRECT_LCD_MOSI_PIN));
        gpio_set_level(CONFIG_DIRECT_LCD_SCLK_PIN, 0);
    }
    if (CONFIG_DIRECT_LCD_CS_PIN >= 0) {
        gpio_set_level(CONFIG_DIRECT_LCD_CS_PIN, 1);
        esp_rom_delay_us(5);
        gpio_set_level(CONFIG_DIRECT_LCD_MOSI_PIN, 0);
        ESP_ERROR_CHECK(gpio_set_direction(CONFIG_DIRECT_LCD_MOSI_PIN,
                                            GPIO_MODE_OUTPUT));
    }
    return result;
}
#endif

/* Compare factory defaults with the supplied ARM power/gamma profile. */
static void panel_register_init(void)
{
    command(0x01); /* SWRESET, after the hardware reset has settled. */
    vTaskDelay(pdMS_TO_TICKS(150));
    command(0x28); /* Keep scanout off until the first complete frame is written. */
    command(0x11);
    vTaskDelay(pdMS_TO_TICKS(120));
#if CONFIG_DIRECT_LCD_MINIMAL_INIT
    /* Keep reset-loaded analog defaults. This is not an optical calibration
     * and cannot eliminate the module's TN viewing-angle dependence. */
    command(0x36); data8(0x70); /* 320x240 landscape, matching the ARM. */
    command(0x3a); data8(0x55); /* RGB565 for MCU/RGB interfaces. */
    command(0xb0); data8(0x00); data8(0xf0); /* MCU RAM, big endian. */
    command(0x13); /* Normal display mode. */
    command(0x20); /* Inversion off, matching this TN module's ARM profile. */
    ESP_LOGI(TAG, "LCD MINIMAL_INIT: factory power/gamma, COLMOD=55 RAMCTRL=00/F0, CS=%d",
             CONFIG_DIRECT_LCD_CS_PIN);
#else
#if CONFIG_DIRECT_LCD_GPIO_SPI
    command(0x13); /* NORON: explicitly leave partial mode. */
#endif
    command(0x36); data8(0x70);
    command(0x35); data8(0x00);
    static const uint8_t porch[] = {0x0c,0x0c,0x00,0x33,0x33};
    command(0xb2); tx(true, porch, sizeof(porch));
    command(0x3a); data8(0x55); /* Explicit RGB565 for both interfaces. */
    command(0xb0); data8(0x00); data8(0xf0); /* Preserve known big-endian RAM path. */
    command(0x13); /* Normal display mode. */
    command(0xb7); data8(0x65);
    command(0xbb); data8(0x1d);
    command(0xc0); data8(0x2c);
    command(0xc2); data8(0x01);
    command(0xc3); data8(0x13);
    command(0xc4); data8(0x20);
    command(0xc6); data8(0x0f);
    command(0xd0); data8(0xa4); data8(0xa1);
    command(0xd6); data8(0xa1);
    static const uint8_t gamma_pos[] =
        {0xf0,0x06,0x12,0x13,0x14,0x2d,0x37,0x44,0x48,0x08,0x10,0x0f,0x17,0x1a};
    static const uint8_t gamma_neg[] =
        {0xf0,0x04,0x0d,0x0c,0x0c,0x27,0x38,0x54,0x48,0x3d,0x1a,0x1a,0x1c,0x1f};
    command(0xe0); tx(true, gamma_pos, sizeof(gamma_pos));
    command(0xe1); tx(true, gamma_neg, sizeof(gamma_neg));
    command(0x20);
    ESP_LOGI(TAG,"LCD ARM_PROFILE: original power/gamma B7=65 BB=1D C3=13 C4=20, RGB565=55 inversion=OFF");
#endif
}

static void window(int x, int y, int w, int h)
{
    uint8_t b[4];
    command(0x2a);
    b[0] = (uint8_t)(x >> 8); b[1] = (uint8_t)x;
    b[2] = (uint8_t)((x + w - 1) >> 8); b[3] = (uint8_t)(x + w - 1);
    tx(true, b, 4);
    command(0x2b);
    b[0] = (uint8_t)(y >> 8); b[1] = (uint8_t)y;
    b[2] = (uint8_t)((y + h - 1) >> 8); b[3] = (uint8_t)(y + h - 1);
    tx(true, b, 4);
    command(0x2c);
}

static void fill_rect(int x, int y, int w, int h, uint16_t color)
{
    if (x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > LCD_W || y + h > LCD_H)
        return;
    uint8_t pixels[512];
    for (size_t i = 0; i < sizeof(pixels); i += 2) {
        pixels[i] = (uint8_t)(color >> 8);
        pixels[i + 1] = (uint8_t)color;
    }
    window(x, y, w, h);
    size_t left = (size_t)w * (size_t)h;
    while (left) {
        const size_t batch = left < 256U ? left : 256U;
        tx(true, pixels, batch * 2U);
        left -= batch;
    }
}

/* Called at startup and later only by the existing panel owner task. */
static bool panel_hardware_initialize(void)
{
    const unsigned errors_before=atomic_load(&s_lcd_tx_errors);
    const int rst_pin=CONFIG_DIRECT_LCD_RST_PIN;
    if(rst_pin>=0){
        gpio_set_level(rst_pin,0);
        ESP_LOGW(TAG,"LCD_RESET hold_low_ms=20");
        vTaskDelay(pdMS_TO_TICKS(20));
#if CONFIG_DIRECT_LCD_NOCS_BOOT && !CONFIG_DIRECT_LCD_GPIO_SPI
        /* Apply IDF's lazy mode configuration while RESET masks the clocks. */
        command(0x00);
        if(atomic_load(&s_lcd_tx_errors)!=errors_before)goto failed;
        ESP_LOGW(TAG,"LCD_SPI_PRIMED: hardware transaction completed under RESET");
#endif
        gpio_set_level(rst_pin,1);
        vTaskDelay(pdMS_TO_TICKS(CONFIG_DIRECT_LCD_NOCS_BOOT ? 150 : 120));
    }
    panel_register_init();
    fill_rect(0,0,LCD_W,LCD_H,BLACK);
    if(atomic_load(&s_lcd_tx_errors)!=errors_before)goto failed;
    return true;
failed:
    if(rst_pin>=0)gpio_set_level(rst_pin,0);
    panel_stage(PANEL_FAILED);
    ESP_LOGE(TAG,"LCD_INIT failed; recovery owner remains available");
    return false;
}

#include "direct_panel_runtime.inc"

uint16_t *direct_panel_band_buffer(void)
{
    return s_strip_pixels;
}

void direct_panel_show_band(int y, uint16_t *pixels)
{
    if (!pixels || y < 0 || y + 16 > LCD_H) return;
    atomic_store(&s_panel_row, (unsigned)y);
    /* The panel RAM path is big endian; swap in place like panel_render does. */
    for (size_t i = 0; i < (size_t)LCD_W * 16; ++i)
        pixels[i] = (uint16_t)((pixels[i] >> 8) | (pixels[i] << 8));
    window(0, y, LCD_W, 16);
    tx(true, pixels, sizeof(uint16_t) * (size_t)LCD_W * 16);
}

void direct_panel_enable_display(void)
{
    command(0x29);
    vTaskDelay(pdMS_TO_TICKS(20));
    if(CONFIG_DIRECT_LCD_BL_PIN>=0)gpio_set_level(CONFIG_DIRECT_LCD_BL_PIN,1);
    ESP_LOGI(TAG,"LCD_BOOT_VISIBLE: complete background written before DISPON");
}

#if CONFIG_DIRECT_LCD_ISOLATION_TEST
_Static_assert(CONFIG_DIRECT_LCD_SPI_CLOCK_HZ == 20000000,
               "LCD isolation image requires 20 MHz hardware SPI");

static void panel_isolation_loop(void)
{
    const uint16_t colors[] = {0xf800, 0x07e0, 0x001f, BLACK, WHITE};
    const char *const names[] = {"RED", "GREEN", "BLUE", "BLACK", "WHITE", "CHECKERBOARD"};
    unsigned frame = 0;
    for (;;) {
        const unsigned pattern = frame % 6U;
        ESP_LOGI(TAG, "LCD_ISOLATION begin frame=%u pattern=%s", frame, names[pattern]);
        if (pattern < 5U) {
            fill_rect(0, 0, LCD_W, LCD_H, colors[pattern]);
        } else {
            /* 16x16 cells, top-left white. Exact full-screen coverage. */
            for (int y = 0; y < LCD_H; y += 16)
                for (int x = 0; x < LCD_W; x += 16)
                    fill_rect(x, y, 16, 16, ((x / 16 + y / 16) & 1) ? BLACK : WHITE);
        }
        if (frame == 0U) {
            direct_panel_enable_display();
            ESP_ERROR_CHECK(atomic_load(&s_lcd_tx_errors) ? ESP_FAIL : ESP_OK);
            /* OTA test images must survive the next reset. No NVS writes. */
            esp_ota_img_states_t state;
            const esp_partition_t *running = esp_ota_get_running_partition();
            if (running && esp_ota_get_state_partition(running, &state) == ESP_OK &&
                state == ESP_OTA_IMG_PENDING_VERIFY)
                ESP_ERROR_CHECK(esp_ota_mark_app_valid_cancel_rollback());
        }
        ESP_LOGI(TAG, "LCD_ISOLATION sent frame=%u pattern=%s tx=%u errors=%u hold_ms=2000",
                 frame, names[pattern], atomic_load(&s_spi_transfers),
                 atomic_load(&s_lcd_tx_errors));
        ++frame;
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
#endif

void direct_panel_init(void)
{
    panel_stage(PANEL_INIT);
#if !CONFIG_DIRECT_LCD_ISOLATION_TEST
    if (direct_keys_init() != ESP_OK)
        ESP_LOGE(TAG, "touch keyboard initialization failed");
#endif
    const int required[] = {CONFIG_DIRECT_LCD_SCLK_PIN,
        CONFIG_DIRECT_LCD_MOSI_PIN,
#if CONFIG_DIRECT_LCD_4WIRE
        CONFIG_DIRECT_LCD_DC_PIN,
#endif
    };
    for (size_t i = 0; i < sizeof(required) / sizeof(required[0]); ++i) {
        if (required[i] < 0) {
            ESP_LOGW(TAG, "panel disabled: configure required SPI pins");
            return;
        }
    }
#if !CONFIG_DIRECT_LCD_GPIO_SPI
    const spi_bus_config_t bus = {
        .sclk_io_num = CONFIG_DIRECT_LCD_SCLK_PIN,
        .mosi_io_num = CONFIG_DIRECT_LCD_MOSI_PIN,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 16384,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO));
    const spi_device_interface_config_t dev = {
        .clock_speed_hz = CONFIG_DIRECT_LCD_SPI_CLOCK_HZ,
        .mode = direct_lcd_spi_mode(CONFIG_DIRECT_LCD_NOCS_BOOT),
        .spics_io_num = direct_lcd_spi_cs(CONFIG_DIRECT_LCD_CS_PIN,CONFIG_DIRECT_LCD_CS_FIXED_LOW),
        .queue_size = 1,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(LCD_HOST, &dev, &s_lcd));
    ESP_LOGI(TAG,"LCD HARDWARE_SPI clock=%d mode=%d peripheral_CS=%d DMA, max_transfer=16384",
             dev.clock_speed_hz,dev.mode,dev.spics_io_num);
#endif
#if CONFIG_DIRECT_LCD_4WIRE
    const int dc_pin = CONFIG_DIRECT_LCD_DC_PIN;
#endif
    const int rst_pin = CONFIG_DIRECT_LCD_RST_PIN;
    const int bl_pin = CONFIG_DIRECT_LCD_BL_PIN;
    uint64_t output_mask = pin_mask(rst_pin) | pin_mask(bl_pin);
#if CONFIG_DIRECT_LCD_GPIO_SPI
    output_mask |= pin_mask(CONFIG_DIRECT_LCD_SCLK_PIN) |
                   pin_mask(CONFIG_DIRECT_LCD_MOSI_PIN) |
                   pin_mask(CONFIG_DIRECT_LCD_CS_PIN);
#endif
#if CONFIG_DIRECT_LCD_4WIRE
    output_mask |= pin_mask(dc_pin);
#endif
    gpio_config_t outputs = {
        .pin_bit_mask = output_mask,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&outputs));
#if CONFIG_DIRECT_LCD_GPIO_SPI
    /* In the no-CS profile RESET is still low from application entry. */
    gpio_set_level(CONFIG_DIRECT_LCD_SCLK_PIN,
                   CONFIG_DIRECT_LCD_NOCS_BOOT ? 1 : 0);
    gpio_set_level(CONFIG_DIRECT_LCD_MOSI_PIN, 0);
    gpio_set_level(dc_pin, 1);
    if (direct_lcd_cs_toggles(CONFIG_DIRECT_LCD_CS_PIN,CONFIG_DIRECT_LCD_CS_FIXED_LOW))
        gpio_set_level(CONFIG_DIRECT_LCD_CS_PIN, 1);
    ESP_LOGI(TAG, "LCD ST7789P3 GPIO-SPI, mode%d MSB-first, half-cycle >=2us",
             CONFIG_DIRECT_LCD_NOCS_BOOT ? 3 : 0);
#endif
#if CONFIG_DIRECT_BOOT_DIAGNOSTICS && !CONFIG_DIRECT_LCD_NOCS_BOOT
#if CONFIG_DIRECT_LCD_READBACK_TERMINAL
    if (CONFIG_DIRECT_LCD_CS_PIN >= 0 &&
        !panel_gpio_probe(CONFIG_DIRECT_LCD_CS_PIN, "CS")) return;
    if (CONFIG_DIRECT_LCD_CS_PIN >= 0)
        gpio_set_level(CONFIG_DIRECT_LCD_CS_PIN, 1);
    if (!panel_gpio_probe(CONFIG_DIRECT_LCD_SCLK_PIN, "SCL") ||
        !panel_gpio_probe(CONFIG_DIRECT_LCD_MOSI_PIN, "SDA")) return;
    gpio_set_level(CONFIG_DIRECT_LCD_SCLK_PIN, 0);
    gpio_set_level(CONFIG_DIRECT_LCD_MOSI_PIN, 0);
#endif
#if CONFIG_DIRECT_LCD_4WIRE
    if (!panel_gpio_probe(dc_pin, "D/C")) return;
#endif
    if (!panel_gpio_probe(rst_pin, "RESET")) return;
#endif
    if (bl_pin >= 0) gpio_set_level(bl_pin, 0);
    s_panel_initialized=panel_hardware_initialize();
#if CONFIG_DIRECT_LCD_ISOLATION_TEST
    ESP_ERROR_CHECK(s_panel_initialized ? ESP_OK : ESP_FAIL);
    panel_isolation_loop();
    return;
#endif
    if(s_panel_initialized){
    atomic_store(&s_recovery_error_baseline,atomic_load(&s_lcd_tx_errors));
#if CONFIG_DIRECT_LCD_READBACK_TERMINAL
    command(0x36);
    data8(0xa8); /* Valid MADCTL bits, a mixed-zero/one readback challenge. */
    const uint8_t received = panel_terminal_read8(0x0b);
    ESP_LOGI(TAG, "LCD_READBACK MADCTL expected=A8 received=%02X match=%s",
             (unsigned)received, received == 0xa8 ? "YES" : "NO");
    if (received != 0xa8)
        ESP_LOGW(TAG, "LCD_READBACK inconclusive: check module readback support / signal path");
    if (CONFIG_DIRECT_LCD_CS_PIN < 0) {
        ESP_LOGI(TAG, "LCD_READBACK terminal: SDA stays input; no further LCD transfers");
        return;
    }
    ESP_LOGI(TAG, "LCD_READBACK CS=GPIO%d deselected; resume color test",
             CONFIG_DIRECT_LCD_CS_PIN);
    command(0x36); data8(0x70); /* Restore landscape after the challenge. */
#endif
    ESP_LOGI(TAG, "LCD SCK=%d MOSI=%d DC=%d CS=%d RST=%d BL=%d",
             CONFIG_DIRECT_LCD_SCLK_PIN, CONFIG_DIRECT_LCD_MOSI_PIN,
             CONFIG_DIRECT_LCD_DC_PIN, CONFIG_DIRECT_LCD_CS_PIN,
             rst_pin, bl_pin);
    if (CONFIG_DIRECT_LCD_CS_PIN < 0 || bl_pin < 0)
        ESP_LOGI(TAG, "LCD uses external CS-low / backlight power where GPIO is -1");

#if CONFIG_DIRECT_BOOT_DIAGNOSTICS && CONFIG_DIRECT_LCD_COLOR_TEST
    direct_panel_enable_display(); /* Explicit diagnostic build only. */
    const uint16_t test_colors[] = {0xf800, 0x07e0, 0x001f};
    const char *const test_names[] = {"RED", "GREEN", "BLUE"};
    for (size_t i = 0; i < 3; ++i) {
#if CONFIG_DIRECT_LCD_GPIO_SPI
        ESP_LOGI(TAG, "LCD color test %s, GPIO-SPI", test_names[i]);
#else
        ESP_LOGI(TAG, "LCD color test %s, SPI=1MHz", test_names[i]);
#endif
        fill_rect(0, 0, LCD_W, LCD_H, test_colors[i]);
        vTaskDelay(pdMS_TO_TICKS(CONFIG_DIRECT_LCD_GPIO_SPI ? 5000 : 1000));
    }
    fill_rect(0, 0, LCD_W, LCD_H, BLACK);
#endif
    }
    /* Even a failed initialization needs a live owner for recovery requests.
     * The panel task owns the only sequence: clear -> splash -> first UI.
     * Do not paint a home-page banner before the brand animation. */
    panel_stage(s_panel_initialized?PANEL_TASK_WAIT:PANEL_FAILED);
    if (xTaskCreate(panel_task, "direct_panel", 6144, NULL, 3, NULL) != pdPASS) {
        panel_stage(PANEL_FAILED);
        ESP_LOGE(TAG, "panel task allocation failed");
    }
}
