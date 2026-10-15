#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "direct_board_pins.h"
#include "direct_lcd_codec.h"
#include "../tmp/board_can_config.inc"

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
static unsigned can_output_enables, lcd_writes;
typedef int esp_err_t;
typedef struct { size_t length; const void *tx_buffer; } spi_transaction_t;
typedef struct { int sclk_io_num, mosi_io_num, miso_io_num, quadwp_io_num,
                     quadhd_io_num, max_transfer_sz; } spi_bus_config_t;
typedef struct { int clock_speed_hz, mode, spics_io_num, queue_size; } spi_device_interface_config_t;
static void *s_lcd;
static unsigned s_lcd_tx_errors;

static void require(bool condition, const char *message)
{
    if (!condition) { fprintf(stderr, "FAIL: %s\n", message); exit(1); }
}

static int gpio_set_level(int pin, int level)
{
    require(pin >= 0 && pin < 64, "LCD accessed an invalid GPIO");
    if (pin == 4)
        require(level == 1, "LCD drove shared CAN TX GPIO4 low");
    else
        ++lcd_writes;
    levels[pin] = level;
    return 0;
}

static int gpio_config(const gpio_config_t *cfg)
{
    if (cfg->pin_bit_mask & (1ULL << 4)) {
        require(levels[4] == 1, "CAN output enabled before setting recessive latch");
        require(cfg->mode == GPIO_MODE_OUTPUT, "CAN TX must remain an output");
        ++can_output_enables;
    }
    return 0;
}

static uint64_t pin_mask(int pin) { return pin < 0 ? 0 : 1ULL << pin; }
static void esp_rom_delay_us(unsigned delay) { (void)delay; }
static void vTaskDelay(unsigned delay) { (void)delay; }
static int direct_keys_init(void) { return 0; }
static int spi_bus_initialize(int host, const spi_bus_config_t *bus, int dma)
{
    (void)host; (void)dma;
    require(bus->sclk_io_num != 4 && bus->mosi_io_num != 4 && bus->miso_io_num != 4,
            "SPI bus took over CAN TX");
    return 0;
}
static int spi_bus_add_device(int host, const spi_device_interface_config_t *dev, void **handle)
{
    (void)host;
    require(dev->spics_io_num == -1, "SPI peripheral took over CAN TX");
    *handle = &levels[0];
    return 0;
}
static int spi_device_polling_transmit(void *handle, const spi_transaction_t *t)
{
    (void)handle;
    lcd_writes += (unsigned)t->length / 8;
    return 0;
}
static void tx(bool data, const void *bytes, size_t count);
static void command(uint8_t value) { tx(false, &value, 1); }
static void data8(uint8_t value) { tx(true, &value, 1); }
static void fill_rect(int x, int y, int w, int h, uint16_t color)
{
    (void)x; (void)y; (void)w; (void)h;
    tx(true, &color, sizeof(color));
}
static void panel_task(void *arg) { (void)arg; }
static int xTaskCreate(void (*task)(void *), const char *name, unsigned stack,
                       void *arg, int prio, void *handle)
{
    (void)task; (void)name; (void)stack; (void)arg; (void)prio; (void)handle;
    return pdPASS;
}

#include "../tmp/board_can_under_test.inc"

int main(void)
{
    memset(levels, -1, sizeof(levels));
    require(direct_board_pins_valid(), "board pins conflict");
#ifdef HAVE_CAN_IDLE_INIT
    direct_can_idle_init();
#endif
    direct_panel_prepare_boot();
    direct_panel_init();
    const uint8_t commands[] = {0x11, 0x29, 0x2a, 0x2b, 0x2c};
    uint8_t pixels[320 * 2];
    memset(pixels, 0xa5, sizeof(pixels));
    for (unsigned i = 0; i < sizeof(commands); ++i) {
        tx(false, &commands[i], 1);
        tx(true, pixels, sizeof(pixels));
    }
    require(can_output_enables == 1 && levels[4] == 1,
            "CAN TX not initialized and retained high");
    require(lcd_writes > 1000, "LCD transport did not run");
    require(direct_lcd_spi_cs(CONFIG_DIRECT_LCD_CS_PIN,
                             CONFIG_DIRECT_LCD_CS_FIXED_LOW) == -1,
            "SPI peripheral must not own CAN TX");
    puts("PASS: actual boot and LCD command/pixel writes retain GPIO4 high; external LCD CS; CAN latch high before output enable; no board pin overlap");
    return 0;
}
