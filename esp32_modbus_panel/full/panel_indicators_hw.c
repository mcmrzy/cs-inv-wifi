#include "panel_indicators_hw.h"
#include "status_led_rmt.h"
#include <stdatomic.h>
#ifdef PANEL_INDICATORS_HOST
#include "indicator_test_sdk.h"
#else
#include "panel_bus.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"
static const char *TAG = "panel_led";
#endif

static atomic_uchar s_brightness = CONFIG_PANEL_LED_BRIGHTNESS;
static panel_snapshot_t s_snapshot;
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

bool full_indicators_init(int gpio)
{
    return status_led_init(gpio);
}

void full_indicators_set_brightness(uint8_t brightness)
{
    atomic_store(&s_brightness,brightness);
}
void full_indicators_tick(const panel_led_network_t *network)
{
    if(!status_led_is_ready())return;
    uint32_t now=(uint32_t)(esp_timer_get_time()/1000);
    bool valid=panel_bus_snapshot(&s_snapshot) && s_snapshot.context_generation==panel_bus_context_generation();
    panel_led_t leds[4];uint8_t pixels[12];
    panel_indicators_policy(valid?&s_snapshot:NULL,network,now,leds);
    report_chg(&leds[LED_CHG],now,valid);
    const panel_led_t chain[]={leds[LED_RUN],leds[LED_CHG],leds[LED_NET],leds[LED_ALM]};
    panel_indicators_grb(chain,now,atomic_load(&s_brightness),pixels);
    status_led_send(pixels);
}
