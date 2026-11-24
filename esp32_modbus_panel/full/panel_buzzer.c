#include "direct_buzzer.h"
#include "panel_sound.h"
#include "sdkconfig.h"
#include "driver/ledc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdatomic.h>
#include <stddef.h>

#ifndef CONFIG_PANEL_BUZZ_FREQUENCY_HZ
#define CONFIG_PANEL_BUZZ_FREQUENCY_HZ 2048
#endif

typedef struct {uint16_t hz,ms;} sound_note_t;
typedef struct {const sound_note_t *notes;uint8_t count,priority;} sound_pattern_t;
#define PATTERN(notes_,priority_) {notes_,sizeof(notes_)/sizeof(notes_[0]),priority_}
#define REST(ms) {0, ms}
// Keep the original pitch contours close to the 2,048 Hz buzzer resonance.
#if CONFIG_PANEL_BUZZ_FREQUENCY_HZ == 2048
#define TONE(hz, ms) {CONFIG_PANEL_BUZZ_FREQUENCY_HZ + ((hz) - 3520) / 4, ms}
#else
#define TONE(hz, ms) {hz, ms}
#endif
#if defined(PANEL_BUZZER_DIAGNOSTIC)
/* Equal drive and durations make the frequency response audible. */
static const sound_note_t boot[]={
    {2000,400}, REST(400), {2500,400}, REST(400), {2700,400}, REST(400),
    {3200,400}, REST(400), {3600,400}, REST(400), {4000,400}, REST(400), {4500,400}
};
#else
static const sound_note_t boot[]={
    TONE(2200,60), REST(25), TONE(2772,60), REST(25), TONE(3296,70), REST(25), TONE(4400,110)
};
#endif
static const sound_note_t key[]={TONE(3520,18)};
static const sound_note_t confirm[]={TONE(2637,35), REST(20), TONE(3951,55)};
#if CONFIG_PANEL_BUZZ_FREQUENCY_HZ == 2048
static const sound_note_t back[]={{CONFIG_PANEL_BUZZ_FREQUENCY_HZ,22}};
#else
static const sound_note_t back[]={TONE(2637,30), REST(10), TONE(1976,35)};
#endif
static const sound_note_t success[]={TONE(2637,55), REST(20), TONE(3296,55), REST(20), TONE(3951,90)};
static const sound_note_t error[]={TONE(1568,70), REST(30), TONE(1175,100)};
static const sound_note_t warning[]={TONE(3136,90), REST(100), TONE(3136,90)};
static const sound_note_t fault[]={
    TONE(3920,100), TONE(1960,100), REST(60),
    TONE(3920,100), TONE(1960,100), REST(60),
    TONE(3920,100), TONE(1960,100)
};
static const sound_note_t recovery[]={TONE(3136,60), REST(20), TONE(3951,90)};
static const sound_note_t pulse[]={{CONFIG_PANEL_BUZZ_FREQUENCY_HZ,70}};
static const sound_pattern_t patterns[PANEL_SOUND_COUNT]={
    [PANEL_SOUND_BOOT]=PATTERN(boot,2),
    [PANEL_SOUND_KEY]=PATTERN(key,1),
    [PANEL_SOUND_CONFIRM]=PATTERN(confirm,1),
    [PANEL_SOUND_BACK]=PATTERN(back,1),
    [PANEL_SOUND_SUCCESS]=PATTERN(success,3),
    [PANEL_SOUND_ERROR]=PATTERN(error,3),
    [PANEL_SOUND_WARNING]=PATTERN(warning,4),
    [PANEL_SOUND_FAULT]=PATTERN(fault,5),
    [PANEL_SOUND_RECOVERY]=PATTERN(recovery,3),
};
static const sound_pattern_t factory_pulse=PATTERN(pulse,1);

static StaticSemaphore_t s_lock_storage;
static SemaphoreHandle_t s_lock;
static portMUX_TYPE s_init_mux=portMUX_INITIALIZER_UNLOCKED;
static esp_timer_handle_t s_timer;
static bool s_pwm_ready;
/* The panel's atomic preferences must be applied before any key/boot sound. */
static atomic_bool s_enabled;
static atomic_uchar s_volume=100;
/* Remember cancellation even when mute is toggled back before taking the lock. */
static atomic_uint s_cancel;
/* Retain an urgent request across a PWM update and let an all-clear wait
 * for the active alarm sound to finish. */
