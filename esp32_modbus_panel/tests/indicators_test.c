#include "panel_indicators.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static panel_snapshot_t s;
static panel_led_network_t net;
static panel_led_t leds[4];
static void put(uint16_t reg,uint16_t value){assert(panel_store(&s,reg,value,s.now_ms));}
static void base(void)
{
    memset(&s,0,sizeof(s));memset(&net,0,sizeof(net));s.now_ms=100;
    put(0x42,3);put(0x43,2|(1U<<6)|(3U<<8));put(0x44,2);
    put(0x46,0);put(0x47,0);put(0x48,0);put(0x49,0);put(0x81,(uint16_t)-10);
}
static void build(void){panel_indicators_policy(&s,&net,s.now_ms,leds);}
static void battery_direction_regression(void)
{
    const panel_profile_t profiles[]={PANEL_PROFILE_2KU,PANEL_PROFILE_3KU,PANEL_PROFILE_6KU,PANEL_PROFILE_12KU};
    for(unsigned i=0;i<sizeof(profiles)/sizeof(profiles[0]);i++){
        base();s.config.profile_override=profiles[i];
        unsigned full=profiles[i]==PANEL_PROFILE_3KU?3:4;
        unsigned charge=profiles[i]==PANEL_PROFILE_3KU?2:3;
        unsigned discharge=profiles[i]==PANEL_PROFILE_3KU?1:2;
        put(0x42,5);put(0x43,(1U<<6)|(full<<8));
        put(0x44,(1U<<8)|(1U<<9));
        /* A full flag must never override even the smallest reported discharge. */
        put(0x81,1);build();
        assert(leds[LED_CHG].color==LED_BLUE && leds[LED_CHG].pattern==LED_BREATHE);
        /* While current rounds to zero, explicit discharge flow still wins. */
        put(0x81,0);build();assert(leds[LED_CHG].color==LED_BLUE);
        put(0x43,(1U<<6)|(charge<<8));build();assert(leds[LED_CHG].color==LED_BLUE);
        /* A real charge reversal takes effect without waiting for terminal flags. */
        put(0x81,(uint16_t)-1);put(0x44,1U<<5);build();
        assert(leds[LED_CHG].color==LED_GREEN && leds[LED_CHG].pattern==LED_BREATHE);
        put(0x43,(1U<<6)|(full<<8));put(0x81,0);put(0x44,0);build();
        assert(leds[LED_CHG].color==LED_GREEN && leds[LED_CHG].pattern==LED_STEADY);
        /* A cached charging flag is insufficient when the current read is missing. */
        put(0x43,(1U<<6)|(charge<<8));panel_invalidate(&s,0x81);build();
        assert(leds[LED_CHG].color==LED_OFF);
        s.now_ms=PANEL_FRESH_MS+101;
        put(0x42,5);put(0x43,(1U<<6)|(full<<8));put(0x48,0);put(0x49,0);
        put(0x81,0);panel_store(&s,0x81,0,100);build();
        assert(leds[LED_CHG].color==LED_OFF);
        /* Without directional flow, the fresh discharge terminal flag remains useful. */
        put(0x81,0);put(0x43,(1U<<6)|(discharge<<8));put(0x44,0);build();
        assert(leds[LED_CHG].color==LED_BLUE);
    }
    base();s.config.profile_override=PANEL_PROFILE_3KU;
    put(0x42,5);put(0x43,(1U<<6)|(2U<<8));put(0x81,0);
    put(0x44,(1U<<5)|(1U<<8));put(0x85,3);build();
    assert(leds[LED_CHG].color==LED_BLUE && leds[LED_CHG].pattern==LED_BREATHE);
    put(0x85,(uint16_t)-3);build();
    assert(leds[LED_CHG].color==LED_GREEN && leds[LED_CHG].pattern==LED_BREATHE);
    put(0x42,2);put(0x85,0);put(0x44,0);build();
    assert(leds[LED_CHG].color==LED_BLUE && leds[LED_CHG].pattern==LED_STEADY); /* Connected idle battery. */
    put(0x44,1U<<5);build();assert(leds[LED_CHG].color==LED_BLUE && leds[LED_CHG].pattern==LED_STEADY);
    put(0x44,0);put(0x85,3);panel_store(&s,0x85,3,1);build();
    assert(leds[LED_CHG].color==LED_BLUE && leds[LED_CHG].pattern==LED_STEADY); /* Do not mix different acquisition times. */
    puts("PASS CHG direction: four models, 0.1A reversal, zero-current discharge flow, full/charge conflict and missing/stale current");
}
int main(void)
{
    battery_direction_regression();
    base();s.detected_profile=PANEL_PROFILE_3KU;
    put(0x43,2|(1U<<6)|(3U<<8));put(0x81,0);build();
    assert(leds[LED_CHG].color==LED_GREEN && leds[LED_CHG].pattern==LED_STEADY);
    put(0x43,2|(1U<<6)|(4U<<8));build();
    assert(leds[LED_CHG].color==LED_YELLOW && leds[LED_CHG].pattern==LED_BREATHE);
    base();build();assert(leds[LED_RUN].color==LED_GREEN);
    assert(leds[LED_CHG].color==LED_GREEN && leds[LED_CHG].pattern==LED_BREATHE);
    assert(leds[LED_ALM].color==LED_OFF && leds[LED_NET].color==LED_OFF);
    net.local_warning=true;build();assert(leds[LED_ALM].color==LED_YELLOW);
    put(0x47,0x8000);build();assert(leds[LED_ALM].color==LED_RED);
    base();build();
    put(0x42,5);put(0x43,(1U<<6)|(2U<<8));put(0x81,20);build();
    assert(leds[LED_RUN].color==LED_BLUE && leds[LED_CHG].color==LED_BLUE);
    put(0x42,4);build();assert(leds[LED_RUN].color==LED_BLUE);
    put(0x43,2|(1U<<6)|(2U<<8));put(0x44,1U<<7);build();assert(leds[LED_RUN].color==LED_GREEN);
    put(0x42,2);build();assert(leds[LED_RUN].color==LED_YELLOW && leds[LED_RUN].pattern==LED_STEADY);
    put(0x42,1);build();assert(leds[LED_RUN].color==LED_YELLOW && leds[LED_RUN].pattern==LED_BREATHE);
    put(0x42,8);build();assert(leds[LED_RUN].color==LED_OFF);
    put(0x42,7);build();assert(leds[LED_RUN].color==LED_OFF && leds[LED_ALM].color==LED_RED);
    base();put(0x43,2|(1U<<6)|(4U<<8));put(0x81,0);build();
    assert(leds[LED_CHG].color==LED_GREEN && leds[LED_CHG].pattern==LED_STEADY);
    put(0x81,10);build();assert(leds[LED_CHG].color==LED_BLUE); /* Full flag alone cannot hide discharge. */
    put(0x81,0);put(0x43,2|(1U<<6)|(3U<<8));build();assert(leds[LED_CHG].color==LED_BLUE && leds[LED_CHG].pattern==LED_STEADY);
    put(0x43,2|(1U<<6));build();assert(leds[LED_CHG].color==LED_OFF);
    base();put(0x48,1U<<1);build();
    assert(leds[LED_CHG].color==LED_YELLOW && leds[LED_CHG].pattern==LED_BREATHE);
    assert(leds[LED_ALM].color==LED_YELLOW);
    put(0x46,1U<<3);build();assert(leds[LED_ALM].color==LED_RED && leds[LED_RUN].color==LED_OFF);
    s.now_ms=7000;build();assert(leds[LED_RUN].color==LED_OFF && leds[LED_CHG].color==LED_OFF);
    assert(leds[LED_ALM].color==LED_RED); /* Never declare old fault cleared just because link aged out. */
    base();s.now_ms=7000;build();assert(leds[LED_RUN].color==LED_OFF && leds[LED_ALM].color==LED_YELLOW);
    panel_indicators_policy(NULL,&net,100,leds);assert(leds[LED_RUN].color==LED_OFF && leds[LED_ALM].color==LED_OFF);
    panel_indicators_policy(NULL,&net,6000,leds);assert(leds[LED_ALM].color==LED_YELLOW);
    base();net.enabled=true;net.provisioning=true;build();assert(leds[LED_NET].color==LED_BLUE && leds[LED_NET].pattern==LED_BREATHE);
    net.wifi=true;net.provisioning=false;build();assert(leds[LED_NET].color==LED_YELLOW);
    net.reconnecting=true;build();assert(leds[LED_NET].color==LED_RED && leds[LED_NET].pattern==LED_BREATHE);
    net.cloud=true;build();assert(leds[LED_NET].color==LED_GREEN);
    net.ble=true;build();assert(leds[LED_NET].color==LED_BLUE && leds[LED_NET].pattern==LED_STEADY);
    memset(&net,0,sizeof(net));build();assert(leds[LED_NET].color==LED_OFF);
    uint8_t grb[12];memset(leds,0,sizeof(leds));
    leds[0]=(panel_led_t){LED_GREEN,LED_BREATHE};leds[1]=(panel_led_t){LED_BLUE,LED_STEADY};
    leds[2]=(panel_led_t){LED_YELLOW,LED_BREATHE};leds[3]=(panel_led_t){LED_RED,LED_STEADY};
    panel_indicators_grb(leds,0,24,grb);
    const uint8_t expected[]={1,0,0, 0,0,24, 1,1,0, 0,24,0};assert(!memcmp(grb,expected,12));
    panel_indicators_grb(leds,250,24,grb);uint8_t rising=grb[0];assert(rising>1 && rising<24);
    panel_indicators_grb(leds,750,24,grb);assert(grb[0]>rising && grb[0]<24);
    panel_indicators_grb(leds,1000,24,grb);assert(grb[0]==24 && grb[6]==12 && grb[7]==24);
    panel_indicators_grb(leds,1750,24,grb);assert(grb[0]==rising);
    /* Check the transmitted RGB bytes, including formerly fast-flashing
     * yellow: a smooth symmetric 2s envelope, no black interval or hard edge. */
    const uint8_t brightnesses[]={1,24,255};
    for(unsigned b=0;b<sizeof(brightnesses);b++){
        uint8_t ceiling=brightnesses[b],previous=1;
        for(uint32_t t=0;t<=2000;t+=20){
            panel_indicators_grb(leds,t,ceiling,grb);
            assert(grb[0]>=1 && grb[0]<=ceiling);
            assert(grb[6]==(grb[0]+1U)/2U && grb[0]==grb[7]);
            assert(grb[1]==0 && grb[2]==0 && grb[8]==0);
            assert(grb[5]==ceiling && grb[10]==ceiling);
            int step=(int)grb[0]-previous;
            assert(step>=-8 && step<=8);
            uint8_t symmetric[12];panel_indicators_grb(leds,2000-t,ceiling,symmetric);
            assert(grb[0]==symmetric[0]);
            previous=grb[0];
        }
    }
    panel_indicators_grb(leds,1000,0,grb);for(unsigned i=0;i<12;i++)assert(grb[i]==0);
    memset(grb,0,12);grb[0]=0x80;panel_led_pulse_t pulses[97];
    assert(panel_indicators_encode(grb,pulses,97)==97);
    assert(pulses[0].high==14 && pulses[0].low==11 && pulses[1].high==7 && pulses[1].low==18);
    assert(pulses[96].high==3000 && pulses[96].low==3000);
    assert(!panel_indicators_encode(grb,pulses,96));
    puts("PASS indicators: RUN/CHG/NET/ALM mapping, battery direction/full/limits, fault priority, unknown/stale, network priority, smooth 2s breathing, GRB order and WS2812 timing");
    return 0;
}
