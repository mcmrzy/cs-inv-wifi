#include "status_led_rmt.h"
#include <string.h>
#ifndef PANEL_INDICATORS_HOST
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "esp_log.h"
#include "esp_timer.h"
#endif

static const char *TAG = "panel_led";
static rmt_channel_handle_t s_channel;
static rmt_encoder_handle_t s_encoder;
static bool s_ready,s_pending,s_have_last;
static uint8_t s_last[PANEL_LED_BYTES],s_pending_pixels[PANEL_LED_BYTES];
static panel_led_pulse_t s_pulses[PANEL_LED_SYMBOLS];
static rmt_symbol_word_t s_symbols[PANEL_LED_SYMBOLS];
static uint32_t s_next_error;
static void report(esp_err_t err)
{
    uint32_t now=(uint32_t)(esp_timer_get_time()/1000);
    if(!s_next_error || (int32_t)(now-s_next_error)>=0){
        ESP_LOGW(TAG,"WS2812 driver: %s; inverter tasks continue",esp_err_to_name(err));
        s_next_error=now+30000U;
    }
}
static bool finish_pending(unsigned timeout_ms)
{
    if(!s_pending)return true;
    esp_err_t err=rmt_tx_wait_all_done(s_channel,(int)timeout_ms);
    if(err!=ESP_OK){if(timeout_ms || err!=ESP_ERR_TIMEOUT)report(err);return false;}
    s_pending=false;s_have_last=true;memcpy(s_last,s_pending_pixels,sizeof(s_last));return true;
}
void status_led_send(const uint8_t pixels[PANEL_LED_BYTES])
{
    if(!s_ready || !finish_pending(0))return; /* Never overwrite a live RMT payload. */
    if(s_have_last && !memcmp(s_last,pixels,sizeof(s_last)))return;
    if(panel_indicators_encode(pixels,s_pulses,PANEL_LED_SYMBOLS)!=PANEL_LED_SYMBOLS)return;
    for(unsigned i=0;i<PANEL_LED_SYMBOLS;i++)s_symbols[i]=(rmt_symbol_word_t){
        .level0=i+1U<PANEL_LED_SYMBOLS,.duration0=s_pulses[i].high,.level1=0,.duration1=s_pulses[i].low};
    const rmt_transmit_config_t tx={.loop_count=0,.flags.eot_level=0};
    esp_err_t err=rmt_transmit(s_channel,s_encoder,s_symbols,sizeof(s_symbols),&tx);
    if(err!=ESP_OK){report(err);return;}
    memcpy(s_pending_pixels,pixels,sizeof(s_pending_pixels));s_pending=true;
    (void)finish_pending(20);
}
bool status_led_init(int gpio)
{
    if(gpio<0)return false;
    if(s_ready)return true;
    /* ESP32-C3 has 4 x 48 RMT words. Reserve 3 contiguous blocks on TX0:
     * all 96 data bits + 300us latch + EOF fit before transmission starts,
     * avoiding a mid-frame refill under WiFi/BLE/flash interrupt latency. */
    const rmt_tx_channel_config_t channel={.gpio_num=gpio,.clk_src=RMT_CLK_SRC_DEFAULT,
        .resolution_hz=PANEL_LED_RMT_HZ,.mem_block_symbols=144,.trans_queue_depth=1};
    esp_err_t err=rmt_new_tx_channel(&channel,&s_channel);
    if(err!=ESP_OK){report(err);return false;}
    const rmt_copy_encoder_config_t copy={};
    err=rmt_new_copy_encoder(&copy,&s_encoder);
    if(err!=ESP_OK){rmt_del_channel(s_channel);s_channel=NULL;report(err);return false;}
    err=rmt_enable(s_channel);
    if(err!=ESP_OK){rmt_del_encoder(s_encoder);rmt_del_channel(s_channel);s_encoder=NULL;s_channel=NULL;report(err);return false;}
    s_ready=true;
    const uint8_t off[PANEL_LED_BYTES] = {0};
    status_led_send(off);
    ESP_LOGI(TAG, "WS2812 GPIO%d count=4 order=RUN/CHG/NET/ALM GRB", gpio);
    return true;
}

bool status_led_is_ready(void)
{
    return s_ready;
}