static panel_sound_effect_t s_pending_alarm;
static bool s_pending_recovery;
static unsigned s_applied_cancel;
static const sound_pattern_t *s_pattern;
static uint8_t s_note_index;
static int64_t s_deadline_us;
static uint16_t s_output_hz,s_output_duty;

static bool take(void)
{
    taskENTER_CRITICAL(&s_init_mux);
    if(!s_lock)s_lock=xSemaphoreCreateMutexStatic(&s_lock_storage);
    taskEXIT_CRITICAL(&s_init_mux);
    return s_lock && xSemaphoreTake(s_lock,0)==pdTRUE;
}
static void clear_pattern(void)
{
    s_pattern=NULL;s_note_index=0;s_deadline_us=0;
}
#if CONFIG_DIRECT_BUZZ_PIN >= 0
static void fail_silent(void)
{
    /* LEDC idle is upstream of GPIO matrix polarity inversion. */
    if (s_pwm_ready) (void)ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, 0);
    s_pwm_ready=false;s_output_hz=s_output_duty=0;clear_pattern();
}
#endif
static void output(uint16_t hz)
{
#if CONFIG_DIRECT_BUZZ_PIN >= 0
    if(!s_pwm_ready)return;
    unsigned volume=atomic_load(&s_volume);
    uint16_t duty=hz && atomic_load(&s_enabled)?(uint16_t)((volume*512U+50U)/100U):0;
    esp_err_t err=ESP_OK;
    /* Timer1/channel1 are exclusive to sound; backlight owns timer0/channel0. */
    if(duty && s_output_hz!=hz){
        err=ledc_set_freq(LEDC_LOW_SPEED_MODE,LEDC_TIMER_1,hz);
        if(err==ESP_OK)s_output_hz=hz;
    }
    if(err==ESP_OK && duty!=s_output_duty){
        err=ledc_set_duty(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_1,duty);
        if(err==ESP_OK)err=ledc_update_duty(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_1);
        if(err==ESP_OK)s_output_duty=duty;
    }
    if(err!=ESP_OK)fail_silent();
#else
    (void)hz;
#endif
}
static void advance(int64_t now)
{
    unsigned cancellation=atomic_load(&s_cancel);
    if(s_applied_cancel!=cancellation || !atomic_load(&s_enabled) || !atomic_load(&s_volume)){
        s_applied_cancel=cancellation;clear_pattern();output(0);return;
    }
    /* Catch up to absolute time; never stretch or replay expired notes. */
    while(s_pattern && now>=s_deadline_us){
        if(++s_note_index>=s_pattern->count)clear_pattern();
        else s_deadline_us+=(int64_t)s_pattern->notes[s_note_index].ms*1000;
    }
    output(s_pattern?s_pattern->notes[s_note_index].hz:0);
}

static void play_locked(const sound_pattern_t *pattern, int64_t now, bool restart_pulse)
{
    if (!s_timer || !s_pwm_ready || !atomic_load(&s_enabled) || !atomic_load(&s_volume) ||
        s_applied_cancel != atomic_load(&s_cancel)) return;
    if (s_pattern && pattern->priority <= s_pattern->priority &&
        !(restart_pulse && s_pattern == &factory_pulse)) return;
    s_pattern = pattern;
    s_note_index = 0;
    s_deadline_us = now + (int64_t)pattern->notes[0].ms * 1000;
    output(pattern->notes[0].hz);
}

