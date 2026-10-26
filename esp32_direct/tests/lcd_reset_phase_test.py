"""Model IDF's lazy mode application at the real LCD initialization boundary."""
from pathlib import Path
import unittest
from mqtt_connection_window_test import function
import mqtt_local_mode_test as local_mode

ROOT = Path(__file__).resolve().parents[1]


class LcdResetPhaseTests(unittest.TestCase):
    def test_first_spi_configuration_finishes_while_lcd_reset_is_asserted(self):
        source = (ROOT / 'main/direct/direct_panel.c').read_text(encoding='utf-8')
        code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include "direct_lcd_codec.h"
#define CONFIG_DIRECT_LCD_GPIO_SPI 0
#define CONFIG_DIRECT_LCD_4WIRE 1
#define CONFIG_DIRECT_LCD_3WIRE 0
#define CONFIG_DIRECT_BOOT_DIAGNOSTICS 0
#define CONFIG_DIRECT_LCD_NOCS_BOOT 1
#define CONFIG_DIRECT_LCD_MINIMAL_INIT 1
#define CONFIG_DIRECT_LCD_READBACK_TERMINAL 0
#define CONFIG_DIRECT_LCD_CS_FIXED_LOW 0
#define CONFIG_DIRECT_LCD_COLOR_TEST 0
#define CONFIG_DIRECT_LCD_SCLK_PIN 6
#define CONFIG_DIRECT_LCD_MOSI_PIN 7
#define CONFIG_DIRECT_LCD_DC_PIN 9
#define CONFIG_DIRECT_LCD_RST_PIN 10
#define CONFIG_DIRECT_LCD_CS_PIN -1
#define CONFIG_DIRECT_LCD_BL_PIN -1
#define CONFIG_DIRECT_LCD_SPI_CLOCK_HZ 8000000
#define GPIO_MODE_OUTPUT 1
#define ESP_OK 0
#define ESP_ERROR_CHECK(expr) assert((expr)==ESP_OK)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define pdMS_TO_TICKS(ms) (ms)
#define pdPASS 1
#define SPI_DMA_CH_AUTO 0
#define LCD_HOST 0
#define LCD_W 320
#define LCD_H 240
#define BLACK 0
typedef int esp_err_t;
typedef struct {uint64_t pin_bit_mask;int mode;} gpio_config_t;
typedef struct {int sclk_io_num,mosi_io_num,miso_io_num,quadwp_io_num,quadhd_io_num,max_transfer_sz;} spi_bus_config_t;
typedef struct {int clock_speed_hz,mode,spics_io_num,queue_size;} spi_device_interface_config_t;
typedef struct {size_t length;const void *tx_buffer;} spi_transaction_t;
static atomic_uint s_spi_bytes,s_spi_started_ms,s_spi_transfers,s_lcd_tx_errors,s_recovery_error_baseline;
static atomic_bool s_spi_active;
static bool s_panel_initialized;
static int pins[32];
static unsigned clock_ms,first_command,reset_writes,useful_writes,reset_low_at,reset_released_at;
static bool configured;
static bool fail_prime;
static unsigned stage_seen,tasks;
static void *s_lcd;
static unsigned now_ms(void){return clock_ms;}
static uint64_t pin_mask(int pin){return pin<0?0:1ULL<<pin;}
static int gpio_set_level(int pin,int value){
 assert(pin>=0&&pin<32&&pin!=4);
 if(pin==10&&!value)reset_low_at=clock_ms;
 if(pin==10&&value){
  /* IDF applies mode at the first real transaction, not add_device(). */
  assert(configured&&"SPI mode must be applied before releasing LCD RESET");
  assert(clock_ms-reset_low_at==20U&&"LCD RESET must use the short 20ms hold, not 1 second");
  reset_released_at=clock_ms;
 }
 pins[pin]=value;return ESP_OK;
}
static int gpio_config(const gpio_config_t *c){assert(!(c->pin_bit_mask&(1ULL<<4)));return ESP_OK;}
static int spi_bus_initialize(int host,const spi_bus_config_t *b,int dma){
 (void)host;(void)b;(void)dma;assert(pins[10]==0);pins[6]=0;return ESP_OK;
}
static int spi_bus_add_device(int host,const spi_device_interface_config_t *d,void **h){
 (void)host;assert(d->mode==3&&d->spics_io_num==-1);*h=(void*)1;return ESP_OK;
}
static int spi_device_polling_transmit(void *h,const spi_transaction_t *t){
 assert(h&&t->length);
 if(fail_prime)return -1;
 if(!configured){assert(pins[10]==0);configured=true;pins[6]=1;}
 if(!pins[10])reset_writes++;
 else {if(!useful_writes){assert(clock_ms-reset_released_at>=150U);first_command=*(const uint8_t*)t->tx_buffer;}useful_writes++;}
 return ESP_OK;
}
static void esp_rom_delay_us(unsigned us){(void)us;}
static void vTaskDelay(unsigned ms){clock_ms+=ms;}
static int direct_keys_init(void){return ESP_OK;}
enum {PANEL_INIT,PANEL_TASK_WAIT,PANEL_FAILED};
static void panel_stage(unsigned stage){stage_seen=stage;}
static void panel_task(void *arg){(void)arg;}
static int xTaskCreate(void(*f)(void*),const char*n,unsigned s,void*a,int p,void*h){
 (void)f;(void)n;(void)s;(void)a;(void)p;(void)h;tasks++;return pdPASS;
}
static void tx(bool data,const void *bytes,size_t count);
static void command(uint8_t c){tx(false,&c,1);}
static void data8(uint8_t v){tx(true,&v,1);}
static void fill_rect(int x,int y,int w,int h,uint16_t c){(void)x;(void)y;(void)w;(void)h;tx(true,&c,2);}
FUNCTIONS
int main(void){
 for(unsigned idle=0;idle<2;idle++){
  configured=false;reset_writes=useful_writes=0;pins[4]=1;pins[6]=idle;
  unsigned started=clock_ms;
  direct_panel_prepare_boot();direct_panel_init();
  assert(clock_ms-started==440U); /* 20ms low + 150ms release + SWRESET 150ms + SLPOUT 120ms */
  assert(reset_writes==1&&first_command==1&&useful_writes>1&&pins[4]==1);
 }
 fail_prime=true;configured=false;tasks=0;atomic_store(&s_lcd_tx_errors,0);
 direct_panel_prepare_boot();direct_panel_init();
 assert(pins[10]==0&&tasks==1&&!s_panel_initialized&&stage_seen==PANEL_FAILED&&atomic_load(&s_lcd_tx_errors)==1);
 puts("PASS actual LCD startup holds RESET 20ms, retains release/SWRESET/SLPOUT waits and primes SPI under RESET; failed prime keeps owner available; CS/CAN unchanged");
}
'''.replace('FUNCTIONS', '\n'.join(function(source, name) for name in (
            'direct_panel_prepare_boot', 'tx', 'panel_register_init', 'panel_hardware_initialize', 'direct_panel_init')))
        code = code.replace('#include "direct_lcd_codec.h"',
                            '#include "' + str(ROOT / 'main/direct/direct_lcd_codec.h').replace('\\', '/') + '"')
        local_mode.LocalModeTests.compile_run(self, code)


if __name__ == '__main__':
    unittest.main()
