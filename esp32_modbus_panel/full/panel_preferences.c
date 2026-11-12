#include "panel_preferences.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#else
#define CONFIG_PANEL_LED_BRIGHTNESS 24
#endif

static StaticSemaphore_t lock_storage;
static SemaphoreHandle_t lock;
static portMUX_TYPE init_guard=portMUX_INITIALIZER_UNLOCKED;
static bool loaded;
static panel_preferences_t committed;
static const panel_preferences_t defaults={1,1,1,480,2,{PANEL_DISPLAY_DEFAULT_BRIGHTNESS,2},
    {CONFIG_PANEL_LED_BRIGHTNESS,PANEL_BEEP_HIGH}};

static bool take(void)
{
    taskENTER_CRITICAL(&init_guard);
    if(!lock)lock=xSemaphoreCreateMutexStatic(&lock_storage);
    taskEXIT_CRITICAL(&init_guard);
    return lock && xSemaphoreTake(lock,pdMS_TO_TICKS(200))==pdTRUE;
}
static bool valid(const panel_preferences_t *p)
{
    return p && p->alarm<=1 && p->buzzer<=1 && p->blank<=1 && p->style<=3 &&
        p->timezone>=-720 && p->timezone<=840 && panel_display_valid(p->display) &&
        p->feedback.beep_mode>0 && p->feedback.beep_mode<PANEL_BEEP_COUNT;
}
static void decode(const uint8_t b[16],panel_preferences_t *p)
{
    /* Duration-era records used byte13 and had one fixed electrical amplitude.
     * Preserve that amplitude as HIGH, including a separately saved OFF flag.
     * New volume levels live in the spare extension so old firmware still sees
     * the normal 70ms mode if it reads this record after a rollback. */
    panel_feedback_settings_t feedback=defaults.feedback;
    if(b[13])feedback.led_brightness=b[12];
    if(b[15]==0xb2)feedback.beep_mode=b[14];
    *p=(panel_preferences_t){b[4],b[5],b[6],(int16_t)(b[8]|((uint16_t)b[9]<<8)),b[7],{b[10],b[11]},feedback};
}
static void encode(const panel_preferences_t *p,uint8_t b[16])
{
    memset(b,0,16);b[0]='M';b[1]='B';b[2]=1;b[3]=16;
    b[4]=(uint8_t)p->alarm;b[5]=(uint8_t)p->buzzer;b[6]=(uint8_t)p->blank;b[7]=p->style;
    b[8]=(uint8_t)p->timezone;b[9]=(uint8_t)((uint16_t)p->timezone>>8);
    b[10]=p->display.brightness;b[11]=p->display.timeout_index;
    b[12]=p->feedback.led_brightness;b[13]=PANEL_BEEP_NORMAL;
    b[14]=p->feedback.beep_mode;b[15]=0xb2;
}
static bool missing(esp_err_t err)
{return err==ESP_ERR_NVS_NOT_FOUND || err==ESP_ERR_NVS_TYPE_MISMATCH;}
static bool load(void)
{
    if(loaded)return true;
    committed=defaults;
    nvs_handle_t h;
    esp_err_t err=nvs_open("direct_ui",NVS_READONLY,&h);
    if(err==ESP_ERR_NVS_NOT_FOUND){loaded=true;return true;}
    if(err!=ESP_OK)return false;
    uint8_t b[16];size_t size=sizeof(b);
    panel_preferences_t next;
    err=nvs_get_blob(h,"prefs",b,&size);
    if(err==ESP_OK && size>=3 && b[0]=='M' && b[1]=='B'){
        if(b[2]>1)goto unavailable; /* Preserve a newer record on rollback. */
        if(size==sizeof(b) && b[2]==1 && b[3]==16){
            decode(b,&next);
            if(valid(&next)){committed=next;nvs_close(h);loaded=true;return true;}
        }
    }else if(err!=ESP_OK && !missing(err)){
        /* An oversized blob may be from a newer version. */
        goto unavailable;
    }
    /* Read old records only when the new authoritative blob is absent/corrupt.
     * Explicit legacy blank OFF wins over the old display timer word. */
    uint16_t v;int16_t tz;uint8_t style;uint32_t word;
#define READ_OLD(call, use) do {err=(call);if(err==ESP_OK){use;}else if(!missing(err))goto unavailable;} while(0)
    READ_OLD(nvs_get_u16(h,"alarm",&v), if(v<=1)committed.alarm=v);
    READ_OLD(nvs_get_u16(h,"buzzer",&v), if(v<=1)committed.buzzer=v);
    READ_OLD(nvs_get_u16(h,"blank",&v), if(v<=1)committed.blank=v);
    READ_OLD(nvs_get_i16(h,"tz",&tz), if(tz>=-720 && tz<=840)committed.timezone=tz);
    READ_OLD(nvs_get_u8(h,"style",&style), if(style<=3)committed.style=style);
    READ_OLD(nvs_get_u32(h,"display",&word), if((word>>16)==0xb105U &&
       panel_display_unpack((uint16_t)((word>>2)&0x7ffU),&next.display)){
        committed.display=next.display;committed.style=(uint8_t)(word&3U);
    });
#undef READ_OLD
    nvs_close(h);loaded=true;return true;
unavailable:
    nvs_close(h);return false;
}
bool panel_preferences_get(panel_preferences_t *out)
{
    if(!out || !take())return false;
    bool ok=load();if(ok)*out=committed;xSemaphoreGive(lock);return ok;
}
bool panel_preferences_update(const panel_preferences_t *p,unsigned fields)
{
    if(!p || !fields || (fields&~PANEL_PREF_ALL) || !take())return false;
    if(!load()){xSemaphoreGive(lock);return false;}
    panel_preferences_t next=committed;
    if(fields&PANEL_PREF_ALARM)next.alarm=p->alarm;
    if(fields&PANEL_PREF_BUZZER)next.buzzer=p->buzzer;
    if(fields&PANEL_PREF_BLANK)next.blank=p->blank;
    if(fields&PANEL_PREF_DISPLAY){next.style=p->style;next.display=p->display;next.blank=p->display.timeout_index!=0;}
    if(fields&PANEL_PREF_BRIGHTNESS)next.display.brightness=p->display.brightness;
    if(fields&PANEL_PREF_TIMEOUT){
        next.display.timeout_index=p->display.timeout_index;
        next.blank=p->display.timeout_index!=0;
    }
    if(fields&PANEL_PREF_THEME)next.style=(next.style&2U)|(p->style&1U);
    if(fields&PANEL_PREF_LANGUAGE)next.style=(next.style&1U)|(p->style&2U);
    if(fields&PANEL_PREF_TIME)next.timezone=p->timezone;
    if(fields&PANEL_PREF_LED_BRIGHTNESS)next.feedback.led_brightness=p->feedback.led_brightness;
    if(fields&PANEL_PREF_BEEP_MODE){
        next.buzzer=p->buzzer;
        next.feedback.beep_mode=p->feedback.beep_mode;
    }
    if(!valid(&next)){xSemaphoreGive(lock);return false;}
    uint8_t b[16],old[16];encode(&next,b);encode(&committed,old);
    if(!memcmp(b,old,sizeof(b))){xSemaphoreGive(lock);return true;}
    nvs_handle_t h;esp_err_t err=nvs_open("direct_ui",NVS_READWRITE,&h);
    if(err==ESP_OK){
        err=nvs_set_blob(h,"prefs",b,sizeof(b));
        if(err==ESP_OK)err=nvs_commit(h);
        nvs_close(h);
    }
    if(err==ESP_OK)committed=next;
    xSemaphoreGive(lock);return err==ESP_OK;
}
bool panel_preferences_reset(void)
{return panel_preferences_update(&defaults,PANEL_PREF_ALL);}
