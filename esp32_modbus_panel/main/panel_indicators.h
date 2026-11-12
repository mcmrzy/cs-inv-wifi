#pragma once
#include "panel_types.h"
#define PANEL_LED_COUNT 4U
#define PANEL_LED_BYTES 12U
#define PANEL_LED_SYMBOLS 97U
#define PANEL_LED_RMT_HZ 20000000U
typedef enum {LED_RUN,LED_CHG,LED_NET,LED_ALM} panel_led_index_t;
typedef enum {LED_OFF,LED_GREEN,LED_BLUE,LED_YELLOW,LED_RED} panel_led_color_t;
typedef enum {LED_STEADY,LED_BREATHE} panel_led_pattern_t;
typedef struct {panel_led_color_t color;panel_led_pattern_t pattern;} panel_led_t;
typedef struct {bool enabled,wifi,cloud,ble,provisioning,reconnecting,local_warning;} panel_led_network_t;
typedef struct {uint16_t high,low;} panel_led_pulse_t;
void panel_indicators_policy(const panel_snapshot_t *,const panel_led_network_t *,uint32_t now_ms,panel_led_t out[4]);
void panel_indicators_grb(const panel_led_t in[4],uint32_t now_ms,uint8_t brightness,uint8_t out[12]);
size_t panel_indicators_encode(const uint8_t grb[12],panel_led_pulse_t *out,size_t capacity);
