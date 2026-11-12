#include "panel_indicators_hw.h"
#include <string.h>
#include <stdatomic.h>
#ifdef PANEL_INDICATORS_HOST
#include "indicator_test_sdk.h"
#else
#include "panel_bus.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#endif

static const char *TAG="panel_led";
static rmt_channel_handle_t s_channel;
static rmt_encoder_handle_t s_encoder;
static bool s_ready,s_pending,s_have_last;
static atomic_uchar s_brightness=CONFIG_PANEL_LED_BRIGHTNESS;
static uint8_t s_last[12],s_pending_pixels[12];
/* The bus snapshot and payload stay in DRAM, not on main_loop's stack. */
static panel_snapshot_t s_snapshot;
static panel_led_pulse_t s_pulses[PANEL_LED_SYMBOLS];
static rmt_symbol_word_t s_symbols[PANEL_LED_SYMBOLS];
static uint32_t s_next_error;
static bool s_chg_reported;
static unsigned s_chg_state;

static void report_chg(const panel_led_t *led,uint32_t now,bool valid)
{
    unsigned state=(unsigned)led->color*2U+(unsigned)led->pattern;
    if(s_chg_reported && state==s_chg_state)return;
    const uint16_t regs[]={0x42,0x43,0x44,0x81,0x85};
    uint16_t values[5]={0xffff,0xffff,0xffff,0xffff,0xffff};
    unsigned fresh_mask=0;
    for(unsigned i=0;i<5;i++){
        (void)panel_value(&s_snapshot,regs[i],&values[i]);
        if(panel_fresh(&s_snapshot,regs[i]))fresh_mask|=1U<<i;
    }
    ESP_LOGI(TAG,"CHG color=%u pattern=%u valid=%u profile=%u detected=%u fresh=%X mode=%04X terminal=%04X flow=%04X current_raw=%04X power_raw=%04X current_age_ms=%lu",
        (unsigned)led->color,(unsigned)led->pattern,(unsigned)valid,(unsigned)s_snapshot.config.profile_override,
        (unsigned)s_snapshot.detected_profile,fresh_mask,(unsigned)values[0],(unsigned)values[1],
        (unsigned)values[2],(unsigned)values[3],(unsigned)values[4],(unsigned long)(now-panel_stamp(&s_snapshot,0x81)));
    s_chg_reported=true;s_chg_state=state;
}

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
static void send_pixels(const uint8_t pixels[12])
{
    if(!finish_pending(0))return; /* Never overwrite a live RMT payload. */
    if(s_have_last && !memcmp(s_last,pixels,12))return;
    if(panel_indicators_encode(pixels,s_pulses,PANEL_LED_SYMBOLS)!=PANEL_LED_SYMBOLS)return;
    for(unsigned i=0;i<PANEL_LED_SYMBOLS;i++)s_symbols[i]=(rmt_symbol_word_t){
        .level0=i<96,.duration0=s_pulses[i].high,.level1=0,.duration1=s_pulses[i].low};
    const rmt_transmit_config_t tx={.loop_count=0,.flags.eot_level=0};
    esp_err_t err=rmt_transmit(s_channel,s_encoder,s_symbols,sizeof(s_symbols),&tx);
    if(err!=ESP_OK){report(err);return;}
    memcpy(s_pending_pixels,pixels,12);s_pending=true;
    (void)finish_pending(20);
}
bool full_indicators_init(int gpio)
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
    const uint8_t off[12]={0};send_pixels(off);
    ESP_LOGI(TAG,"WS2812 GPIO%d count=4 order=ALM/NET/CHG/RUN GRB brightness=%u/255",
        gpio,(unsigned)CONFIG_PANEL_LED_BRIGHTNESS);
    return true;
}
void full_indicators_set_brightness(uint8_t brightness)
{
    atomic_store(&s_brightness,brightness);
}
void full_indicators_tick(const panel_led_network_t *network)
{
    if(!s_ready || !finish_pending(0))return;
    uint32_t now=(uint32_t)(esp_timer_get_time()/1000);
    bool valid=panel_bus_snapshot(&s_snapshot) && s_snapshot.context_generation==panel_bus_context_generation();
    panel_led_t leds[4];uint8_t pixels[12];
    panel_indicators_policy(valid?&s_snapshot:NULL,network,now,leds);
    report_chg(&leds[LED_CHG],now,valid);
    const panel_led_t chain[]={leds[LED_ALM],leds[LED_NET],leds[LED_CHG],leds[LED_RUN]};
    panel_indicators_grb(chain,now,atomic_load(&s_brightness),pixels);
    send_pixels(pixels);
}
