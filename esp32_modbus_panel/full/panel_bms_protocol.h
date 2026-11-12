#pragma once
#include <stdbool.h>

typedef enum { PANEL_BMS_BBJ, PANEL_BMS_CSERGY } panel_bms_protocol_t;
typedef enum {
    PANEL_BMS_DETECT_IDLE, PANEL_BMS_DETECT_RUNNING, PANEL_BMS_DETECT_MATCHED,
    PANEL_BMS_DETECT_MISSING, PANEL_BMS_DETECT_SAVE_FAILED
} panel_bms_detection_t;

panel_bms_protocol_t panel_bms_protocol_get(void);
bool panel_bms_protocol_set(panel_bms_protocol_t protocol);
bool panel_bms_probe_failed(void);
bool panel_bms_detect_start(void);
panel_bms_detection_t panel_bms_detection(void);
