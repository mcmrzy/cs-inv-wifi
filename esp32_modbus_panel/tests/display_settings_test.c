#include "panel_display_settings.h"
#include <assert.h>
#include <stdio.h>
int main(void){
    panel_display_settings_t v=panel_display_preferences(),next;
    assert(v.brightness==100 && panel_display_timeout_ms(v)==60000);
    assert(panel_feedback_beep_ms(PANEL_BEEP_OFF)==0);
    assert(panel_feedback_beep_ms(PANEL_BEEP_SHORT)==70);
    assert(panel_feedback_beep_ms(PANEL_BEEP_NORMAL)==70);
    assert(panel_feedback_beep_ms(PANEL_BEEP_LONG)==70);
    assert(panel_feedback_beep_percent(PANEL_BEEP_OFF)==0);
    assert(panel_feedback_beep_percent(PANEL_BEEP_LOW)==25);
    assert(panel_feedback_beep_percent(PANEL_BEEP_MEDIUM)==60);
    assert(panel_feedback_beep_percent(PANEL_BEEP_HIGH)==100);
    assert(!panel_display_should_sleep(59999,0,v,true,false));
    assert(panel_display_should_sleep(60000,0,v,true,false));
    assert(!panel_display_should_sleep(70000,0,v,true,true));
    assert(!panel_display_should_sleep(70000,0,v,false,false));
    assert(!panel_display_should_sleep(20,0xfffffff0U,v,true,false));
    assert(panel_display_should_sleep(60000,0xfffffff0U,v,true,false));
    v.timeout_index=0;assert(!panel_display_should_sleep(1000000,0,v,true,false));
    for(unsigned i=0;i<6;i++){v.timeout_index=(uint8_t)i;assert(panel_display_unpack(panel_display_pack(v),&next));assert(next.timeout_index==i);}
    assert(!panel_display_unpack(0,&next));assert(!panel_display_unpack(0x0764,&next));
    assert(panel_display_duty(80,false,true)==818 && panel_display_duty(80,true,true)==0);
    assert(panel_display_duty(80,false,false)==205 && panel_display_duty(80,true,false)==1023);
    assert(panel_display_duty(100,false,true)==1023);
    puts("PASS display settings: PWM duty/polarity, persisted bounds, timeout/never, force-awake and wrap-safe idle");return 0;
}
