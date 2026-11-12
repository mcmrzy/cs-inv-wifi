#include "panel_lcd.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

enum { PIN_RST=4, PIN_BL=5, PIN_SCK=6, PIN_MOSI=7, PIN_DC=8 };
static spi_device_handle_t s_spi;
static uint16_t s_pixels[320*16] __attribute__((aligned(4)));
static bool s_visible;
static bool s_bus_initialized;

void panel_lcd_prepare(void)
{
    gpio_set_level(PIN_RST,0);
    gpio_set_level(PIN_BL,!CONFIG_PANEL_LCD_BL_ACTIVE_LEVEL);
    gpio_set_level(PIN_SCK,CONFIG_PANEL_LCD_SPI_MODE>=2);
    gpio_set_level(PIN_MOSI,0);
    gpio_set_level(PIN_DC,1);
    gpio_config_t pins={.pin_bit_mask=(1ULL<<PIN_RST)|(1ULL<<PIN_BL)|
        (1ULL<<PIN_SCK)|(1ULL<<PIN_MOSI)|(1ULL<<PIN_DC),.mode=GPIO_MODE_OUTPUT};
    (void)gpio_config(&pins);
}

static esp_err_t tx(bool data,const void *bytes,size_t size)
{
    gpio_set_level(PIN_DC,data);
    spi_transaction_t t={.length=size*8,.tx_buffer=bytes};
    return spi_device_polling_transmit(s_spi,&t);
}
static esp_err_t command(uint8_t c) { return tx(false,&c,1); }
static esp_err_t reg(uint8_t c,const uint8_t *bytes,size_t size)
{
    esp_err_t e=command(c);
    return e==ESP_OK && size ? tx(true,bytes,size) : e;
}

#define CHECK(expr) do {esp_err_t err=(expr);if(err!=ESP_OK)return err;} while(0)

esp_err_t panel_lcd_init(void)
{
    gpio_set_level(PIN_BL,!CONFIG_PANEL_LCD_BL_ACTIVE_LEVEL);
    gpio_set_level(PIN_RST,0);
    s_visible=false;
    if(!s_spi) {
        spi_bus_config_t bus={.mosi_io_num=PIN_MOSI,.miso_io_num=-1,.sclk_io_num=PIN_SCK,
            .quadwp_io_num=-1,.quadhd_io_num=-1,.max_transfer_sz=sizeof(s_pixels)};
        if(!s_bus_initialized) {
            CHECK(spi_bus_initialize(SPI2_HOST,&bus,SPI_DMA_CH_AUTO));
            s_bus_initialized=true;
        }
        spi_device_interface_config_t dev={.clock_speed_hz=CONFIG_PANEL_LCD_SPI_HZ,
            .mode=CONFIG_PANEL_LCD_SPI_MODE,.spics_io_num=-1,.queue_size=1};
        CHECK(spi_bus_add_device(SPI2_HOST,&dev,&s_spi));
    }
    vTaskDelay(pdMS_TO_TICKS(20));
    /* Prime the peripheral's lazy mode setup while RESET masks the clocks. */
    CHECK(command(0));
    gpio_set_level(PIN_RST,1);
    vTaskDelay(pdMS_TO_TICKS(150));
    CHECK(command(0x01));vTaskDelay(pdMS_TO_TICKS(150));
    CHECK(command(0x28));CHECK(command(0x11));vTaskDelay(pdMS_TO_TICKS(120));
    /* Same landscape, RAM byte order and power/gamma profile as direct. */
    CHECK(reg(0x36,(uint8_t[]){0x70},1));
    CHECK(reg(0x3a,(uint8_t[]){0x55},1));
    CHECK(reg(0xb0,(uint8_t[]){0x00,0xf0},2));
    CHECK(reg(0xb2,(uint8_t[]){0x0c,0x0c,0x00,0x33,0x33},5));
    CHECK(reg(0xb7,(uint8_t[]){0x65},1));CHECK(reg(0xbb,(uint8_t[]){0x1d},1));
    CHECK(reg(0xc0,(uint8_t[]){0x2c},1));CHECK(reg(0xc2,(uint8_t[]){0x01},1));
    CHECK(reg(0xc3,(uint8_t[]){0x13},1));CHECK(reg(0xc4,(uint8_t[]){0x20},1));
    CHECK(reg(0xc6,(uint8_t[]){0x0f},1));CHECK(reg(0xd0,(uint8_t[]){0xa4,0xa1},2));
    CHECK(reg(0xd6,(uint8_t[]){0xa1},1));
    CHECK(reg(0xe0,(uint8_t[]){0xf0,0x06,0x12,0x13,0x14,0x2d,0x37,0x44,0x48,0x08,0x10,0x0f,0x17,0x1a},14));
    CHECK(reg(0xe1,(uint8_t[]){0xf0,0x04,0x0d,0x0c,0x0c,0x27,0x38,0x54,0x48,0x3d,0x1a,0x1a,0x1c,0x1f},14));
    CHECK(command(0x13));CHECK(command(0x20));
    return ESP_OK;
}

esp_err_t panel_lcd_render(const panel_view_t *view)
{
    if(!s_spi || !view)return ESP_ERR_INVALID_STATE;
    for(unsigned y=0;y<240;y+=16) {
        panel_ui_draw(view,s_pixels,(int)y,16);
        for(unsigned i=0;i<320*16;i++)s_pixels[i]=(s_pixels[i]>>8)|(s_pixels[i]<<8);
        CHECK(reg(0x2a,(uint8_t[]){0,0,1,0x3f},4));
        CHECK(reg(0x2b,(uint8_t[]){0,(uint8_t)y,0,(uint8_t)(y+15)},4));
        CHECK(command(0x2c));CHECK(tx(true,s_pixels,sizeof(s_pixels)));
    }
    if(!s_visible) {
        CHECK(command(0x29));vTaskDelay(pdMS_TO_TICKS(20));
        gpio_set_level(PIN_BL,CONFIG_PANEL_LCD_BL_ACTIVE_LEVEL);
        s_visible=true;
    }
    return ESP_OK;
}
