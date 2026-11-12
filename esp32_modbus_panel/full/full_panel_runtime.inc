#include "full_ui_bridge.h"
#include "full_modbus_map.h"
#include "panel_bus.h"
#include "panel_ui.h"
#include "panel_catalog.h"
#include "panel_lvgl.h"
#include "panel_wifi.h"
#include "panel_firmware_catalog.h"
#include "panel_weather.h"
#include "panel_pairing.h"
#include "panel_display_settings.h"
#include "panel_backlight.h"
#include "panel_indicators_hw.h"
#include "panel_ble_aux.h"
/* Included by direct_panel.c so rendering has one owner and one SPI stream. */
#include "direct_keys.h"
#include "direct_buzzer.h"
#include "direct_link.h"
#include "direct_display_sample.h"
#include "direct_display_events.h"
#include "direct_home.h"
#include "direct_ui.h"
#include "panel_preferences.h"
#include "nvs.h"
#include "config.h"
#include "mqtt_handler.h"
#include "app_network.h"
#include "ota_service.h"
#include "factory_identity.h"
#include "factory_service.h"
#include "onboarding_state.h"
#include "onboarding_qr.h"
#include "cloud_binding.h"
#include "nvs_storage.h"
#include "wifi_sta.h"
#include "ble_prov.h"
#include "esp_system.h"
#include "esp_attr.h"
#include <time.h>
#include <sys/time.h>

static direct_ui_state_t s_ui;
static bool s_modbus_active,s_modbus_drawn;
static panel_ui_t s_modbus_ui;
static panel_page_t s_modbus_root;
static panel_snapshot_t s_modbus_snapshot;
static panel_view_t s_modbus_view,s_modbus_previous;
static direct_ui_data_t s_ui_data;
static direct_display_sample_t s_display_sample;
static direct_dsp_status_t s_display_status;
static uint32_t s_display_status_ms,s_display_status_blocked_ms;
static bool s_display_status_valid,s_display_status_waiting;
static direct_ui_view_t s_view;
#include "panel_wifi_ui.inc"
#include "panel_firmware_ui.inc"
#include "panel_weather_ui.inc"
static char s_clock_text[24];
static factory_record_t s_identity;
static onboarding_state_t s_onboarding;
static onboarding_qr_t s_onboarding_qr;
static char s_pair_pin[7],s_dismissed_sn[33];
static direct_ui_page_t s_phone_return=UI_HOME;
static bool s_has_wifi,s_bound;
static bool s_have_previous, s_blank;
static uint32_t s_last_input;
static direct_lcd_recovery_t s_recovery;
static bool s_manual_recovery_pending;
static bool s_wifi_rssi_valid,s_wifi_was_connected;
static int16_t s_wifi_rssi;
static uint32_t s_wifi_rssi_ms;
static bool s_pair_ready,s_pair_loaded;
static char s_pair_sn[17];
static uint32_t s_pair_refresh_ms;
static onboarding_inputs_t panel_onboarding_inputs(bool testing){
 cloud_binding_state_t binding=cloud_binding_get();
 uint32_t now=now_ms();
 /* A changed SN must never leave the prior PIN or QR visible for a poll interval. */
 if(strcmp(s_pair_sn,g_device_sn)){
  memset(s_pair_pin,0,sizeof(s_pair_pin));memset(&s_onboarding_qr,0,sizeof(s_onboarding_qr));
  s_pair_ready=false;s_pair_loaded=false;
  snprintf(s_pair_sn,sizeof(s_pair_sn),"%.16s",g_device_sn);
 }
 /* Recheck network configuration at a bounded rate; PIN uses the current SN. */
 if(!s_pair_loaded || (uint32_t)(now-s_pair_refresh_ms)>=1000U){
  s_pair_loaded=true;s_pair_refresh_ms=now;
  s_has_wifi=storage_has_wifi_config()||WIFI_DEFAULT_SSID[0];
  s_pair_ready=panel_pairing_pin(g_device_sn,s_pair_pin);
  if(!s_pair_ready)s_pair_pin[0]=0;
 }
 s_bound=binding==CLOUD_BINDING_BOUND;
 onboarding_inputs_t input={.has_wifi=s_has_wifi,.bound=s_bound,.binding_known=binding!=CLOUD_BINDING_UNKNOWN,
  .identity_ready=s_pair_ready,.dismissed=!strcmp(s_dismissed_sn,g_device_sn),
  .alarm=direct_ui_has_alarm(&s_ui_data),.ota=s_ui_data.ota_busy,.factory=testing,
  .editing=s_ui.editing||s_ui.pending||s_ui.page==UI_REVIEW||
    (s_ui.page!=UI_HOME&&s_ui.page!=UI_PHONE_SETUP)};
 return input;
}
static void panel_onboarding_close(void){
 s_ui.page=s_phone_return;if(s_phone_return==UI_SUBMENU)s_ui.selected=s_ui.submenu_selected;s_ui.dirty=true;s_have_previous=false;
}
static bool s_lcd_test_inverted=true;
static uint16_t s_local_alarm=1, s_local_buzzer=1, s_local_blank=0;
/* One atomic NVS style value: bit0 light, bit1 Chinese. Default Chinese/dark. */
static uint8_t s_saved_style=2, s_preview_style=2;
static panel_display_settings_t s_saved_display={PANEL_DISPLAY_DEFAULT_BRIGHTNESS,2},
    s_preview_display={PANEL_DISPLAY_DEFAULT_BRIGHTNESS,2};
static int16_t s_timezone=480;
static DMA_ATTR uint16_t s_strip_pixels[LCD_W*16] __attribute__((aligned(4)));