static void timer_tick(void *arg)
{
    (void)arg;
    if(!take())return;
    int64_t now = esp_timer_get_time();
    advance(now);
    taskENTER_CRITICAL(&s_init_mux);
    panel_sound_effect_t effect = s_pending_alarm;
    unsigned alarm_generation = atomic_load(&s_cancel);
    s_pending_alarm = PANEL_SOUND_NONE;
    if (effect == PANEL_SOUND_NONE && s_pending_recovery &&
        (!s_pattern || patterns[PANEL_SOUND_RECOVERY].priority > s_pattern->priority)) {
        effect = PANEL_SOUND_RECOVERY;
        s_pending_recovery = false;
    }
    taskEXIT_CRITICAL(&s_init_mux);
    if (effect != PANEL_SOUND_NONE && alarm_generation == s_applied_cancel)
        play_locked(&patterns[effect], now, false);
    xSemaphoreGive(s_lock);
}
esp_err_t direct_buzzer_init(void)
{
    if(!take())return ESP_ERR_NO_MEM;
    esp_err_t err=ESP_OK;
#if CONFIG_DIRECT_BUZZ_PIN >= 0
    if(!s_pwm_ready){
        const ledc_timer_config_t tone={.speed_mode=LEDC_LOW_SPEED_MODE,
            .duty_resolution=LEDC_TIMER_10_BIT,.timer_num=LEDC_TIMER_1,
            .freq_hz=CONFIG_PANEL_BUZZ_FREQUENCY_HZ,.clk_cfg=LEDC_AUTO_CLK};
        err=ledc_timer_config(&tone);
        if(err==ESP_OK){
            const ledc_channel_config_t channel={.gpio_num=CONFIG_DIRECT_BUZZ_PIN,
                .speed_mode=LEDC_LOW_SPEED_MODE,.channel=LEDC_CHANNEL_1,
                .timer_sel=LEDC_TIMER_1,.intr_type=LEDC_INTR_DISABLE,.duty=0,.hpoint=0,
                .flags.output_invert=!CONFIG_DIRECT_BUZZ_ACTIVE_LEVEL};
            err=ledc_channel_config(&channel);
        }
        if(err==ESP_OK){s_pwm_ready=true;s_output_hz=CONFIG_PANEL_BUZZ_FREQUENCY_HZ;s_output_duty=0;}
    }
    if(err==ESP_OK && !s_timer){
        const esp_timer_create_args_t args={.callback=timer_tick,.name="panel_beep"};
        esp_timer_handle_t timer;
        err=esp_timer_create(&args,&timer);
        if(err==ESP_OK){
            err=esp_timer_start_periodic(timer,5000);
            if(err==ESP_OK)s_timer=timer;else esp_timer_delete(timer);
        }
    }
    if(err!=ESP_OK)fail_silent();
#endif
    xSemaphoreGive(s_lock);return err;
}

static void cancel_playback(void)
{
    taskENTER_CRITICAL(&s_init_mux);
    s_pending_alarm = PANEL_SOUND_NONE;
    s_pending_recovery = false;
    atomic_fetch_add(&s_cancel, 1);
    taskEXIT_CRITICAL(&s_init_mux);
}

void direct_buzzer_set_enabled(bool enabled)
{
    atomic_store(&s_enabled,enabled);
    if(!enabled)cancel_playback();
    timer_tick(NULL);
}
void direct_buzzer_set_duration_ms(uint16_t duration_ms)
{
    /* Factory compatibility API: direct_buzzer_pulse remains a fixed 70ms. */
    (void)duration_ms;
}
void direct_buzzer_set_volume(uint8_t percent)
{
    atomic_store(&s_volume,percent>100?100:percent);
    if(!percent)cancel_playback();
    timer_tick(NULL);
}
static void play(const sound_pattern_t *pattern,bool restart_pulse)
{
    if(!take())return;
    const int64_t now=esp_timer_get_time();
    advance(now);
    play_locked(pattern,now,restart_pulse);
    xSemaphoreGive(s_lock);
}
void direct_buzzer_pulse(void)
{
    play(&factory_pulse,true);
}
void panel_sound_play(panel_sound_effect_t effect)
{
    if (effect == PANEL_SOUND_FAULT || effect == PANEL_SOUND_WARNING || effect == PANEL_SOUND_RECOVERY) {
        taskENTER_CRITICAL(&s_init_mux);
        if (atomic_load(&s_enabled) && atomic_load(&s_volume)) {
            s_pending_recovery = effect == PANEL_SOUND_RECOVERY;
            if (!s_pending_recovery && (s_pending_alarm == PANEL_SOUND_NONE || effect == PANEL_SOUND_FAULT))
                s_pending_alarm = effect;
        }
        taskEXIT_CRITICAL(&s_init_mux);
        timer_tick(NULL);
    } else if (effect > PANEL_SOUND_NONE && effect < PANEL_SOUND_COUNT) {
        play(&patterns[effect], false);
    }
}
void panel_sound_key(uint8_t key_code)
{
    if(key_code==1)panel_sound_play(PANEL_SOUND_BACK);
    else if(key_code==2)panel_sound_play(PANEL_SOUND_CONFIRM);
    else if(key_code>=3 && key_code<=6)panel_sound_play(PANEL_SOUND_KEY);
}
