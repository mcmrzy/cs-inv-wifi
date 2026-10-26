#include "direct_ui.h"
#include "onboarding_state.h"
#include "cloud_binding.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define WIFI_DEFAULT_SSID ""
static direct_ui_state_t s_ui;
static direct_ui_data_t s_ui_data;
static char s_pair_pin[7],s_dismissed_sn[33];
static direct_ui_page_t s_phone_return;
static bool s_has_wifi,s_bound,s_have_previous,s_blank;
static bool s_pair_ready,s_pair_loaded;
static uint32_t s_pair_refresh_ms,clock_ms;
static char g_device_sn[]="H1ZZX0013900001P";
static unsigned reads,wifi_reads;
static bool available=true,wifi,alarm;
static cloud_binding_state_t binding;
static uint32_t now_ms(void){return clock_ms;}
static cloud_binding_state_t cloud_binding_get_stub(void){return binding;}
#define cloud_binding_get cloud_binding_get_stub
static bool storage_has_wifi_config(void){wifi_reads++;return wifi;}
static bool factory_identity_pairing(const char *sn,char *pin){assert(!strcmp(sn,g_device_sn));reads++;if(available)strcpy(pin,"001234");return available;}
bool direct_ui_has_alarm(const direct_ui_data_t *data){(void)data;return alarm;}
#include "../tmp/onboarding_panel_under_test.inc"
int main(void){
 s_ui.page=UI_HOME;
 onboarding_inputs_t i=panel_onboarding_inputs(false);
 assert(i.identity_ready&&!i.has_wifi&&!i.editing&&reads==1&&wifi_reads==1);
 for(clock_ms=20;clock_ms<1000;clock_ms+=20)panel_onboarding_inputs(false);
 assert(reads==1&&wifi_reads==1);
 available=false;wifi=true;binding=CLOUD_BINDING_BOUND;
 i=panel_onboarding_inputs(false);
 assert(!i.identity_ready&&i.has_wifi&&i.bound&&i.binding_known&&reads==2&&!s_pair_pin[0]);
 alarm=true;assert(panel_onboarding_inputs(false).alarm);
 assert(panel_onboarding_inputs(true).factory);
 s_ui_data.ota_busy=true;assert(panel_onboarding_inputs(false).ota);
 s_ui.page=UI_CONFIG;assert(panel_onboarding_inputs(false).editing);
 s_ui.page=UI_PHONE_SETUP;s_phone_return=UI_SUBMENU;s_ui.submenu_selected=9;s_have_previous=true;
 panel_onboarding_close();assert(s_ui.page==UI_SUBMENU&&s_ui.selected==9&&s_ui.dirty&&!s_have_previous);
 (void)s_blank;
 puts("PASS actual panel inputs: bounded identity reads, live priority, missing PIN, exact menu return");
}
