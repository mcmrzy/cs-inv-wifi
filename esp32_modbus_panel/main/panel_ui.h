#pragma once
#include "panel_types.h"

typedef enum {
    PANEL_PAGE_HOME, PANEL_PAGE_MENU, PANEL_PAGE_DETAILS, PANEL_PAGE_ALARMS,
    PANEL_PAGE_SETTINGS, PANEL_PAGE_POWER, PANEL_PAGE_COMM, PANEL_PAGE_DIAG,
    PANEL_PAGE_SETTING_GROUPS, PANEL_PAGE_DETAIL_GROUPS
} panel_page_t;

typedef struct {
    panel_page_t page;
    uint16_t selected;
    uint8_t detail,detail_origin;
    uint8_t setting_group;
    bool detail_from_index,detail_index_from_page;
    bool have_result;
    panel_write_state_t last_result;
    uint16_t result_address,result_value;
    uint8_t result_exception;
    panel_profile_t result_profile;
    bool editing, review, pending;
    bool blocked,model_setup;
    bool language_en;
    bool context_seen,edit_original_valid;
    bool had_link,link_lost,show_recovery;
    uint16_t edit_original;
    uint32_t context_generation,pending_since_ms;
    uint32_t recovered_ms;
    uint16_t model_return_selected;
    uint16_t draft;
    bool battery_limits_page, battery_draft;
    uint16_t battery_charge_deciamp,battery_charge_decivolt,battery_discharge_decivolt;
    panel_config_t comm_draft;
    uint32_t pending_sequence;
    panel_command_type_t pending_type;
    char message[96];
} panel_ui_t;

typedef struct {
    char title[96], subtitle[128], rows[7][128], footer[128];
    char values[7][32];
    int selected;
    bool alert;
    bool result;
    uint8_t focus_frame,activity_frame;
    bool theme_light;
    bool language_en;
    bool setting_editor;
    bool setting_list;
    bool setting_groups,detail_groups,detail_dashboard;
    uint8_t illustration;
    bool alarm_list;
    uint8_t fault_rows;
    uint16_t setting_addresses[PANEL_SETTING_READ_SLOTS];
    uint8_t setting_count,stale_rows,unread_rows,read_error_rows;
    uint8_t key_labels[6];
    bool footer_notice;
} panel_view_t;

#define PANEL_DETAIL_PAGES 8U
/* Embedded functions return directly to the graphical menu that opened them. */
bool panel_ui_can_exit(const panel_ui_t *,const panel_snapshot_t *,panel_page_t root);

void panel_ui_init(panel_ui_t *ui);
/* Call before key handling, including when no physical key arrived. */
void panel_ui_sync(panel_ui_t *ui, const panel_snapshot_t *snapshot);
/* Queue refusal is independent of a bus completion. */
void panel_ui_submit_failed(panel_ui_t *ui,panel_command_type_t type);
/* Bind to the ID actually accepted by the bus, not a guessed next ID. */
void panel_ui_submitted(panel_ui_t *,uint32_t sequence,panel_command_type_t type);
/* True returns exactly one command; no SPI/UART or persistence occurs here. */
bool panel_ui_key(panel_ui_t *ui, uint8_t key, const panel_snapshot_t *snapshot,
                  panel_command_t *command);
/* Deterministic, padding-zeroed content for event-driven redraw comparison. */
bool panel_ui_view(const panel_ui_t *ui, const panel_snapshot_t *snapshot,
                   panel_view_t *view);
void panel_ui_draw(const panel_view_t *view, uint16_t *pixels, int y0, int height);
