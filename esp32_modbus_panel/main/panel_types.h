#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PANEL_REG_COUNT 0x360U
#define PANEL_CACHE_SLOTS 274U
#define PANEL_FRESH_MS 5000U
#define PANEL_WRITE_GAP_MS 600U

typedef enum {
    /* Persisted numeric IDs. Keep retired profiles for detection and migration. */
    PANEL_PROFILE_UNKNOWN, PANEL_PROFILE_6KU, PANEL_PROFILE_6KU_PARALLEL,
    PANEL_PROFILE_5KU, PANEL_PROFILE_12KU, PANEL_PROFILE_2KU,
    PANEL_PROFILE_4KL, PANEL_PROFILE_3KU, PANEL_PROFILE_COUNT
} panel_profile_t;

static inline bool panel_profile_supported(panel_profile_t profile)
{
    return profile==PANEL_PROFILE_2KU || profile==PANEL_PROFILE_3KU ||
           profile==PANEL_PROFILE_6KU || profile==PANEL_PROFILE_12KU;
}

typedef struct {
    uint8_t address; /* 1..247; never broadcast writes */
    uint32_t baud;
    uint8_t parity; /* 0=N, 1=E, 2=O; 8 data bits, 1 stop bit */
    panel_profile_t profile_override; /* UNKNOWN=automatic diagnostic only */
} panel_config_t;
bool panel_config_valid(const panel_config_t *config);

typedef enum {
    PANEL_WRITE_IDLE, PANEL_WRITE_QUEUED, PANEL_WRITE_SENDING,
    PANEL_WRITE_ACK, PANEL_WRITE_PROCESSED, PANEL_WRITE_CONFIRMED,
    PANEL_WRITE_UNCERTAIN, PANEL_WRITE_REJECTED, PANEL_WRITE_PROTOCOL_ACCEPTED
} panel_write_state_t;

#define PANEL_RESULT_SLOTS 4U
#define PANEL_SETTING_LCD_SLOTS 5U
#define PANEL_SETTING_READ_SLOTS (PANEL_SETTING_LCD_SLOTS+1U)
typedef enum { PANEL_SETTING_UNREAD, PANEL_SETTING_VALUE, PANEL_SETTING_EMPTY,
    PANEL_SETTING_TIMEOUT, PANEL_SETTING_UNSUPPORTED, PANEL_SETTING_INVALID } panel_setting_read_state_t;
typedef struct {
    uint16_t address,value;
    uint32_t stamp;
    uint8_t state;
    bool independent;
} panel_setting_read_t;
typedef struct {
    uint32_t sequence;
    panel_write_state_t state;
    uint8_t exception,scan_found;
    bool valid;
} panel_result_t;

typedef struct {
    uint16_t values[PANEL_CACHE_SLOTS+1];
    uint32_t updated_ms[PANEL_CACHE_SLOTS+1];
    bool valid[PANEL_CACHE_SLOTS+1];
    uint32_t now_ms, polls, errors;
    panel_config_t config;
    panel_profile_t detected_profile; /* Candidate only; manual profile enables model-specific writes. */
    panel_write_state_t write_state;
    uint16_t write_address, write_value;
    uint8_t exception;
    uint32_t command_sequence;
    bool command_busy, scanning, profile_locked;
    uint8_t scan_address, scan_found;
    uint16_t ota_error,ota_run,ota_info[14]; /* 4090,4092,4095..40A2; read only */
    uint32_t ota_updated_ms[3];
    uint8_t ota_valid;
    uint32_t context_generation;
    panel_result_t results[PANEL_RESULT_SLOTS];
    uint8_t result_next;
    panel_setting_read_t setting_reads[PANEL_SETTING_READ_SLOTS];
} panel_snapshot_t;

static inline bool panel_is_3ku(const panel_snapshot_t *s)
{
    return s && (s->config.profile_override==PANEL_PROFILE_3KU ||
                 s->detected_profile==PANEL_PROFILE_3KU);
}

bool panel_grid_watts(const panel_snapshot_t *,int32_t *value,bool *fresh);
bool panel_pv_watts(const panel_snapshot_t *,uint32_t *value,bool *fresh);
void panel_result_record(panel_snapshot_t *,uint32_t sequence,panel_write_state_t,uint8_t exception);
const panel_result_t *panel_result_find(const panel_snapshot_t *,uint32_t sequence);
void panel_device_context_reset(panel_snapshot_t *,const panel_config_t *);
const panel_setting_read_t *panel_setting_read_find(const panel_snapshot_t *,uint16_t address);

/* Retained values remain available for display; callers independently check age. */
bool panel_value(const panel_snapshot_t *s, uint16_t address, uint16_t *value);
bool panel_fresh(const panel_snapshot_t *s, uint16_t address);
bool panel_online(const panel_snapshot_t *s);
/* Sign only: -1 charging, +1 discharging, 0 idle/unknown. Check freshness separately. */
int panel_battery_direction(const panel_snapshot_t *s);
/* Sparse address map saves >4KB per snapshot, preserving independent ages. */
unsigned panel_slot(uint16_t address);
uint32_t panel_stamp(const panel_snapshot_t *s,uint16_t address);
bool panel_store(panel_snapshot_t *s,uint16_t address,uint16_t value,uint32_t stamp);
void panel_invalidate(panel_snapshot_t *s,uint16_t address);

typedef enum { PANEL_CMD_WRITE, PANEL_CMD_CONFIG, PANEL_CMD_SCAN, PANEL_CMD_SCAN_CANCEL } panel_command_type_t;
typedef struct {
    panel_command_type_t type;
    uint16_t address, value;
    panel_config_t config;
} panel_command_t;
