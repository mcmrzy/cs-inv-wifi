#include "direct_keys.h"
#include "direct_buzzer.h"
#include "sdkconfig.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static QueueHandle_t s_events;
static const char *TAG = "direct_keys";
static portMUX_TYPE s_scan_mux = portMUX_INITIALIZER_UNLOCKED;
typedef struct {
    uint8_t key;
    uint32_t timestamp_ms;
} key_event_t;

static int read_bs818a(uint16_t *raw)
{
    if (gpio_get_level(CONFIG_DIRECT_KEY_DATA_PIN)) return DIRECT_KEY_NO_FRAME;
    uint16_t frame = 0;
    /* One ~800us frame; interrupts must not stretch the chip's clock phases. */
    taskENTER_CRITICAL(&s_scan_mux);
    if(gpio_get_level(CONFIG_DIRECT_KEY_DATA_PIN)){
        taskEXIT_CRITICAL(&s_scan_mux);
        return DIRECT_KEY_NO_FRAME;
    }
    /* Same LSB-first sampling edge and 25us half periods as ARM keyboard.c. */
    for (unsigned bit = 0; bit < 16; ++bit) {
        esp_rom_delay_us(25);
        gpio_set_level(CONFIG_DIRECT_KEY_CLOCK_PIN, 0);
        esp_rom_delay_us(25);
        frame |= (uint16_t)(gpio_get_level(CONFIG_DIRECT_KEY_DATA_PIN) << bit);
        gpio_set_level(CONFIG_DIRECT_KEY_CLOCK_PIN, 1);
    }
    taskEXIT_CRITICAL(&s_scan_mux);
    *raw=frame;
    return direct_key_decode(frame);
}

static void keys_task(void *arg)
{
    (void)arg;
    direct_key_state_t state = {0};
    TickType_t wake = xTaskGetTickCount();
    uint32_t next_scan=0,last_health=0,last_frame_log=0,frames=0,valid=0,bad=0,events=0;
    uint16_t last_raw=0;bool had_raw=false;
    ESP_LOGI(TAG,"KEY_SCAN started: hardware quiet=200ms; confirm/back press only; directions repeat");
    for (;;) {
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        if((int32_t)(now_ms-next_scan)>=0){
            uint16_t raw=0;
            int sample=read_bs818a(&raw);
            next_scan=now_ms+direct_key_scan_interval(sample);
            if(sample!=DIRECT_KEY_NO_FRAME){
                frames++;
                if(sample==DIRECT_KEY_BAD_FRAME)bad++;else valid++;
                if((!had_raw || raw!=last_raw) &&
                   (sample!=DIRECT_KEY_BAD_FRAME || !had_raw || now_ms-last_frame_log>=1000U)){
                    ESP_LOGI(TAG,"KEY_FRAME raw=0x%04X decoded=%d",raw,sample);
                    last_raw=raw;had_raw=true;last_frame_log=now_ms;
                }
            }
            uint8_t key = direct_key_update(&state,sample,now_ms);
            key_event_t event = {.key = key, .timestamp_ms = now_ms};
            if(key && key!=DIRECT_KEY_ENTER_BACK){
                /* Repeats must not fill all slots while rendering is busy.
                 * Reserve four slots for physical presses, with a bounded send
                 * wait for those edges. Dropped edges are explicitly logged. */
                bool queued=false;
                if(state.press_edge || uxQueueSpacesAvailable(s_events)>4U)
                    queued=xQueueSend(s_events,&event,state.press_edge ? pdMS_TO_TICKS(100) : 0)==pdTRUE;
                if(queued)events++;
                if(state.press_edge)direct_buzzer_pulse();
                ESP_LOGW(TAG,"KEY_EVENT key=%u queued=%u",key,queued);
                if(!queued)ESP_LOGW(TAG,"Key event queue full; event %u dropped",key);
            }
        }
        if(now_ms-last_health>=5000U){
            ESP_LOGI(TAG,"KEY_HEALTH CLK=%d DAT=%d level=%d frames=%lu valid=%lu bad=%lu events=%lu queued=%u",
                CONFIG_DIRECT_KEY_CLOCK_PIN,CONFIG_DIRECT_KEY_DATA_PIN,
                gpio_get_level(CONFIG_DIRECT_KEY_DATA_PIN),(unsigned long)frames,
                (unsigned long)valid,(unsigned long)bad,(unsigned long)events,
                (unsigned)uxQueueMessagesWaiting(s_events));
            last_health=now_ms;
        }
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(10));
    }
}

esp_err_t direct_keys_init(void)
{
    if (s_events) return ESP_OK;
#if !CONFIG_DIRECT_KEY_BS818A
    return ESP_ERR_NOT_SUPPORTED;
#endif
    const int clk = CONFIG_DIRECT_KEY_CLOCK_PIN;
    const int dat = CONFIG_DIRECT_KEY_DATA_PIN;
    if (clk < 0 || dat < 0 || clk == dat) return ESP_ERR_INVALID_ARG;
    gpio_set_level(clk, 1);
    gpio_config_t clock = {.pin_bit_mask = 1ULL << clk, .mode = GPIO_MODE_OUTPUT};
    gpio_config_t data = {.pin_bit_mask = 1ULL << dat, .mode = GPIO_MODE_INPUT,
                          .pull_up_en = GPIO_PULLUP_ENABLE};
    esp_err_t err = gpio_config(&clock);
    if (err != ESP_OK) return err;
    err = gpio_config(&data);
    if (err != ESP_OK) return err;
    err = direct_buzzer_init();
    if (err != ESP_OK) ESP_LOGW(TAG, "Key buzzer initialization failed: %s", esp_err_to_name(err));
    s_events = xQueueCreate(32, sizeof(key_event_t));
    if (!s_events) return ESP_ERR_NO_MEM;
    if (xTaskCreate(keys_task, "direct_keys", 4096, NULL, 5, NULL) != pdPASS) {
        vQueueDelete(s_events); s_events = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "BS818A CLK=%d DAT=%d, 16bit LSB, 25us, quiet=200ms, direction repeat=200ms", clk, dat);
    return ESP_OK;
}

uint8_t direct_keys_get_event(void)
{
    uint8_t key = 0;
    uint32_t timestamp_ms;
    (void)direct_keys_get_timed_event(&key, &timestamp_ms);
    return key;
}

bool direct_keys_get_timed_event(uint8_t *key, uint32_t *timestamp_ms)
{
    if (!key || !timestamp_ms) return false;
    key_event_t event;
    if (!s_events || xQueueReceive(s_events, &event, 0) != pdTRUE) return false;
    *key = event.key;
    *timestamp_ms = event.timestamp_ms;
    return true;
}