static void panel_beep(void) { direct_buzzer_pulse(); }
static bool panel_alarm_feedback(bool *previous) {
    bool alarm=direct_ui_has_alarm(&s_ui_data);
    if(alarm && !*previous && s_local_alarm)panel_beep();
    *previous=alarm;return alarm;
}
static void panel_set_blank(bool blank) {
    if(s_blank!=blank){
        /* Keep scanout running when the board can blank the backlight itself. */
        if(CONFIG_DIRECT_LCD_BL_PIN<0)command(blank?0x28:0x29);
        s_blank=blank;
        s_have_previous=false;
        s_modbus_drawn=false;
    }
    full_backlight_set(s_preview_display.brightness,s_blank);
}
static bool panel_wake_key(void) {
    s_last_input=now_ms();
    if(!s_blank)return false;
    panel_set_blank(false);return true; /* First key wakes, never activates a control. */
}
static bool panel_key_current(uint32_t timestamp) {
    /* A press buffered during a long flash/driver operation belongs to the old
     * screen. Never replay it into a confirmation dialog after recovery. */
    return now_ms()-timestamp<=1000U;
}
static void panel_discard_preview(void) {
    s_preview_style=s_saved_style;s_preview_display=s_saved_display;
    full_backlight_set(s_preview_display.brightness,s_blank);
}
static void panel_idle_tick(bool blocked) {
    if(blocked){s_last_input=now_ms();panel_set_blank(false);return;}
    if(panel_display_should_sleep(now_ms(),s_last_input,s_preview_display,s_local_blank!=0,false))panel_set_blank(true);
}
static void panel_apply_preferences(const panel_preferences_t *p,bool preview) {
    static bool feedback_configured;
    panel_feedback_settings_t feedback=p->feedback;
    if(!p->buzzer)feedback.beep_mode=PANEL_BEEP_OFF;
    panel_feedback_settings_t previous=panel_feedback_preferences();
    if(!feedback_configured || previous.beep_mode!=feedback.beep_mode){
        direct_buzzer_set_duration_ms(70);
        direct_buzzer_set_volume((uint8_t)panel_feedback_beep_percent(feedback.beep_mode));
        direct_buzzer_set_enabled(p->buzzer!=0);
    }
    if(!feedback_configured || previous.led_brightness!=feedback.led_brightness)
        full_indicators_set_brightness(feedback.led_brightness);
    panel_feedback_set_preferences(feedback);
    feedback_configured=true;
    s_local_alarm=p->alarm;s_local_buzzer=p->buzzer;s_local_blank=p->blank;
    s_saved_style=p->style;s_saved_display=p->display;s_timezone=p->timezone;
    panel_display_set_preferences(s_saved_display);
    if(preview){s_preview_style=s_saved_style;s_preview_display=s_saved_display;}
    full_backlight_set(s_preview_display.brightness,s_blank);
}
static bool panel_save_local(unsigned fields) {
    panel_preferences_t next={.alarm=s_local_alarm,.buzzer=s_local_buzzer,.blank=s_local_blank,
        .timezone=s_timezone,.style=s_saved_style,.display=s_saved_display};
    if(!panel_preferences_update(&next,fields))return false;
    full_ui_reload_local_preferences();return true;
}
static void panel_load_local(void) {
    panel_preferences_t stored;
    if(panel_preferences_get(&stored))panel_apply_preferences(&stored,true);
}
/* The admin lock also covers BLE memory handovers. It is a write interlock,
 * not an upgrade screen request. Only real transfer states change pages. */
