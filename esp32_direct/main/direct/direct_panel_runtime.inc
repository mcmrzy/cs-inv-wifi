/* Included by direct_panel.c so rendering has one owner and one SPI stream. */
#include "direct_keys.h"
#include "direct_buzzer.h"
#include "direct_link.h"
#include "direct_display_sample.h"
#include "direct_display_events.h"
#include "direct_home.h"
#include "direct_ui.h"
#include "direct_ui_scene.h"
#include "direct_ui_assets.h"
#include "direct_energy.h"
#include "nvs.h"
#include "config.h"
#include "mqtt_handler.h"
#include "app_network.h"
#include "ota_service.h"
#include "factory_identity.h"
#include "factory_service.h"
#include "onboarding_state.h"
#include "onboarding_qr.h"
#include "direct_onboarding_scene.h"
#include "cloud_binding.h"
#include "nvs_storage.h"
#include "wifi_sta.h"
#include "esp_system.h"
#include "esp_attr.h"
#include <time.h>
#include <sys/time.h>

static direct_ui_state_t s_ui;
static direct_ui_data_t s_ui_data;
static direct_display_sample_t s_display_sample;
static direct_dsp_status_t s_display_status;
static uint32_t s_display_status_ms,s_display_status_blocked_ms;
static bool s_display_status_valid,s_display_status_waiting;
static direct_ui_view_t s_view;
static char s_clock_text[24];
static factory_record_t s_identity;
static onboarding_state_t s_onboarding;
static onboarding_qr_t s_onboarding_qr;
static char s_pair_pin[7],s_dismissed_sn[33];
static direct_ui_page_t s_phone_return=UI_HOME;
static bool s_has_wifi,s_bound;
static bool s_have_previous, s_blank;
static direct_lcd_recovery_t s_recovery;
static bool s_manual_recovery_pending;
static bool s_wifi_rssi_valid,s_wifi_was_connected;
static int16_t s_wifi_rssi;
static uint32_t s_wifi_rssi_ms;
static bool s_reset_local_pending;
static bool s_pair_ready,s_pair_loaded;
static uint32_t s_pair_refresh_ms;
static onboarding_inputs_t panel_onboarding_inputs(bool testing){
 cloud_binding_state_t binding=cloud_binding_get();
 uint32_t now=now_ms();
 /* Identity changes infrequently; do not read both NVS slots at key-scan rate. */
 if(!s_pair_loaded || (uint32_t)(now-s_pair_refresh_ms)>=1000U){
  s_pair_loaded=true;s_pair_refresh_ms=now;
  s_has_wifi=storage_has_wifi_config()||WIFI_DEFAULT_SSID[0];
  s_pair_ready=factory_identity_pairing(g_device_sn,s_pair_pin);
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
static bool s_lcd_test_inverted;
static direct_ui_page_t s_previous_page;
static uint8_t s_previous_mode;
static uint32_t s_strip_hash[15];
static uint16_t s_local_alarm=1, s_local_buzzer=1, s_local_blank=0;
/* One atomic NVS style value: bit0 light, bit1 Chinese. Default Chinese/dark. */
static uint8_t s_saved_style=2, s_preview_style=2;
static int16_t s_timezone=480;
static DMA_ATTR uint16_t s_strip_pixels[LCD_W*16] __attribute__((aligned(4)));

static void panel_beep(void) { direct_buzzer_pulse(); }
static bool panel_save_local(void) {
    nvs_handle_t h;
    if(nvs_open("direct_ui",NVS_READWRITE,&h)!=ESP_OK)return false;
    esp_err_t err=nvs_set_u16(h,"alarm",s_local_alarm);
    if(err==ESP_OK)err=nvs_set_u16(h,"buzzer",s_local_buzzer);
    if(err==ESP_OK)err=nvs_set_u16(h,"blank",s_local_blank);
    if(err==ESP_OK)err=nvs_set_i16(h,"tz",s_timezone);
    if(err==ESP_OK)err=nvs_set_u8(h,"style",s_saved_style);
    if(err==ESP_OK)err=nvs_commit(h);
    nvs_close(h);return err==ESP_OK;
}
static void panel_load_local(void) {
    nvs_handle_t h;
    if(nvs_open("direct_ui",NVS_READONLY,&h)==ESP_OK){
        nvs_get_u16(h,"alarm",&s_local_alarm);nvs_get_u16(h,"buzzer",&s_local_buzzer);
        nvs_get_u16(h,"blank",&s_local_blank);nvs_get_i16(h,"tz",&s_timezone);nvs_close(h);
    }
    s_local_alarm=!!s_local_alarm;s_local_buzzer=!!s_local_buzzer;s_local_blank=!!s_local_blank;
    if(s_timezone < -720 || s_timezone > 840)s_timezone=480;
    direct_buzzer_set_enabled(s_local_buzzer!=0);
    if(nvs_open("direct_ui",NVS_READONLY,&h)==ESP_OK){
        uint8_t stored=2;
        if(nvs_get_u8(h,"style",&stored)==ESP_OK && stored<=3)s_saved_style=stored;
        nvs_close(h);
    }
    s_preview_style=s_saved_style;
}
static void panel_snapshot(void) {
    memset(&s_ui_data,0,sizeof(s_ui_data));
    s_ui_data.wifi_connected=wifi_sta_is_connected();
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
    s_ui_data.theme_light=(s_preview_style&1)!=0;
    s_ui_data.language_zh=(s_preview_style&2)!=0;
    cloud_binding_state_t binding=cloud_binding_get();
    s_ui_data.phone_setup_needed=!s_has_wifi || binding==CLOUD_BINDING_UNBOUND;
    if(arm_poll_take_param()){
        s_ui_data.polls=g_arm_stat.poll_count;s_ui_data.errors=g_arm_stat.err_count;
        arm_poll_give_param();
    }
    bool dsp_paused=ota_service_is_busy() || direct_dsp_boot_active();
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
    direct_config_snapshot(s_ui_data.config,s_ui_data.valid);
    s_ui_data.battery_valid=direct_battery_config_get(&s_ui_data.battery);
    if(!s_ui_data.battery_valid)memset(&s_ui_data.battery,0,sizeof(s_ui_data.battery));
    if(!direct_battery_config_profiles(s_ui_data.battery_profiles))
        memset(s_ui_data.battery_profiles,0,sizeof(s_ui_data.battery_profiles));
    if(s_ui.page==UI_CONFIG || s_ui.page==UI_REVIEW)
        for(unsigned i=0;i<CONFIG_PARAM_MAX_INDEX;i++)s_ui_data.writable[i]=direct_config_writable(i);
    if(s_ui_data.valid[14])s_local_alarm=s_ui_data.config[14];
    if(s_ui_data.valid[15])s_local_blank=s_ui_data.config[15];
    if(s_ui_data.valid[41] && s_local_buzzer!=s_ui_data.config[41]){s_local_buzzer=s_ui_data.config[41];direct_buzzer_set_enabled(s_local_buzzer!=0);}
    const unsigned local[]={14,15,41};const uint16_t values[]={s_local_alarm,s_local_blank,s_local_buzzer};
    for(unsigned i=0;i<3;i++)if(!s_ui_data.valid[local[i]]){s_ui_data.config[local[i]]=values[i];s_ui_data.valid[local[i]]=true;}
    uint32_t age=0;
    s_ui_data.bms_online=direct_bms_get_snapshot(&s_ui_data.bms,&age) && age<5000;
    s_ui_data.energy_valid=direct_energy_snapshot(s_ui_data.energy_today,s_ui_data.energy_total);
    s_ui_data.version=FIRMWARE_VERSION;s_ui_data.sn=g_device_sn;
    s_ui_data.ota_busy=ota_service_is_busy();ota_service_get_snapshot(&s_ui_data.ota);
    s_ui_data.model="CS-L10-6K2";s_ui_data.hardware="UNSET";
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
    s_ui_data.home_flow_phase=s_blank?0:direct_home_animation_phase(&s_ui,&s_ui_data,sampled_ms);
}
static uint32_t strip_hash(const uint16_t *pixels,size_t count) {
    uint32_t h=2166136261U;
    for(size_t i=0;i<count;i++){h^=pixels[i];h*=16777619U;}
    return h;
}
static void panel_render(void) {
    direct_ui_build(&s_ui,&s_ui_data,&s_view);
    bool requested_inversion=s_ui.page==UI_HELP && s_ui.detail_page==2;
    if(requested_inversion!=s_lcd_test_inverted){
        uint32_t errors=s_lcd_tx_errors;
        command(requested_inversion?0x21:0x20);
        if(errors!=s_lcd_tx_errors){s_have_previous=false;s_ui.dirty=true;return;}
        s_lcd_test_inverted=requested_inversion;s_have_previous=false;
        ESP_LOGI(TAG,"LCD_VIEW_TEST inversion=%s, transient only",requested_inversion?"ON":"OFF");
    }
    bool full=!s_have_previous || s_previous_page!=s_ui.page || s_previous_mode!=s_ui.main_mode;
    bool ok=true;
    for(int y=0;y<LCD_H;y+=16){
        atomic_store(&s_panel_row,(unsigned)y);
        direct_gfx_t gfx;
        direct_gfx_begin(&gfx,s_strip_pixels,LCD_W,16,y,0);
        if(s_ui.page==UI_PHONE_SETUP){
         gfx.theme_light=s_ui_data.theme_light;gfx.language_zh=s_ui_data.language_zh;
         direct_onboarding_scene_draw(&gfx,&s_onboarding_qr,g_device_sn,s_pair_pin,s_has_wifi,s_bound);
        }else direct_ui_scene_draw(&gfx,&s_ui,&s_ui_data,&s_view);
        uint32_t hash=strip_hash(s_strip_pixels,LCD_W*16);
        unsigned stripe=(unsigned)y/16;
        if(full || hash!=s_strip_hash[stripe]){
            for(size_t i=0;i<LCD_W*16;i++) {
                uint16_t p=s_strip_pixels[i];s_strip_pixels[i]=(uint16_t)((p>>8)|(p<<8));
            }
            uint32_t errors=s_lcd_tx_errors;
            window(0,y,LCD_W,16);
#if CONFIG_DIRECT_LCD_GPIO_SPI
            for(size_t at=0;at<sizeof(s_strip_pixels);at+=512){
                size_t left=sizeof(s_strip_pixels)-at;
                tx(true,(const uint8_t*)s_strip_pixels+at,left<512?left:512);
            }
#else
            tx(true,s_strip_pixels,sizeof(s_strip_pixels));
#endif
            if(errors==s_lcd_tx_errors)s_strip_hash[stripe]=hash;
            else ok=false;
        }
    }
    s_previous_page=s_ui.page;s_previous_mode=s_ui.main_mode;
    s_have_previous=ok;s_ui.dirty=!ok;
}

static bool panel_reset_display(void) {
    const unsigned before=atomic_load(&s_lcd_tx_errors);
    atomic_store(&s_first_frame_done,false);
    s_panel_initialized=panel_hardware_initialize();
    if(s_panel_initialized)direct_panel_enable_display();
    s_panel_initialized=s_panel_initialized && atomic_load(&s_lcd_tx_errors)==before;
    s_blank=false;s_have_previous=false;s_lcd_test_inverted=false;s_ui.dirty=true;
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
    ESP_LOGW(TAG,"LCD_RECOVERY done ok=%u; full repaint requested",ok);
}
static bool panel_set_time(void) {
    const int *f=s_ui.time_fields;
    int64_t utc;
    if(!direct_ui_time_utc(f,&utc))return false;
    int16_t previous=s_timezone;s_timezone=(int16_t)f[6];
    if(!panel_save_local()){s_timezone=previous;return false;}
    direct_energy_set_timezone(s_timezone);
    struct timeval now={.tv_sec=(time_t)utc};return settimeofday(&now,NULL)==0;
}
static void panel_action(direct_ui_event_t event) {
    _Static_assert((int)UI_WRITE_BUSY==(int)ARM_WRITE_BUSY &&
        (int)UI_WRITE_INVALID==(int)ARM_WRITE_INVALID &&
        (int)UI_WRITE_UNSUPPORTED==(int)ARM_WRITE_UNSUPPORTED &&
        (int)UI_WRITE_UPGRADE_BUSY==(int)ARM_WRITE_UPGRADE_BUSY &&
        (int)UI_WRITE_NOT_READY==(int)ARM_WRITE_NOT_READY &&
        (int)UI_WRITE_STALE==(int)ARM_WRITE_REVISION_CONFLICT &&
        (int)UI_WRITE_BATTERY_PROTOCOL==(int)ARM_WRITE_PROTOCOL_UNSUPPORTED &&
        (int)UI_WRITE_BATTERY_INCOMPLETE==(int)ARM_WRITE_PROFILE_REQUIRED &&
        (int)UI_WRITE_SAVE_FAILED==(int)ARM_WRITE_SAVE_FAILED &&
        (int)UI_WRITE_SUBMIT_FAILED==(int)ARM_WRITE_SUBMIT_FAILED,"UI write result adapter mismatch");
    if(event.action==UI_ACT_BATTERY_WRITE){
        /* Recheck the race between the key snapshot and admission. Only the
         * UART owner persists/submits the group and advances its revision. */
        int result=ota_service_is_busy()?UI_WRITE_UPGRADE_BUSY:
            direct_battery_config_write(&s_ui.battery_draft,false);
        if(result!=0)direct_ui_write_result(&s_ui,result);
        return;
    }
    if(ota_service_is_busy() && (event.action==UI_ACT_WRITE || event.action==UI_ACT_RESTART || event.action==UI_ACT_LOCAL_RESET || event.action==UI_ACT_LCD_RESET))return;
    if(event.action==UI_ACT_OTA_CANCEL){OtaSnapshot snapshot;if(ota_service_get_snapshot(&snapshot))ota_service_cancel(snapshot.task_id);return;}
    if(event.action==UI_ACT_APPEARANCE_PREVIEW){s_preview_style=(uint8_t)(event.value&3);s_ui.dirty=true;}
    else if(event.action==UI_ACT_APPEARANCE_CANCEL){s_preview_style=s_saved_style;s_ui.dirty=true;}
    else if(event.action==UI_ACT_APPEARANCE_SAVE){
        uint8_t old=s_saved_style;s_saved_style=(uint8_t)(event.value&3);
        bool saved=panel_save_local();if(!saved)s_saved_style=old;
        else s_preview_style=s_saved_style;
        snprintf(s_ui.message,sizeof(s_ui.message),"%s",saved?"Appearance saved":"Save failed / retry");s_ui.dirty=true;
    }
    else if(event.action==UI_ACT_REFRESH){arm_poll_request_config();arm_poll_request_info();}
    else if(event.action==UI_ACT_WRITE){
        int result=direct_config_write(event.index,event.value);
        if(result!=0)direct_ui_write_result(&s_ui,result);
    }else if(event.action==UI_ACT_LCD_RESET)s_manual_recovery_pending=true;
    else if(event.action==UI_ACT_RESTART)esp_restart();
    else if(event.action==UI_ACT_LOCAL_RESET){
        if(direct_config_reset_local()==0){s_reset_local_pending=true;s_ui.pending=true;snprintf(s_ui.message,sizeof(s_ui.message),"Saving user settings");}
        else snprintf(s_ui.message,sizeof(s_ui.message),"Save failed / retry");
        s_ui.dirty=true;
    }else if(event.action==UI_ACT_SET_TIME){
        snprintf(s_ui.message,sizeof(s_ui.message),"%s",panel_set_time()?"Time saved":"Invalid date / save failed");s_ui.dirty=true;
    }
}
static void panel_task(void *arg) {
    (void)arg;
    panel_stage(PANEL_LOCAL);
    atomic_store(&s_panel_stack_free,uxTaskGetStackHighWaterMark(NULL));
    ESP_LOGW(TAG,"LCD_UI_TASK started, free_heap=%lu",(unsigned long)esp_get_free_heap_size());
    ESP_LOGI(TAG,"LCD_GRAPHICAL: RGB565 320x16 strip, buffer=10240, menus=6 parameters=84, themes=2 languages=2 Noto font");
    direct_ui_init(&s_ui);panel_load_local();onboarding_state_init(&s_onboarding);
    nvs_handle_t guide;if(nvs_open("phone_guide",NVS_READONLY,&guide)==ESP_OK){size_t n=sizeof(s_dismissed_sn);if(nvs_get_str(guide,"dismissed_sn",s_dismissed_sn,&n)!=ESP_OK)s_dismissed_sn[0]=0;nvs_close(guide);}
    ESP_LOGW(TAG,"LCD_UI_TASK local settings loaded, blank=%u",s_local_blank);
    panel_stage(PANEL_ENERGY);
    direct_energy_init(s_timezone);
    /* Brand splash, drawn on the saved theme before the first UI frame.
     * Force a full repaint afterwards so the splash cannot bleed into it. */
    panel_stage(PANEL_SPLASH);
    if(s_panel_initialized)direct_splash_run((s_saved_style&1)!=0);
    s_have_previous=false;s_ui.dirty=true;
    ESP_LOGW(TAG,"LCD_UI_TASK entering refresh loop");
    uint32_t last_render=0,last_input=now_ms();
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
        if(!s_panel_initialized){
            panel_stage(PANEL_FAILED);
            vTaskDelay(pdMS_TO_TICKS(20));
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
            if(s_blank){command(0x29);s_blank=false;s_have_previous=false;}
            s_ui.page=UI_HELP;s_ui.detail_page=1;
            if(!previous_factory_test)s_ui.dirty=true;
        }else if(previous_factory_test){s_ui.page=UI_HOME;s_ui.detail_page=0;s_ui.dirty=true;}
        previous_factory_test=factory_testing;
        if(s_ui_data.ota_busy && s_ui.page!=UI_UPGRADE){s_ui.page=UI_UPGRADE;s_ui.dirty=true;}
        if(s_reset_local_pending){
            int result=direct_config_last_write_result();
            if(result!=1){
                s_reset_local_pending=false;s_ui.pending=false;
                if(result==2){
                    uint8_t style=s_saved_style;int16_t tz=s_timezone;
                    s_saved_style=2;s_timezone=480;
                    if(panel_save_local()){s_preview_style=s_saved_style;direct_energy_set_timezone(s_timezone);direct_ui_init(&s_ui);}
                    else{s_saved_style=style;s_timezone=tz;snprintf(s_ui.message,sizeof(s_ui.message),"Save failed / retry");s_ui.dirty=true;}
                }else{snprintf(s_ui.message,sizeof(s_ui.message),"%s",result==UI_WRITE_SUBMIT_FAILED?"Saved / sync incomplete":"Save failed / retry");s_ui.dirty=true;}
            }
        }else if(s_ui.pending)direct_ui_write_result(&s_ui,direct_config_last_write_result());
        uint8_t key;uint32_t sampled;
        while(direct_keys_get_timed_event(&key,&sampled)){
            unsigned key_sequence=atomic_fetch_add(&s_panel_key_events,1)+1;
            ESP_LOGW(TAG,"LCD_KEY received seq=%u key=%u page=%u guide=%u blank=%u factory=%u",
                     key_sequence,key,s_ui.page,s_onboarding.visible,s_blank,factory_testing);
            handled_key=true;factory_note_key(key);
            last_input=now_ms();
            /* Diagnostic key presses are evidence only; never issue inverter actions. */
            if(factory_testing){panel_beep();s_ui.dirty=true;continue;}
            if(s_blank){command(0x29);s_blank=false;s_have_previous=false;continue;}
            if(s_onboarding.visible){
             panel_beep();
             if(key==1){
              if(onboarding_state_back(&s_onboarding)==ONBOARDING_SAVE_DISMISSED){
               nvs_handle_t h;if(nvs_open("phone_guide",NVS_READWRITE,&h)==ESP_OK){if(nvs_set_str(h,"dismissed_sn",g_device_sn)==ESP_OK&&nvs_commit(h)==ESP_OK)snprintf(s_dismissed_sn,sizeof(s_dismissed_sn),"%s",g_device_sn);nvs_close(h);}
              }
              panel_onboarding_close();
             }else onboarding_state_activity(&s_onboarding,now_ms());
             continue;
            }
            direct_ui_event_t event=direct_ui_key(&s_ui,key,sampled,&s_ui_data);panel_action(event);
            ESP_LOGW(TAG,"LCD_KEY applied seq=%u page=%u selected=%u action=%u",
                     key_sequence,s_ui.page,s_ui.selected,event.action);
        }
        panel_stage(PANEL_SNAPSHOT);
        panel_snapshot();
        /* Opening the existing Phone connection page requests exclusive BLE.
         * Resource work stays in the main loop, never the LCD/key task. */
        app_request_local_ble(s_onboarding.visible && !s_onboarding.automatic);
        bool alarm=direct_ui_has_alarm(&s_ui_data);
        if(alarm && !previous_alarm && s_local_alarm)panel_beep();
        previous_alarm=alarm;
        if(s_blank && (alarm || s_ui_data.ota_busy)){command(0x29);s_blank=false;s_have_previous=false;}
        if(!s_blank && s_local_blank && !alarm && !s_ui_data.ota_busy && !s_onboarding.visible && now_ms()-last_input>=120000){command(0x28);s_blank=true;}
        atomic_store(&s_panel_page,s_ui.page);
        panel_stage(PANEL_RENDER);
        direct_ui_build(&s_ui,&s_ui_data,&s_view);
        uint32_t signature=direct_display_signature(&s_ui,&s_ui_data,&s_view);
        if(s_ui.page==UI_PHONE_SETUP){
            signature^=(uint32_t)s_has_wifi | ((uint32_t)s_bound<<1);
            for(size_t i=0;s_pair_pin[i];i++)signature=(signature^s_pair_pin[i])*16777619U;
            for(size_t i=0;g_device_sn[i];i++)signature=(signature^g_device_sn[i])*16777619U;
        }
        if(!s_blank && (!s_have_previous || signature!=last_signature ||
                       direct_display_should_render(now_ms(),last_render,s_ui.dirty))){
            bool first=!atomic_load(&s_first_frame_done);
            if(first)ESP_LOGW(TAG,"LCD_UI_FIRST_FRAME begin");
            int64_t render_start=esp_timer_get_time();
            panel_render();last_render=now_ms();
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
