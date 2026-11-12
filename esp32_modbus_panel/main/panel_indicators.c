#include "panel_indicators.h"

static bool fresh(const panel_snapshot_t *s,uint16_t reg,uint16_t *value)
{return panel_fresh(s,reg) && panel_value(s,reg,value);}
static uint16_t cached(const panel_snapshot_t *s,uint16_t reg)
{uint16_t value=0;(void)panel_value(s,reg,&value);return value;}
static panel_led_t steady(panel_led_color_t c){return (panel_led_t){c,LED_STEADY};}
static panel_led_t breathe(panel_led_color_t c){return (panel_led_t){c,LED_BREATHE};}

void panel_indicators_policy(const panel_snapshot_t *s,const panel_led_network_t *net,uint32_t now,panel_led_t out[4])
{
    if(!out)return;
    for(unsigned i=0;i<4;i++)out[i]=steady(LED_OFF);
    uint16_t mode=0,terminal=0,flow=0;
    bool mode_ok=fresh(s,0x42,&mode) && mode<=10;
    bool terminal_ok=fresh(s,0x43,&terminal);
    bool flow_ok=fresh(s,0x44,&flow);
    uint16_t f1=cached(s,0x46),f2=cached(s,0x47),w1=cached(s,0x48),w2=cached(s,0x49);
    uint16_t old_mode;
    bool fault=f1 || f2 || (panel_value(s,0x42,&old_mode) && old_mode==7) ||
        (terminal_ok && ((terminal>>6)&3U)==3U);
    bool alarm_fresh=panel_fresh(s,0x46) && panel_fresh(s,0x47) &&
                     panel_fresh(s,0x48) && panel_fresh(s,0x49);
    bool warning=w1 || w2 || (terminal_ok && ((terminal>>6)&3U)==2U);
    /* A lost link never clears the last observed fault. Missing alarm/status
     * data is a local warning after a five-second startup grace period. */
    if(fault)out[LED_ALM]=steady(LED_RED);
    else if(warning || (now>=5000U && (!mode_ok || !alarm_fresh)))out[LED_ALM]=steady(LED_YELLOW);

    if(mode_ok && !fault){
        if(mode==0 || mode==1 || mode==9 || mode==10)out[LED_RUN]=breathe(LED_YELLOW);
        else if(mode==2)out[LED_RUN]=steady(LED_YELLOW);
        else if(mode>=3 && mode<=6 && terminal_ok && alarm_fresh){
            bool output=((terminal>>6)&3U)==1U;
            bool grid=(terminal&3U)==2U;
            if(output && mode==3 && grid)out[LED_RUN]=steady(LED_GREEN);
            else if(output && mode!=3)
                out[LED_RUN]=steady(grid && flow_ok && (flow&(1U<<7))?LED_GREEN:LED_BLUE);
            else out[LED_RUN]=breathe(LED_YELLOW);
        }
    }
    /* V1.27 0043: battery 0=absent/short,1=low,2=discharge,3=charge,4=full.
     * 0081 is signed 0.1A, POSITIVE discharge / NEGATIVE charge.
     * Match the screen's signed-current direction, including +/-0.1A.
     * Cached current must not turn a terminal flag into a live green light. */
    if(mode_ok && terminal_ok && panel_fresh(s,0x48) && panel_fresh(s,0x49)){
        unsigned battery=(terminal>>8)&7U;
        bool is_3ku=panel_is_3ku(s);
        if(is_3ku){
            /* Map 3KU discharge/charge/full/low to the common LED policy. */
            static const uint8_t battery_states[]={0,2,3,4,1,0,0,0};
            battery=battery_states[battery];
        }
        if(battery>=1 && battery<=4 && !(w1&1U)){
            bool limited=battery==1 || (f1&0x0078U) || (f2&(1U<<9)) ||
                (w1&0x006eU) || (w2&0x007cU);
            uint16_t current_raw=0;bool current_ok=fresh(s,0x81,&current_raw);
            int direction=panel_battery_direction(s);
            if(limited)out[LED_CHG]=breathe(LED_YELLOW);
            else if(current_ok){
                if(direction>0)out[LED_CHG]=breathe(LED_BLUE);
                else if(direction<0)out[LED_CHG]=breathe(LED_GREEN);
                else if(battery==4 && (!flow_ok || !(flow&(1U<<8))))out[LED_CHG]=steady(LED_GREEN);
                else out[LED_CHG]=steady(LED_BLUE); /* Connected, no measured flow. */
            }
        }
    }
    if(net){
        if(net->local_warning && out[LED_ALM].color!=LED_RED)out[LED_ALM]=steady(LED_YELLOW);
        if(net->ble)out[LED_NET]=steady(LED_BLUE);
        else if(net->cloud)out[LED_NET]=steady(LED_GREEN);
        else if(net->provisioning)out[LED_NET]=breathe(LED_BLUE);
        else if(net->enabled && net->reconnecting)out[LED_NET]=breathe(LED_RED);
        else if(net->wifi)out[LED_NET]=steady(LED_YELLOW);
    }
}

static uint8_t breathing_level(uint32_t now,uint8_t ceiling)
{
    if(!ceiling)return 0;
    unsigned phase=now%2000U;
    unsigned ramp=phase<=1000U?phase:2000U-phase;
    unsigned x=(ramp*256U+500U)/1000U;
    /* Smoothstep gives zero slope at both ends, without a float or sine table.
     * Keep one PWM count at the trough so breathing never becomes an off gap. */
    unsigned eased=(x*x*(768U-2U*x)+32768U)>>16;
    return (uint8_t)(1U+((ceiling-1U)*eased+128U)/256U);
}

void panel_indicators_grb(const panel_led_t in[4],uint32_t now,uint8_t brightness,uint8_t out[12])
{
    if(!out)return;
    for(unsigned i=0;i<4;i++){
        out[i*3]=out[i*3+1]=out[i*3+2]=0;
        if(!in)continue;
        uint8_t level=in[i].pattern==LED_BREATHE?breathing_level(now,brightness):brightness;
        /* WS2812 wire order G7..G0 R7..R0 B7..B0. */
        if(in[i].color==LED_GREEN)out[i*3]=level;
        /* Reduce green in yellow so the panel LEDs look warmer. */
        if(in[i].color==LED_YELLOW)out[i*3]=(uint8_t)((level+1U)/2U);
        if(in[i].color==LED_RED || in[i].color==LED_YELLOW)out[i*3+1]=level;
        if(in[i].color==LED_BLUE)out[i*3+2]=level;
    }
}
size_t panel_indicators_encode(const uint8_t grb[12],panel_led_pulse_t *out,size_t capacity)
{
    if(!grb || !out || capacity<PANEL_LED_SYMBOLS)return 0;
    for(unsigned i=0;i<96;i++)
        out[i]=(grb[i/8]&(0x80U>>(i%8)))?(panel_led_pulse_t){14,11}:(panel_led_pulse_t){7,18};
    /* 20MHz: 350/900ns for 0, 700/550ns for 1; 300us low latch. */
    out[96]=(panel_led_pulse_t){3000,3000};return PANEL_LED_SYMBOLS;
}
