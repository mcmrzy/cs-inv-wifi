#ifndef DIRECT_UI_H
#define DIRECT_UI_H
#include <stdbool.h>
#include <stdint.h>
#include "arm_param.h"
#include "direct_protocol.h"
#include "ota_service.h"
#include "direct_battery_config.h"

#define DIRECT_UI_ROWS 9
#define DIRECT_UI_PARAMETER_PAGES 5
#define DIRECT_UI_CONFIG_ROWS 6U
#define DIRECT_UI_SUBMENU_ROWS 6U
#define DIRECT_UI_GROUPS 13U
typedef enum { UI_HOME, UI_MENU, UI_CONFIG, UI_PARAMETERS, UI_ALARM,
               UI_BMS, UI_HELP, UI_TIME, UI_FACTORY, UI_APPEARANCE, UI_SUBMENU, UI_REVIEW, UI_UPGRADE, UI_PRODUCTION, UI_PHONE_SETUP,
               UI_BATTERY, UI_BATTERY_REVIEW } direct_ui_page_t;
typedef enum { UI_ACT_NONE, UI_ACT_REFRESH, UI_ACT_WRITE, UI_ACT_LCD_RESET,
               UI_ACT_RESTART, UI_ACT_LOCAL_RESET, UI_ACT_SET_TIME,
               UI_ACT_APPEARANCE_PREVIEW, UI_ACT_APPEARANCE_SAVE,
               UI_ACT_APPEARANCE_CANCEL, UI_ACT_OTA_CANCEL, UI_ACT_BATTERY_WRITE } direct_ui_action_t;
typedef struct {
    RunParamDef run;
    direct_dsp_status_t status;
    direct_bms_measurement_t bms;
    bool dsp_online, status_valid, bms_online;
    uint32_t measurement_age_ms; /* LCD last actual 04 reception age, UINT32_MAX if none. */
    bool dsp_display_held; /* Last real LCD measurement retained while DSP is stale. */
    bool dsp_status_held; /* Last 03 status for display only; status_valid stays fresh. */
    bool network_online; /* WiFi has IP and MQTT is connected to the server. */
    bool wifi_connected, wifi_rssi_valid; /* AP link, independent of MQTT. */
    int16_t wifi_rssi_dbm;
    uint8_t home_flow_phase; /* 0..15, awake home with real current/held power. */
    bool ota_busy;
    OtaSnapshot ota;
    const char *model, *hardware;
    bool factory_ready, factory_locked;
    uint32_t factory_keys;
    bool factory_write_enabled;
    bool theme_light, language_zh;
    bool phone_setup_needed;
    uint16_t config[CONFIG_PARAM_MAX_INDEX];
    bool valid[CONFIG_PARAM_MAX_INDEX], writable[CONFIG_PARAM_MAX_INDEX];
    bool battery_valid;
    direct_battery_config_t battery, battery_profiles[DIRECT_BATTERY_PROFILE_COUNT];
    uint32_t polls, errors;
    const char *version, *sn, *clock;
    int year, month, day, hour, minute, second;
    int timezone_minutes;
    bool energy_valid, clock_valid;
    uint64_t energy_today[7], energy_total[7];
} direct_ui_data_t;
typedef struct {
    direct_ui_page_t page, parent_page, menu_origin;
    uint8_t group, selected, detail_page, main_mode;
    uint8_t menu_selected, submenu_selected, group_selected[DIRECT_UI_GROUPS];
    uint8_t quick_detail[4], quick_selected[4];
    bool editing, pending, confirm, dirty;
    bool appearance_theme, appearance_language;
    uint16_t draft;
    uint32_t last_left_ms;
    uint8_t combo_count;
    char message[40];
    int time_fields[7];
    direct_battery_config_t battery_original, battery_draft;
    direct_battery_config_t battery_profiles[DIRECT_BATTERY_PROFILE_COUNT];
    uint8_t battery_return_selected, battery_source, battery_edit_source;
    direct_battery_config_t battery_edit_original;
} direct_ui_state_t;
typedef struct {
    direct_ui_action_t action;
    uint16_t index, value;
} direct_ui_event_t;
/* Matches the direct-write adapter result codes without coupling the UI to
 * arm_poll tasks/callbacks. The panel adapter checks this contract at build. */
enum {
    UI_WRITE_BUSY = -1,
    UI_WRITE_INVALID = -10,
    UI_WRITE_UNSUPPORTED = -11,
    UI_WRITE_UPGRADE_BUSY = -12,
    UI_WRITE_NOT_READY = -13,
    UI_WRITE_STALE = -14,
    UI_WRITE_BATTERY_PROTOCOL = -15,
    UI_WRITE_BATTERY_INCOMPLETE = -16,
    UI_WRITE_SAVE_FAILED = -20,
    UI_WRITE_SUBMIT_FAILED = -21,
};
typedef struct {
    char title[40];
    char rows[DIRECT_UI_ROWS][48];
    char footer[48];
    int selected;
} direct_ui_view_t;
void direct_ui_init(direct_ui_state_t *ui);
direct_ui_event_t direct_ui_key(direct_ui_state_t *ui, uint8_t key,
                               uint32_t now, const direct_ui_data_t *data);
void direct_ui_write_result(direct_ui_state_t *ui, int result);
/* Only live sources trigger alarm feedback and wake a sleeping display. */
bool direct_ui_has_alarm(const direct_ui_data_t *data);
void direct_ui_build(const direct_ui_state_t *ui, const direct_ui_data_t *data,
                     direct_ui_view_t *view);
/* Actions align with physical BACK, LEFT, UP, DOWN, RIGHT, ENTER buttons. */
void direct_ui_key_hints(const direct_ui_state_t *ui,const direct_ui_data_t *data,const char *hints[6]);
const char *direct_ui_param_name(unsigned index);
unsigned direct_ui_menu_count(void);
unsigned direct_ui_submenu_count(unsigned menu);
const char *direct_ui_submenu_name(unsigned menu,unsigned position);
/* Returns NULL for an invalid menu index. */
const char *direct_ui_menu_name(unsigned index);
unsigned direct_ui_group_count(unsigned group);
/* Group7 exposes every parameter index0..83; invalid access returns-1. */
int direct_ui_group_param(unsigned group,unsigned position);
/* Formats DSP/local raw values using the same units as the legacy view.
 * Invalid index is rendered "--"; NULL/zero-size output is a no-op. */
void direct_ui_config_value(unsigned index,uint16_t value,char *out,size_t size);
bool direct_ui_value_allowed(unsigned index, uint16_t value);
/* UTC civil-date conversion independent of process TZ/SNTP settings. */
bool direct_ui_time_utc(const int fields[7], int64_t *utc_seconds);
#endif