static bool panel_ota_active(void) {
    OtaSnapshot snapshot;
    if(!ota_service_get_snapshot(&snapshot))return false;
    return (snapshot.state>=OTA_STATE_ACCEPTED && snapshot.state<=OTA_STATE_CANCELLING) ||
           snapshot.state==OTA_STATE_REBOOTING;
}
static void panel_snapshot(void) {
    static uint32_t context_generation;
    uint32_t current_context=full_link_context_generation();
    if(context_generation!=current_context){
        context_generation=current_context;
        memset(&s_display_sample,0,sizeof(s_display_sample));
        memset(&s_display_status,0,sizeof(s_display_status));
        s_display_status_valid=false;s_display_status_waiting=false;
        s_display_status_ms=0;
    }
    memset(&s_ui_data,0,sizeof(s_ui_data));
    s_ui_data.wifi_connected=wifi_sta_is_connected();
    s_ui_data.bluetooth_connected=ble_prov_is_connected();
    panel_weather_snapshot(&s_weather);
    panel_lvgl_weather(&s_weather);
    s_ui_data.network_online=s_ui_data.wifi_connected && mqtt_is_connected();
    uint32_t sampled_ms=now_ms();
    if(s_ui_data.wifi_connected){
        if(!s_wifi_was_connected || sampled_ms-s_wifi_rssi_ms>=1000U){
            s_wifi_rssi=wifi_sta_get_rssi();s_wifi_rssi_ms=sampled_ms;
            s_wifi_rssi_valid=s_wifi_rssi<0 && s_wifi_rssi>=-127;
        }
        s_ui_data.wifi_rssi_valid=s_wifi_rssi_valid;s_ui_data.wifi_rssi_dbm=s_wifi_rssi;
    }
    s_wifi_was_connected=s_ui_data.wifi_connected;
    panel_preferences_t preferences;
    if(panel_preferences_get(&preferences))panel_apply_preferences(&preferences,s_ui.page!=UI_APPEARANCE);
    s_ui_data.theme_light=(s_preview_style&1)!=0;
    s_ui_data.language_zh=(s_preview_style&2)!=0;
    cloud_binding_state_t binding=cloud_binding_get();
    s_ui_data.phone_setup_needed=!s_has_wifi || binding==CLOUD_BINDING_UNBOUND;
    if(arm_poll_take_param()){
        s_ui_data.polls=g_arm_stat.poll_count;s_ui_data.errors=g_arm_stat.err_count;
        arm_poll_give_param();
    }
    bool dsp_paused=panel_ota_active() || direct_dsp_boot_active();
    if(dsp_paused){
        s_display_status_valid=false;s_display_status_waiting=true;
        s_display_status_blocked_ms=now_ms();
    }else{
        direct_dsp_status_t status;uint32_t status_ms;
        if(direct_dsp_get_status_sample(&status,&status_ms) &&
           (!s_display_status_waiting || (int32_t)(status_ms-s_display_status_blocked_ms)>0)){
            s_display_status=status;s_display_status_ms=status_ms;
            s_display_status_valid=true;s_display_status_waiting=false;
        }
    }
    s_ui_data.status_valid=s_display_status_valid && now_ms()-s_display_status_ms<5000U;
    s_ui_data.dsp_status_held=s_display_status_valid && !s_ui_data.status_valid;
    if(s_display_status_valid)s_ui_data.status=s_display_status;
    uint32_t measured_at=0,display_age=UINT32_MAX;
    RunParamDef measurement={0};
    bool received=!dsp_paused && direct_dsp_get_measurement(&measurement,&measured_at);
    bool display_available=direct_display_sample_update(&s_display_sample,now_ms(),
        dsp_paused,received,&measurement,measured_at,&s_ui_data.run,&display_age);
    s_ui_data.measurement_age_ms=display_age;
    s_ui_data.dsp_online=display_available && display_age<2000U;
    s_ui_data.dsp_display_held=display_available && !s_ui_data.dsp_online;
    s_ui_data.bms_age_ms=UINT32_MAX;
    s_ui_data.bms_protocol=panel_bms_protocol_get();
    s_ui_data.bms_detection=panel_bms_detection();
    s_ui_data.bms_no_reply=panel_bms_probe_failed();
    s_ui_data.bms_received=direct_bms_get_snapshot(&s_ui_data.bms,&s_ui_data.bms_age_ms);
    s_ui_data.bms_online=s_ui_data.bms_received && s_ui_data.bms_age_ms<5000U;
    if(!s_modbus_active && (s_ui.page==UI_HOME || s_ui.page==UI_BMS) && panel_bus_snapshot(&s_modbus_snapshot))
        full_mb_battery(&s_modbus_snapshot,&s_ui_data.inverter_battery);
    const unsigned local[]={14,15,41};const uint16_t values[]={s_local_alarm,s_local_blank,s_local_buzzer};
    for(unsigned i=0;i<3;i++){s_ui_data.config[local[i]]=values[i];s_ui_data.valid[local[i]]=true;}
    s_ui_data.version=FIRMWARE_VERSION;s_ui_data.sn=g_device_sn;
    ota_service_get_snapshot(&s_ui_data.ota);s_ui_data.ota_busy=panel_ota_active();
    s_ui_data.model="CS-MB-PANEL";s_ui_data.hardware="UNSET";
    s_ui_data.factory_keys=factory_key_mask();s_ui_data.factory_write_enabled=CONFIG_DIRECT_FACTORY_WRITES;
    s_ui_data.factory_ready=factory_identity_read(&s_identity)==FACTORY_OK;
    if(s_ui_data.factory_ready){
        s_ui_data.model=s_identity.model;s_ui_data.hardware=s_identity.hw_controller;s_ui_data.factory_locked=s_identity.locked;
#if CONFIG_DIRECT_IDENTITY_PRODUCTION
        s_ui_data.sn=s_identity.sn;
#endif
    }
    time_t utc=time(NULL);struct tm local_time={0};
    if(utc>1577836800){
        s_ui_data.clock_valid=true;
        time_t local=utc+s_timezone*60;gmtime_r(&local,&local_time);
        snprintf(s_clock_text,sizeof(s_clock_text),"%02d-%02d %02d:%02d",local_time.tm_mon+1,local_time.tm_mday,local_time.tm_hour,local_time.tm_min);
        s_ui_data.year=local_time.tm_year+1900;s_ui_data.month=local_time.tm_mon+1;s_ui_data.day=local_time.tm_mday;
        s_ui_data.hour=local_time.tm_hour;s_ui_data.minute=local_time.tm_min;s_ui_data.second=local_time.tm_sec;
    }else{
        snprintf(s_clock_text,sizeof(s_clock_text),"Time not synced");
        s_ui_data.year=2026;s_ui_data.month=1;s_ui_data.day=1;
    }
    s_ui_data.clock=s_clock_text;s_ui_data.timezone_minutes=s_timezone;
}
static void panel_write_pixels(int x,int y,int width,int height,uint16_t *pixels)
{
    if(!pixels || x<0 || y<0 || width<1 || height<1 || x+width>LCD_W || y+height>LCD_H ||
       (size_t)width*height>LCD_W*16)return;
    atomic_store(&s_panel_row,(unsigned)y);
    size_t count=(size_t)width*height;
    for(size_t i=0;i<count;i++)pixels[i]=(uint16_t)((pixels[i]>>8)|(pixels[i]<<8));
    window(x,y,width,height);
#if CONFIG_DIRECT_LCD_GPIO_SPI
    size_t bytes=count*sizeof(uint16_t);
    for(size_t at=0;at<bytes;at+=512){
        size_t left=bytes-at;
        tx(true,(const uint8_t *)pixels+at,left<512?left:512);
    }
#else
    tx(true,pixels,count*sizeof(uint16_t));
#endif
}
static bool panel_set_view_inversion(bool requested) {
    if(requested==s_lcd_test_inverted)return true;
    unsigned errors=atomic_load(&s_lcd_tx_errors);
    command(requested?0x21:0x20);
    if(errors!=atomic_load(&s_lcd_tx_errors))return false;
    s_lcd_test_inverted=requested;
    ESP_LOGI(TAG,"LCD_VIEW_TEST inversion=%s, transient only",requested?"ON":"OFF");
    return true;
}
static void panel_render(void) {
    bool ok=panel_set_view_inversion(!(s_ui.page==UI_HELP && s_ui.detail_page==1));
    if(!ok){s_have_previous=false;s_ui.dirty=true;return;}
    unsigned errors=atomic_load(&s_lcd_tx_errors);
    panel_lvgl_show_local(&s_ui,&s_ui_data,&s_view,&s_onboarding_qr,s_pair_pin,s_has_wifi,s_bound);
    panel_lvgl_refresh(now_ms(),!s_have_previous);
    ok=atomic_load(&s_lcd_tx_errors)==errors;
    s_have_previous=ok;s_ui.dirty=!ok;
}

