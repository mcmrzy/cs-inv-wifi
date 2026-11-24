#include "status_led.h"

void panel_indicators_apply_network(const panel_led_network_t *net, panel_led_t out[PANEL_LED_COUNT])
{
    if (!net) return;
    if (net->local_warning && out[LED_ALM].color != LED_RED)
        out[LED_ALM] = (panel_led_t){LED_YELLOW, LED_STEADY};

    panel_led_t led = {LED_OFF, LED_STEADY};
    if (net->ble) led = (panel_led_t){LED_BLUE, LED_STEADY};
    else if (net->cloud) led = (panel_led_t){LED_GREEN, LED_STEADY};
    else if (net->provisioning) led = (panel_led_t){LED_BLUE, LED_BREATHE};
    else if (net->enabled && net->reconnecting) led = (panel_led_t){LED_RED, LED_BREATHE};
    else if (net->wifi) led = (panel_led_t){LED_YELLOW, LED_STEADY};
    out[LED_NET] = led;
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
