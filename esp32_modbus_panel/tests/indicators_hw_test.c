#define PANEL_INDICATORS_HOST 1
#include "panel_indicators.h"
#include "indicator_test_sdk.h"
#include "../../esp32_direct/main/direct/status_led_rmt.c"
#include "../full/panel_indicators_hw.c"
#include <assert.h>
#include <stdio.h>
static int fail_stage;
static bool stalled;
static bool inverter_online;
static bool battery_only;
static unsigned sent,deleted;
static uint32_t time_ms=1000; /* Breath peak for deterministic payload checks. */
void mock_log(const char *t,const char *f,...){(void)t;(void)f;}
int64_t esp_timer_get_time(void){return (int64_t)time_ms*1000;}
const char *esp_err_to_name(int e){(void)e;return "mock";}
esp_err_t rmt_new_tx_channel(const rmt_tx_channel_config_t *c,rmt_channel_handle_t *out)
{assert(c->gpio_num==9 && c->mem_block_symbols==144 && c->resolution_hz==20000000 && c->trans_queue_depth==1);*out=(void *)1;return fail_stage==1?ESP_FAIL:ESP_OK;}
esp_err_t rmt_new_copy_encoder(const rmt_copy_encoder_config_t *c,rmt_encoder_handle_t *out)
{(void)c;*out=(void *)2;return fail_stage==2?ESP_FAIL:ESP_OK;}
esp_err_t rmt_enable(rmt_channel_handle_t c){(void)c;return fail_stage==3?ESP_FAIL:ESP_OK;}
esp_err_t rmt_del_channel(rmt_channel_handle_t c){(void)c;deleted++;return ESP_OK;}
esp_err_t rmt_del_encoder(rmt_encoder_handle_t c){(void)c;deleted++;return ESP_OK;}
esp_err_t rmt_transmit(rmt_channel_handle_t c,rmt_encoder_handle_t e,const void *p,size_t n,const rmt_transmit_config_t *t)
{(void)c;(void)e;assert(p==s_symbols && n==97*sizeof(rmt_symbol_word_t) && t->loop_count==0 && !t->flags.eot_level);sent++;return fail_stage==4?ESP_FAIL:ESP_OK;}
esp_err_t rmt_tx_wait_all_done(rmt_channel_handle_t c,int wait){(void)c;(void)wait;return stalled?ESP_ERR_TIMEOUT:ESP_OK;}
bool panel_bus_snapshot(panel_snapshot_t *s)
{
    memset(s,0,sizeof(*s));
    s->context_generation=1;
    s->now_ms=time_ms;
    if(!inverter_online)return false;
    if(battery_only){
        s->config.profile_override=PANEL_PROFILE_3KU;
        panel_store(s,0x42,5,time_ms);panel_store(s,0x43,(1U<<6)|(3U<<8),time_ms);
        panel_store(s,0x44,(1U<<8)|(1U<<9),time_ms);
        panel_store(s,0x46,0,time_ms);panel_store(s,0x47,0,time_ms);
        panel_store(s,0x48,0,time_ms);panel_store(s,0x49,0,time_ms);
        return panel_store(s,0x81,1,time_ms);
    }
    return panel_store(s,0x42,2,time_ms);
}
uint32_t panel_bus_context_generation(void){return 1;}
static void reset(void){s_ready=s_pending=s_have_last=false;s_channel=s_encoder=NULL;s_next_error=0;sent=deleted=0;fail_stage=0;stalled=false;}
int main(void){
    reset();assert(!full_indicators_init(-1) && !s_ready && !sent);
    reset();fail_stage=1;assert(!full_indicators_init(9) && !s_ready);
    reset();fail_stage=2;assert(!full_indicators_init(9) && deleted==1);
    reset();fail_stage=3;assert(!full_indicators_init(9) && deleted==2);
    reset();assert(full_indicators_init(9) && sent==1 && s_have_last);
    assert(s_symbols[0].duration0==7 && s_symbols[0].level0==1);
    assert(!s_symbols[96].level0 && !s_symbols[96].level1 && s_symbols[96].duration0==3000);
    panel_led_network_t n={.enabled=true,.cloud=true};full_indicators_tick(&n);
    assert(sent==2 && s_last[6]==24 && s_last[3]==0);full_indicators_tick(&n);assert(sent==2);
    n.ble=true;stalled=true;full_indicators_tick(&n);assert(sent==3 && s_pending);
    rmt_symbol_word_t live[97];memcpy(live,s_symbols,sizeof(live));
    n.ble=false;n.cloud=false;n.reconnecting=true;full_indicators_tick(&n);
    assert(sent==3 && !memcmp(live,s_symbols,sizeof(live)));
    stalled=false;full_indicators_tick(&n);assert(sent==4 && !s_pending && s_last[7]==24);
    n.cloud=true;n.reconnecting=false;fail_stage=4;full_indicators_tick(&n);assert(sent==5 && s_last[7]==24);
    fail_stage=0;full_indicators_tick(&n);assert(sent==6 && s_last[6]==24 && !s_last[7]);
    n.cloud=false;n.reconnecting=true;time_ms=1250;full_indicators_tick(&n);
    assert(sent==7 && s_last[7]>1 && s_last[7]<24);uint8_t fading=s_last[7];
    time_ms=1750;full_indicators_tick(&n);assert(sent==8 && s_last[7]>0 && s_last[7]<fading);
    full_indicators_tick(&n);assert(sent==8); /* Same brightness is not retransmitted. */
    inverter_online=true;
    full_indicators_tick(NULL);
    assert(sent==9 && s_last[0]==12 && s_last[1]==24 && s_last[2]==0);
    for(unsigned i=3;i<12;i++)assert(!s_last[i]);
    full_indicators_set_brightness(80);full_indicators_tick(NULL);
    assert(sent==10 && s_last[0]==40 && s_last[1]==80);
    full_indicators_set_brightness(0);full_indicators_tick(NULL);
    assert(sent==11);
    for(unsigned i=0;i<12;i++)assert(!s_last[i]);
    /* DIN to DOUT: RUN blue, CHG breathing blue, NET green, ALM yellow. */
    battery_only=true;full_indicators_set_brightness(24);time_ms=1250;
    n=(panel_led_network_t){.cloud=true,.local_warning=true};full_indicators_tick(&n);
    assert(s_last[5]>0 && s_last[5]<24);
    const uint8_t expected[]={0,0,24,0,0,s_last[5],24,0,0,12,24,0};
    assert(!memcmp(s_last,expected,sizeof(expected)));
    for(unsigned bit=0;bit<96;bit++)
        assert(s_symbols[bit].duration0==((expected[bit/8]&(0x80U>>(bit%8)))?14:7));
    puts("PASS indicator hardware: DIN RUN/CHG/NET/ALM in GRB, RMT timing and safe retry");return 0;
}