static bool panel_reset_display(void) {
    const unsigned before=atomic_load(&s_lcd_tx_errors);
    atomic_store(&s_first_frame_done,false);
    s_panel_initialized=panel_hardware_initialize();
    if(s_panel_initialized)direct_panel_enable_display();
    s_panel_initialized=s_panel_initialized && atomic_load(&s_lcd_tx_errors)==before;
    panel_set_blank(false);s_last_input=now_ms();s_have_previous=false;s_modbus_drawn=false;
    s_lcd_test_inverted=true;s_ui.dirty=true;
    return s_panel_initialized;
}

static void panel_service_recovery(void) {
    const uint32_t now=now_ms();
    if(!direct_lcd_recovery_due(&s_recovery,now,atomic_load(&s_lcd_tx_errors),
          s_panel_initialized,s_manual_recovery_pending,ota_service_is_busy()))return;
    const bool manual=s_manual_recovery_pending;
    s_manual_recovery_pending=false;
    /* Do not replay navigation collected while the user could not see the UI. */
    uint8_t discarded_key;uint32_t discarded_ms;
    while(direct_keys_get_timed_event(&discarded_key,&discarded_ms)){}
    s_recovery.last_attempt_ms=now;
    if(!manual)s_recovery.auto_attempts++;
    ESP_LOGW(TAG,"LCD_RECOVERY begin source=%s auto_attempts=%u/2",manual?"menu":"spi_error",s_recovery.auto_attempts);
    const bool ok=panel_reset_display();
    s_recovery.last_attempt_ms=now_ms();
    s_recovery.baseline_errors=atomic_load(&s_lcd_tx_errors);
    if(ok)atomic_store(&s_recovery_error_baseline,s_recovery.baseline_errors);
    if(manual){
        snprintf(s_ui.message,sizeof(s_ui.message),"%s",ok?"Screen restored":"Screen reset failed");
        s_ui.dirty=true;
    }
    ESP_LOGW(TAG,"LCD_RECOVERY done ok=%u; full repaint requested",ok);
}
static bool panel_set_time(bool change_clock) {
    const int *f=s_ui.time_fields;
    struct timeval previous_time;
    if(change_clock){
        int64_t utc;
        if(!direct_ui_time_utc(f,&utc) || gettimeofday(&previous_time,NULL)!=0)return false;
        struct timeval next_time={.tv_sec=(time_t)utc};
        if(settimeofday(&next_time,NULL)!=0)return false;
    }
    int16_t previous=s_timezone;s_timezone=(int16_t)f[6];
    if(!panel_save_local(PANEL_PREF_TIME)){
        s_timezone=previous;
        if(change_clock)(void)settimeofday(&previous_time,NULL);
        return false;
    }
    return true;
}
static void panel_save_appearance(direct_ui_event_t event) {
    panel_display_settings_t display;
    if(!panel_display_unpack(event.index,&display))return;
    unsigned fields=PANEL_PREF_DISPLAY;
    unsigned edits=(unsigned)s_ui.time_fields[4];
    unsigned beep=(unsigned)s_ui.time_fields[3];
    panel_preferences_t next={
        .style=(uint8_t)(event.value&3),.display=display,
        .buzzer=beep!=PANEL_BEEP_OFF,
        .feedback={(uint8_t)s_ui.time_fields[2],beep?(uint8_t)beep:PANEL_BEEP_HIGH}
    };
    if(edits&PANEL_FEEDBACK_LED_EDIT)fields|=PANEL_PREF_LED_BRIGHTNESS;
    if(edits&PANEL_FEEDBACK_BEEP_EDIT)fields|=PANEL_PREF_BEEP_MODE;
    if(!panel_preferences_update(&next,fields)){
        panel_discard_preview();
        snprintf(s_ui.message,sizeof(s_ui.message),"Save failed / retry");
        s_ui.dirty=true;
        return;
    }
    s_saved_style=next.style;
    s_saved_display=display;
    s_local_blank=display.timeout_index!=0;
    panel_display_set_preferences(display);
    if(edits)panel_load_local();
    full_ui_reload_local_preferences();
    panel_discard_preview();
    s_ui.appearance_theme=(s_saved_style&1)!=0;
    s_ui.appearance_language=(s_saved_style&2)!=0;
    s_ui.time_fields[0]=s_saved_display.brightness;
    s_ui.time_fields[1]=s_local_blank?s_saved_display.timeout_index:0;
    panel_feedback_settings_t feedback=panel_feedback_preferences();
    s_ui.time_fields[2]=feedback.led_brightness;
    s_ui.time_fields[3]=feedback.beep_mode;
    s_ui.time_fields[4]=0;
    snprintf(s_ui.message,sizeof(s_ui.message),"Appearance saved");
    s_ui.dirty=true;
}
static panel_page_t s_bms_return_root;
static direct_ui_page_t s_bms_return_page;
static uint8_t s_bms_return_detail;
static bool s_bms_return_pending;

