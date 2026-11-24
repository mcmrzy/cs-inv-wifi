#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include "direct_board_pins.h"
#include "direct_lcd_codec.h"
#include "../tmp/board_l10_config.inc"

#define GPIO_MODE_OUTPUT 1
#define GPIO_PULLUP_ENABLE 1
#define GPIO_PULLUP_DISABLE 0
#define GPIO_PULLDOWN_DISABLE 0
#define GPIO_INTR_DISABLE 0
#define ESP_ERROR_CHECK(call) do { if ((call) != 0) exit(2); } while (0)
#define ESP_LOGI(tag, ...) ((void)(tag))
#define ESP_LOGE(tag, ...) ((void)(tag))
#define ESP_LOGW(tag, ...) ((void)(tag))
#define ESP_OK 0
#define LCD_HOST 0
#define SPI_DMA_CH_AUTO 0
#define LCD_W 320
#define LCD_H 240
#define BLACK 0
#define pdPASS 1
#define pdMS_TO_TICKS(ms) (ms)

typedef struct {
    uint64_t pin_bit_mask;
    int mode, pull_up_en, pull_down_en, intr_type;
} gpio_config_t;
static const char *TAG = "test";
static int levels[64];
static unsigned reset_releases, lcd_writes;
typedef int esp_err_t;
typedef struct { size_t length; const void *tx_buffer; } spi_transaction_t;
typedef struct { int sclk_io_num, mosi_io_num, miso_io_num, quadwp_io_num,
                     quadhd_io_num, max_transfer_sz; } spi_bus_config_t;
typedef struct { int clock_speed_hz, mode, spics_io_num, queue_size; } spi_device_interface_config_t;
static void *s_lcd;
static unsigned s_lcd_tx_errors;
static atomic_uint s_recovery_error_baseline;
static bool s_panel_initialized;
static atomic_uint s_spi_bytes,s_spi_started_ms,s_spi_transfers;
static atomic_bool s_spi_active;
enum {PANEL_INIT,PANEL_TASK_WAIT,PANEL_FAILED};
static void panel_stage(unsigned stage){(void)stage;}
static unsigned now_ms(void){return 0;}

static void require(bool condition, const char *message)
{
    if (!condition) { fprintf(stderr, "FAIL: %s\n", message); exit(1); }
}

static int gpio_set_level(int pin, int level)
{
    require(pin >= 0 && pin < 64, "LCD accessed an invalid GPIO");
    require(pin != 9 && pin != 10, "LCD took over LED data or buzzer");
    if (pin == 4 && level) {
        require(levels[5] == 0, "backlight enabled during LCD reset");
        ++reset_releases;
    }
    if (pin == 5 && level)
        require(reset_releases && lcd_writes > 1000, "backlight enabled before an LCD frame");
    levels[pin] = level;
    return 0;
}

static int gpio_config(const gpio_config_t *cfg)
{
    if (cfg->pin_bit_mask & (1ULL << 4))
        require(levels[4] == 0, "LCD reset latch must be low before output enable");
    if (cfg->pin_bit_mask & (1ULL << 5))
        require(levels[5] == 0, "Backlight latch must be low before output enable");
    return 0;
}

static uint64_t pin_mask(int pin) { return pin < 0 ? 0 : 1ULL << pin; }
static void esp_rom_delay_us(unsigned delay) { (void)delay; }
static void vTaskDelay(unsigned delay) { (void)delay; }
static int direct_keys_init(void) { return 0; }
static int spi_bus_initialize(int host, const spi_bus_config_t *bus, int dma)
{
    (void)host; (void)dma;
    require(bus->sclk_io_num == 6 && bus->mosi_io_num == 7 && bus->miso_io_num == -1,
            "unexpected ST7789 SPI wiring");
    return 0;
}
static int spi_bus_add_device(int host, const spi_device_interface_config_t *dev, void **handle)
{
    (void)host;
    require(dev->spics_io_num == -1 && dev->mode == 3, "LCD CS must be external low with mode 3");
    *handle = &levels[0];
    return 0;
}
static int spi_device_polling_transmit(void *handle, const spi_transaction_t *t)
{
    (void)handle;
    require(levels[8] == 0 || levels[8] == 1, "missing GPIO8 D/C");
    lcd_writes += (unsigned)t->length / 8;
    return 0;
}
static void tx(bool data, const void *bytes, size_t count);
static void command(uint8_t value) { tx(false, &value, 1); }
static void data8(uint8_t value) { tx(true, &value, 1); }
static void fill_rect(int x, int y, int w, int h, uint16_t color)
{
    (void)x; (void)y; (void)w; (void)h;
    uint16_t row[320] = {color};
    for (int line = 0; line < h; ++line) tx(true, row, sizeof(row));
}
static void panel_task(void *arg) { (void)arg; }
static int xTaskCreate(void (*task)(void *), const char *name, unsigned stack,
                       void *arg, int prio, void *handle)
{
    (void)task; (void)name; (void)stack; (void)arg; (void)prio; (void)handle;
    return pdPASS;
}

#include "../tmp/board_l10_under_test.inc"

int main(void)
{
    memset(levels, -1, sizeof(levels));
    require(direct_board_pins_valid(), "board pins conflict");
    require(CONFIG_DIRECT_DSP_TX_PIN == 1 && CONFIG_DIRECT_DSP_RX_PIN == 0 &&
            CONFIG_DIRECT_BMS_TX_PIN == 21 && CONFIG_DIRECT_BMS_RX_PIN == 20,
            "DSP or BMS UART pins changed");
    require(CONFIG_DIRECT_KEY_CLOCK_PIN == 3 && CONFIG_DIRECT_KEY_DATA_PIN == 2 &&
            CONFIG_DIRECT_LED_GPIO == 9 && CONFIG_DIRECT_BUZZ_PIN == 10,
            "wrong key, LED or buzzer mapping");
    direct_can_idle_init();
    require(levels[4] == -1, "CAN must not drive the LCD reset");
    direct_panel_prepare_boot();
    direct_panel_init();
    require(reset_releases == 1 && levels[4] == 1 && levels[5] == 0,
            "LCD must release reset with backlight still off");
    const uint8_t commands[] = {0x11, 0x29, 0x2a, 0x2b, 0x2c};
    uint8_t pixels[320 * 2];
    memset(pixels, 0xa5, sizeof(pixels));
    for (unsigned i = 0; i < sizeof(commands); ++i) {
        tx(false, &commands[i], 1);
        tx(true, pixels, sizeof(pixels));
    }
    direct_panel_enable_display();
    require(levels[5] == 1, "Backlight did not turn on after display enable");
    puts("PASS: L10 GPIO roles, SPI mode 3, reset and first-frame backlight");
    return 0;
}
