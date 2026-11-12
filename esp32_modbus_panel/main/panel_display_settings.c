#include "panel_display_settings.h"
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#else
#define CONFIG_PANEL_LED_BRIGHTNESS 24
#endif
static panel_display_settings_t saved={PANEL_DISPLAY_DEFAULT_BRIGHTNESS,2};
static panel_feedback_settings_t feedback={CONFIG_PANEL_LED_BRIGHTNESS,PANEL_BEEP_HIGH};
panel_feedback_settings_t panel_feedback_preferences(void){return feedback;}
void panel_feedback_set_preferences(panel_feedback_settings_t settings){feedback=settings;}
unsigned panel_feedback_beep_ms(unsigned mode)
{
    return mode==PANEL_BEEP_OFF?0:70;
}
unsigned panel_feedback_beep_percent(unsigned mode)
{
    static const unsigned level[]={0,25,60,100};
    return mode<PANEL_BEEP_COUNT?level[mode]:100;
}
panel_display_settings_t panel_display_preferences(void){return saved;}
void panel_display_set_preferences(panel_display_settings_t v){if(panel_display_valid(v))saved=v;}
bool panel_display_valid(panel_display_settings_t v){return v.brightness>=10 && v.brightness<=100 && v.timeout_index<PANEL_DISPLAY_TIMEOUT_COUNT;}
uint16_t panel_display_pack(panel_display_settings_t v){return (uint16_t)(v.brightness|((uint16_t)v.timeout_index<<8));}
bool panel_display_unpack(uint16_t word,panel_display_settings_t *out)
{panel_display_settings_t v={(uint8_t)word,(uint8_t)(word>>8)};if(!out || !panel_display_valid(v))return false;*out=v;return true;}
uint32_t panel_display_timeout_ms(panel_display_settings_t v)
{static const uint32_t seconds[]={0,30,60,120,300,600};return panel_display_valid(v)?seconds[v.timeout_index]*1000U:0;}
bool panel_display_should_sleep(uint32_t now,uint32_t last,panel_display_settings_t v,bool enabled,bool blocked)
{uint32_t timeout=panel_display_timeout_ms(v);return enabled && !blocked && timeout && now-last>=timeout;}
uint32_t panel_display_duty(uint8_t brightness,bool asleep,bool high)
{uint32_t duty=asleep?0:(brightness>100?100:brightness)*1023U/100U;return high?duty:1023U-duty;}