static void panel_action(direct_ui_event_t event) {
    if(event.action==FULL_UI_OPEN_MODBUS){
        if(panel_ota_active())return;
        panel_ui_init(&s_modbus_ui);
        s_modbus_root=(panel_page_t)event.index;s_modbus_ui.page=s_modbus_root;
        if(s_modbus_root==PANEL_PAGE_DETAILS)s_modbus_ui.detail=(uint8_t)(event.value%PANEL_DETAIL_PAGES);
        if(s_modbus_root==PANEL_PAGE_DETAIL_GROUPS){
            s_modbus_ui.selected=(uint16_t)(event.value%PANEL_DETAIL_PAGES);
            if(event.value==3 && s_bms_return_pending){
                s_bms_return_pending=false;
                s_ui.page=s_bms_return_page;
                s_modbus_root=s_bms_return_root;
                s_modbus_ui.detail_index_from_page=s_modbus_root==PANEL_PAGE_DETAILS;
                s_modbus_ui.detail_origin=s_bms_return_detail;
            }
        }
        if(s_modbus_root==PANEL_PAGE_SETTINGS){
            s_modbus_ui.setting_group=event.value<PANEL_SETTING_GROUPS?(uint8_t)event.value:0;
            s_modbus_ui.selected=(uint16_t)panel_setting_group_index(s_modbus_ui.setting_group,0);
            if(s_ui.page==UI_BMS_PROTOCOL && s_modbus_ui.setting_group==2){
                s_modbus_ui.battery_limits_page=true;
                const panel_setting_t *type=panel_setting_find(0x4107);
                if(type)s_modbus_ui.selected=(uint16_t)(type-panel_setting(0));
            }
        }
        s_modbus_active=true;s_modbus_drawn=false;
        return;
    }
    if(ota_service_is_busy() && (event.action==UI_ACT_RESTART || event.action==UI_ACT_LOCAL_RESET || event.action==UI_ACT_LCD_RESET ||
            event.action==UI_ACT_APPEARANCE_SAVE || event.action==UI_ACT_SET_TIME || event.action==FULL_UI_SAVE_BMS_PROTOCOL)){
        snprintf(s_ui.message,sizeof(s_ui.message),"Settings busy / retry");s_ui.dirty=true;return;
    }
    if(event.action==UI_ACT_OTA_CANCEL){
        OtaSnapshot snapshot;
        if(!ota_service_get_snapshot(&snapshot) || ota_service_cancel(snapshot.task_id)!=OTA_RESULT_OK)
            snprintf(s_ui.message,sizeof(s_ui.message),"Upgrade cannot be cancelled");
        s_ui.dirty=true;return;
    }
    if(event.action==FULL_UI_DETECT_BMS){
        if(panel_ota_active())snprintf(s_ui.message,sizeof(s_ui.message),"Upgrade busy");
        else if(!panel_bms_detect_start())snprintf(s_ui.message,sizeof(s_ui.message),"Target not ready");
        s_ui.dirty=true;
    }else if(event.action==FULL_UI_SAVE_BMS_PROTOCOL){
        bool saved=panel_bms_protocol_set((panel_bms_protocol_t)event.value);
        snprintf(s_ui.message,sizeof(s_ui.message),"%s",saved?"":"Save failed / retry");
        s_ui.dirty=true;
    }else if(event.action==UI_ACT_APPEARANCE_PREVIEW){s_preview_style=(uint8_t)(event.value&3);
        (void)panel_display_unpack(event.index,&s_preview_display);full_backlight_set(s_preview_display.brightness,s_blank);s_ui.dirty=true;}
    else if(event.action==UI_ACT_APPEARANCE_CANCEL){panel_discard_preview();s_ui.dirty=true;}
    else if(event.action==UI_ACT_APPEARANCE_SAVE)panel_save_appearance(event);
    else if(event.action==UI_ACT_REFRESH){arm_poll_request_config();arm_poll_request_info();}
    else if(event.action==UI_ACT_LCD_RESET)s_manual_recovery_pending=true;
    else if(event.action==UI_ACT_RESTART)esp_restart();
    else if(event.action==UI_ACT_LOCAL_RESET){
        if(panel_preferences_reset()){
            full_ui_reload_local_preferences();panel_load_local();direct_ui_init(&s_ui);
            snprintf(s_ui.message,sizeof(s_ui.message),"Local settings restored");
        }else snprintf(s_ui.message,sizeof(s_ui.message),"Save failed / retry");
        s_ui.dirty=true;
    }else if(event.action==UI_ACT_SET_TIME){
        bool saved=panel_set_time(event.value!=0);
        if(saved)s_ui.editing=false;
        snprintf(s_ui.message,sizeof(s_ui.message),"%s",saved?(event.value?"Time saved":"Time zone saved"):"Invalid date / save failed");
        s_ui.dirty=true;
    }
}
static void full_modbus_panel_tick(void)
{
    static uint32_t last_ota_query;
    if(!panel_bus_snapshot(&s_modbus_snapshot)){
        /* A failed driver start must leave the screen navigable. Empty cache
         * also prevents the UI from admitting inverter settings. */
        memset(&s_modbus_snapshot,0,sizeof(s_modbus_snapshot));
        s_modbus_snapshot.now_ms=now_ms();
    }
    if(s_modbus_ui.page==PANEL_PAGE_DIAG && s_modbus_ui.detail%2 && now_ms()-last_ota_query>5000U){
        panel_bus_request_ota_diagnostics();last_ota_query=now_ms();
    }
    s_modbus_ui.language_en=(s_preview_style&2)==0;
    if(s_modbus_ui.battery_limits_page){
        const direct_bms_measurement_t *b=&s_ui_data.bms;
        bool fresh=s_ui_data.bms_online && full_battery_sample_valid(&s_ui_data) &&
                   s_ui_data.bms_protocol==PANEL_BMS_CSERGY && !b->protection;
        s_modbus_ui.battery_charge_deciamp=fresh?b->charge_max_deciamp:0;
        s_modbus_ui.battery_charge_decivolt=fresh?b->charge_max_decivolt:0;
        s_modbus_ui.battery_discharge_decivolt=fresh?b->discharge_min_decivolt:0;
    }
    panel_ui_sync(&s_modbus_ui,&s_modbus_snapshot);
    uint8_t key;uint32_t timestamp;
    while(direct_keys_get_timed_event(&key,&timestamp)){
        if(!panel_key_current(timestamp))continue;
        unsigned sequence=atomic_fetch_add(&s_panel_key_events,1)+1;
        ESP_LOGW(TAG,"LCD_MB_KEY seq=%u key=%u page=%u edit=%u review=%u pending=%u",
                 sequence,key,s_modbus_ui.page,s_modbus_ui.editing,s_modbus_ui.review,s_modbus_ui.pending);
        factory_note_key(key);
        if(panel_wake_key())continue;
        if(key==DIRECT_KEY_BACK && panel_ui_can_exit(&s_modbus_ui,&s_modbus_snapshot,s_modbus_root)){
            panel_bus_request_settings(NULL,0);
            s_modbus_active=false;s_have_previous=false;s_ui.dirty=true;
            return;
        }
        panel_command_t command={0};
        if(panel_ui_key(&s_modbus_ui,key,&s_modbus_snapshot,&command)){
            uint32_t sequence;
            if(panel_bus_submit_sequence(&command,&sequence))panel_ui_submitted(&s_modbus_ui,sequence,command.type);
            else panel_ui_submit_failed(&s_modbus_ui,command.type);
        }
        const panel_setting_t *item=s_modbus_ui.page==PANEL_PAGE_SETTINGS?panel_setting(s_modbus_ui.selected):NULL;
        ESP_LOGW(TAG,"LCD_MB_APPLY seq=%u page=%u item=%u reg=%04X edit=%u review=%u pending=%u blocked=%u reason=%s",
                 sequence,s_modbus_ui.page,s_modbus_ui.selected,item?item->address:0,
                 s_modbus_ui.editing,s_modbus_ui.review,s_modbus_ui.pending,s_modbus_ui.blocked,s_modbus_ui.message);
    }
    if(s_modbus_ui.page==PANEL_PAGE_DETAILS && s_modbus_ui.detail==3){
        panel_bus_request_settings(NULL,0);
        s_modbus_active=false;s_have_previous=false;
        s_bms_return_root=s_modbus_root;s_bms_return_page=s_ui.page;
        s_bms_return_detail=s_modbus_ui.detail_index_from_page?s_modbus_ui.detail_origin:2;
        s_bms_return_pending=true;
        s_ui.page=UI_BMS;s_ui.parent_page=UI_PARAMETERS;s_ui.detail_page=0;s_ui.dirty=true;
        return;
    }
    panel_ui_view(&s_modbus_ui,&s_modbus_snapshot,&s_modbus_view);
    if(s_modbus_view.detail_dashboard && s_modbus_view.illustration==7 &&
       full_battery_sample_valid(&s_ui_data)){
        snprintf(s_modbus_view.values[3],sizeof(s_modbus_view.values[3]),"%s",
                 s_ui_data.bms_protocol==PANEL_BMS_CSERGY?"CSERGY":"BBJ");
        if(!s_ui_data.bms_online)s_modbus_view.stale_rows|=1U<<3;
    }
    panel_bus_request_settings(s_modbus_view.setting_addresses,s_modbus_view.setting_count);
    s_modbus_view.activity_frame=s_modbus_ui.pending?1:0;
    atomic_store(&s_panel_page,0x100U+s_modbus_ui.page);
    s_modbus_view.theme_light=(s_preview_style&1)!=0;
    if(!s_blank){
        bool changed=!s_modbus_drawn || memcmp(&s_modbus_view,&s_modbus_previous,sizeof(s_modbus_view));
        panel_stage(PANEL_RENDER);
        if(!panel_set_view_inversion(true)){s_modbus_drawn=false;return;}
        unsigned errors=atomic_load(&s_lcd_tx_errors);
        if(changed)panel_lvgl_show_modbus(&s_modbus_view);
        panel_lvgl_refresh(now_ms(),!s_modbus_drawn);
        bool ok=atomic_load(&s_lcd_tx_errors)==errors;
        if(ok){
            s_modbus_previous=s_modbus_view;s_modbus_drawn=true;
            if(changed)atomic_fetch_add(&s_panel_frames,1);
            atomic_store(&s_first_frame_done,true);
        }else s_modbus_drawn=false;
    }
}

