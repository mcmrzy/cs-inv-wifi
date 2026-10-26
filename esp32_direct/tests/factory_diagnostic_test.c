#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
static atomic_uint keys,diagnostic_tick;
static atomic_bool display_test;
static unsigned pulses;
static bool busy;
static struct{unsigned length;uint8_t payload[1];}request;
static bool ota_service_is_busy(void){return busy;}
static int64_t esp_timer_get_time(void){return 1234000;}
static void direct_buzzer_pulse(void){pulses++;}
static void panel_beep(void){pulses++;}
static struct{bool dirty;}s_ui;
static unsigned inverter_actions;
static void key_event(bool factory_testing){
 for(unsigned i=0;i<1;i++){
#include "../tmp/factory_key_guard_under_test.inc"
  inverter_actions++;
 }
}
static void lease(uint32_t tick){
#include "../tmp/factory_diagnostic_lease_under_test.inc"
}
static const char *run(void){const char *result="OK";
#include "../tmp/factory_diagnostic_under_test.inc"
 return result;
}
int main(void){
 request.length=1;request.payload[0]=2;
 assert(!strcmp(run(),"OK")&&atomic_load(&display_test)&&pulses==1);
 assert(atomic_load(&diagnostic_tick)==1234);
 busy=true;assert(!strcmp(run(),"BUSY")&&pulses==1);
 busy=false;request.payload[0]=3;assert(!strcmp(run(),"INVALID_ARGUMENT")&&pulses==1);
 request.payload[0]=0;assert(!strcmp(run(),"OK")&&!atomic_load(&display_test));
 key_event(true);assert(inverter_actions==0&&pulses==2&&s_ui.dirty);
 key_event(false);assert(inverter_actions==1&&pulses==2);
 atomic_store(&display_test,true);atomic_store(&diagnostic_tick,0);
 lease(60000);assert(atomic_load(&display_test));lease(60001);assert(!atomic_load(&display_test));
 puts("PASS actual diagnostic command: Device/Factory/Production, OTA guard, invalid payload, reversible stop");
}
