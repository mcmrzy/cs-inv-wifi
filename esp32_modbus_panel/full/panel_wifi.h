#pragma once
#include <stdbool.h>

enum { PANEL_WIFI_MAX_AP=8, PANEL_WIFI_COLUMNS=10, PANEL_WIFI_KEYS=40,
       PANEL_WIFI_MODE_FIRST=40, PANEL_WIFI_ACTION_FIRST=44, PANEL_WIFI_MODE_SHIFT=6 };
static inline const char *panel_wifi_charset(unsigned mode)
{
    static const char *const sets[]={"abcdefghijklmnopqrstuvwxyz ","ABCDEFGHIJKLMNOPQRSTUVWXYZ ",
        "0123456789","!@#$%^&*()-_=+[]{}\\|;:'\",.<>/?`~ "};
    return sets[mode<4?mode:0];
}
typedef enum {
    PANEL_WIFI_IDLE,PANEL_WIFI_SCANNING,PANEL_WIFI_LIST,PANEL_WIFI_EMPTY,PANEL_WIFI_SCAN_FAILED,
    PANEL_WIFI_CONNECTING,PANEL_WIFI_CONNECTED,PANEL_WIFI_FAILED,PANEL_WIFI_NOT_FOUND,PANEL_WIFI_SAVE_FAILED
} panel_wifi_status_t;
typedef struct {char ssid[33];bool secured;} panel_wifi_ap_t;
bool panel_wifi_scan(void);
bool panel_wifi_connect(const char *ssid,const char *pass);
panel_wifi_status_t panel_wifi_status(void);
const panel_wifi_ap_t *panel_wifi_ap(unsigned index);