static void panel_task(void *arg) {
    (void)arg;
    panel_stage(PANEL_LOCAL);
    atomic_store(&s_panel_stack_free,uxTaskGetStackHighWaterMark(NULL));
    ESP_LOGW(TAG,"LCD_UI_TASK started, free_heap=%lu",(unsigned long)esp_get_free_heap_size());
    ESP_LOGI(TAG,"LCD_LVGL: RGB565 partial buffer=10240, menus=%u Modbus settings=%u",
             direct_ui_menu_count(),(unsigned)panel_setting_count());
    direct_ui_init(&s_ui);panel_load_local();onboarding_state_init(&s_onboarding);
    nvs_handle_t guide;if(nvs_open("phone_guide",NVS_READONLY,&guide)==ESP_OK){size_t n=sizeof(s_dismissed_sn);if(nvs_get_str(guide,"dismissed_sn",s_dismissed_sn,&n)!=ESP_OK)s_dismissed_sn[0]=0;nvs_close(guide);}
    ESP_LOGW(TAG,"LCD_UI_TASK local settings loaded, blank=%u",s_local_blank);
    /* Brand splash, drawn on the saved theme before the first UI frame.
     * Force a full repaint afterwards so the splash cannot bleed into it. */
    panel_stage(PANEL_SPLASH);
    if(!panel_lvgl_init(s_strip_pixels,panel_write_pixels,now_ms())){
        ESP_LOGE(TAG,"LCD_LVGL initialization failed");
        panel_stage(PANEL_FAILED);vTaskDelete(NULL);return;
    }
    if(s_panel_initialized){
        panel_lvgl_splash((s_saved_style&1)!=0);
        unsigned errors=atomic_load(&s_lcd_tx_errors);
        panel_lvgl_refresh(now_ms(),true);
        /* Register init deliberately leaves DISPON off. The removed legacy
         * splash used to enable scanout; the LVGL boot path must do it after
         * the complete first frame, before starting the normal UI loop. */
        if(atomic_load(&s_lcd_tx_errors)==errors)direct_panel_enable_display();
        s_panel_initialized=atomic_load(&s_lcd_tx_errors)==errors;
        if(s_panel_initialized){
            uint32_t started=now_ms();
            while(now_ms()-started<PANEL_SPLASH_DURATION_MS){
                vTaskDelay(pdMS_TO_TICKS(30));
                panel_lvgl_refresh(now_ms(),false);
                atomic_store(&s_panel_tick,now_ms());
                if(atomic_load(&s_lcd_tx_errors)!=errors){s_panel_initialized=false;break;}
            }
        }
        if(!s_panel_initialized)ESP_LOGW(TAG,"LCD_LVGL startup transfer failed; retrying through recovery owner");
    }
    s_have_previous=false;s_ui.dirty=true;
    ESP_LOGW(TAG,"LCD_UI_TASK entering refresh loop");
    uint32_t last_render=0;s_last_input=now_ms();
    uint32_t last_signature=0;
    bool previous_alarm=false;
    bool previous_factory_test=false;
    s_recovery.last_attempt_ms=now_ms();
    s_recovery.baseline_errors=atomic_load(&s_recovery_error_baseline);
    panel_stage(PANEL_SNAPSHOT);
    panel_snapshot();
    for(;;){
        atomic_store(&s_panel_tick,now_ms());
        panel_stage(PANEL_INPUT);
        panel_service_recovery();
        if(panel_ble_aux_take_preferences_changed()){
            panel_load_local();
            if(s_ui.page==UI_APPEARANCE){
                panel_feedback_settings_t feedback=panel_feedback_preferences();
                s_ui.appearance_theme=(s_saved_style&1U)!=0;
                s_ui.appearance_language=(s_saved_style&2U)!=0;
                s_ui.time_fields[0]=s_saved_display.brightness;
                s_ui.time_fields[1]=s_local_blank?s_saved_display.timeout_index:0;
                s_ui.time_fields[2]=feedback.led_brightness;
                s_ui.time_fields[3]=feedback.beep_mode;
                s_ui.time_fields[4]=0;
            }
            s_ui.dirty=true;
            s_have_previous=false;
        }
        if(!s_panel_initialized){
            panel_stage(PANEL_FAILED);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if(s_modbus_active && (panel_ota_active() || factory_test_display_requested())){
            s_modbus_active=false;s_have_previous=false;s_ui.dirty=true;
        }
        if(s_modbus_active){
            panel_snapshot();
            full_modbus_panel_tick();
            panel_idle_tick(panel_alarm_feedback(&previous_alarm) || s_ui_data.ota_busy || s_modbus_snapshot.command_busy);
            panel_stage(PANEL_WAIT);
            atomic_store(&s_panel_stack_free,uxTaskGetStackHighWaterMark(NULL));
            if(s_modbus_active)vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        bool handled_key=false;
#if CONFIG_DIRECT_IDENTITY_PRODUCTION
        if(!g_device_sn[0]){s_ui.page=UI_PRODUCTION;}
#endif
        bool factory_testing=factory_test_display_requested();
        onboarding_inputs_t inputs=panel_onboarding_inputs(factory_testing);
        if(s_ui.page==UI_PHONE_SETUP&&!s_onboarding.visible){
         /* Manual settings entry: preserve navigation, allow missing PIN guidance. */
         inputs.editing=false;s_phone_return=UI_SUBMENU;
         if(onboarding_state_open(&s_onboarding,&inputs,now_ms())!=ONBOARDING_OPENED)panel_onboarding_close();
        }
        onboarding_change_t change=onboarding_state_tick(&s_onboarding,&inputs,now_ms(),true);
        if(change==ONBOARDING_OPENED){s_phone_return=UI_HOME;s_ui.page=UI_PHONE_SETUP;s_ui.dirty=true;}
        if(change==ONBOARDING_CLOSED)panel_onboarding_close();
        if(s_onboarding.visible)onboarding_qr_update(&s_onboarding_qr,g_device_sn,s_pair_pin);
        if(factory_testing){
            if(s_ui.page==UI_APPEARANCE)panel_discard_preview();
            panel_set_blank(false);s_last_input=now_ms();
            s_ui.page=UI_HELP;s_ui.detail_page=1;
            if(!previous_factory_test)s_ui.dirty=true;
        }else if(previous_factory_test){s_ui.page=UI_HOME;s_ui.detail_page=0;s_ui.dirty=true;}
        previous_factory_test=factory_testing;
        if(s_ui_data.ota_busy && s_ui.page!=UI_UPGRADE){
            if(s_ui.page==UI_APPEARANCE)panel_discard_preview();
            s_ui.page=UI_UPGRADE;s_ui.confirm=false;s_ui.message[0]=0;s_ui.dirty=true;
        }
        uint8_t key;uint32_t sampled;
        while(direct_keys_get_timed_event(&key,&sampled)){
            if(!panel_key_current(sampled))continue;
            unsigned key_sequence=atomic_fetch_add(&s_panel_key_events,1)+1;
            ESP_LOGW(TAG,"LCD_KEY received seq=%u key=%u page=%u guide=%u blank=%u factory=%u",
                     key_sequence,key,s_ui.page,s_onboarding.visible,s_blank,factory_testing);
            handled_key=true;factory_note_key(key);
            bool woke=panel_wake_key();
            /* Diagnostic key presses are evidence only; never issue inverter actions. */
            if(factory_testing){s_ui.dirty=true;continue;}
            if(woke)continue;
            if(s_onboarding.visible){
             if(key==1){
              if(onboarding_state_back(&s_onboarding)==ONBOARDING_SAVE_DISMISSED){
               nvs_handle_t h;if(nvs_open("phone_guide",NVS_READWRITE,&h)==ESP_OK){if(nvs_set_str(h,"dismissed_sn",g_device_sn)==ESP_OK&&nvs_commit(h)==ESP_OK)snprintf(s_dismissed_sn,sizeof(s_dismissed_sn),"%s",g_device_sn);nvs_close(h);}
              }
              panel_onboarding_close();
              break;
             }else onboarding_state_activity(&s_onboarding,now_ms());
             continue;
            }
            if(s_ui.page==UI_WIFI){wifi_ui_key(key);continue;}
            if(s_ui.page==UI_UPGRADE && !panel_ota_active()){firmware_ui_key(key);continue;}
            if(s_ui.page==UI_WEATHER){weather_ui_key(key);continue;}
            direct_ui_event_t event=direct_ui_key(&s_ui,key,sampled,&s_ui_data);panel_action(event);
            ESP_LOGW(TAG,"LCD_KEY applied seq=%u page=%u selected=%u action=%u",
                     key_sequence,s_ui.page,s_ui.selected,event.action);
            if(s_modbus_active)break;
        }
        if(!s_panel_initialized || s_modbus_active)continue;
        if(s_ui.page==UI_WIFI && !s_ui.detail_page){
            s_ui.detail_page=1;
            panel_wifi_status_t state=panel_wifi_status();
            if(state!=PANEL_WIFI_SCANNING && state!=PANEL_WIFI_CONNECTING)wifi_ui_scan();
        }else if(s_ui.page!=UI_WIFI && s_ui.page!=UI_WEATHER && s_wifi_length)wifi_edit_reset();
        panel_bus_request_settings(NULL,0);
        panel_stage(PANEL_SNAPSHOT);
        panel_snapshot();
        /* Opening the existing Phone connection page requests exclusive BLE.
         * Resource work stays in the main loop, never the LCD/key task. */
        app_request_local_ble(s_onboarding.visible && !s_onboarding.automatic);
        bool alarm=panel_alarm_feedback(&previous_alarm);
        panel_idle_tick(alarm || s_ui_data.ota_busy || factory_testing);
        atomic_store(&s_panel_page,s_ui.page);
        panel_stage(PANEL_RENDER);
        bool catalog=firmware_ui_tick();
        if(s_ui.page==UI_WIFI)wifi_ui_view(&s_view,s_ui_data.language_zh);
        else if(s_ui.page==UI_WEATHER)weather_ui_view(&s_view,s_ui_data.language_zh);
        else if(catalog)firmware_ui_view(&s_view,s_ui_data.language_zh);
        else direct_ui_build(&s_ui,&s_ui_data,&s_view);
        uint32_t signature=direct_display_signature(&s_ui,&s_ui_data,&s_view);
        if(s_ui.page==UI_HOME){
            signature=(signature^full_inverter_alarm_active(&s_ui_data))*16777619U;
            full_battery_source_t battery=full_battery_source(&s_ui_data);
            signature=(signature^(uint32_t)battery)*16777619U;
            if(battery==FULL_BATTERY_DIRECT){
                signature=(signature^s_ui_data.bms.soc_percent)*16777619U;
                signature=(signature^s_ui_data.bms.voltage_10mv)*16777619U;
                signature^=s_ui_data.bms_online;
            }else if(battery==FULL_BATTERY_INVERTER){
                signature=(signature^s_ui_data.inverter_battery.values[2])*16777619U;
                signature^=s_ui_data.inverter_battery.valid|(unsigned)s_ui_data.inverter_battery.online<<8;
            }
        }
        if(s_ui.page==UI_PHONE_SETUP){
            signature^=(uint32_t)s_has_wifi | ((uint32_t)s_bound<<1);
            signature=(signature^s_onboarding_qr.generation)*16777619U;
            signature^=(uint32_t)s_onboarding_qr.ready<<2;
            for(size_t i=0;s_pair_pin[i];i++)signature=(signature^s_pair_pin[i])*16777619U;
            for(size_t i=0;g_device_sn[i];i++)signature=(signature^g_device_sn[i])*16777619U;
        }
        if(!s_blank && (!s_have_previous || signature!=last_signature ||
                       direct_display_should_render(now_ms(),last_render,s_ui.dirty))){
            bool first=!atomic_load(&s_first_frame_done);
            if(first)ESP_LOGW(TAG,"LCD_UI_FIRST_FRAME begin");
            int64_t render_start=esp_timer_get_time();
            panel_render();
            last_render=now_ms();
            if(!s_have_previous)s_panel_initialized=false;
            atomic_fetch_add(&s_panel_frames,1);
            if(s_have_previous)last_signature=signature;
            if(first && s_have_previous)atomic_store(&s_first_frame_done,true);
            uint32_t render_ms=(uint32_t)((esp_timer_get_time()-render_start)/1000);
            if(first || handled_key)ESP_LOGW(TAG,"LCD_RENDER ms=%lu reason=%s page=%u frames=%u tx=%u errors=%u",
                (unsigned long)render_ms,first?"first":"key",s_ui.page,
                atomic_load(&s_panel_frames),atomic_load(&s_spi_transfers),atomic_load(&s_lcd_tx_errors));
            if(render_ms>100)ESP_LOGD(TAG,"LCD_RENDER slow ms=%lu",(unsigned long)render_ms);
            if(first){
                atomic_store(&s_panel_stack_free,uxTaskGetStackHighWaterMark(NULL));
                ESP_LOGW(TAG,"LCD_UI_FIRST_FRAME done, ok=%u stack_free=%u",s_have_previous,atomic_load(&s_panel_stack_free));
            }
        }else if(!s_blank){
            unsigned errors=atomic_load(&s_lcd_tx_errors);
            panel_lvgl_refresh(now_ms(),false);
            if(atomic_load(&s_lcd_tx_errors)!=errors){s_have_previous=false;s_ui.dirty=true;}
        }
        panel_stage(PANEL_WAIT);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

bool direct_panel_software_healthy(void){
    return atomic_load(&s_first_frame_done) &&
           atomic_load(&s_lcd_tx_errors)==atomic_load(&s_recovery_error_baseline) &&
           now_ms()-atomic_load(&s_panel_tick)<=2000U;
}
