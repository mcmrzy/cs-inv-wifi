#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { PANEL_WEATHER_DAYS=7, PANEL_WEATHER_CITY_BYTES=65, PANEL_WEATHER_CITIES=3 };
typedef enum { PANEL_CITY_IDLE,PANEL_CITY_SEARCHING,PANEL_CITY_RESULTS,PANEL_CITY_EMPTY,PANEL_CITY_ERROR } panel_city_state_t;
typedef enum {
    PANEL_WEATHER_IDLE,PANEL_WEATHER_LOCATING,PANEL_WEATHER_LOADING,
    PANEL_WEATHER_READY,PANEL_WEATHER_OFFLINE,PANEL_WEATHER_LOCATION_FAILED,
    PANEL_WEATHER_TIMEOUT,PANEL_WEATHER_ERROR
} panel_weather_state_t;
typedef struct {
    char date[11];
    int16_t low_10,high_10;
    uint16_t code;
} panel_weather_day_t;
typedef struct {
    panel_weather_state_t state;
    char city[PANEL_WEATHER_CITY_BYTES];
    uint32_t updated_at,age_ms;
    int16_t temperature_10;
    uint16_t code;
    uint8_t days,city_count;
    panel_city_state_t search_state;
    bool location_valid,valid,stale,configured_city;
    panel_weather_day_t daily[PANEL_WEATHER_DAYS];
} panel_weather_snapshot_t;

void panel_weather_init(void);
/* Call before radio admission. The worker serializes TLS with MQTT/BLE/OTA. */
void panel_weather_process(bool network_reserved);
bool panel_weather_network_busy(void);
void panel_weather_refresh(void);
bool panel_weather_search(const char *city);
bool panel_weather_set_city(int index); /* -1 = automatic IP location */
bool panel_weather_city(unsigned index,char *out,size_t capacity);
bool panel_weather_snapshot(panel_weather_snapshot_t *out);
