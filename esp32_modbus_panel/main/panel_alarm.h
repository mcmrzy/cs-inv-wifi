#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define PANEL_ALARM_DEPTH 128U
#define PANEL_ALARM_LEGACY_SLOTS 67U
#define PANEL_ALARM_SLOTS (PANEL_ALARM_LEGACY_SLOTS + 64U)
#define PANEL_ALARM_3KU_OFFSET 0x1000U
#define PANEL_ALARM_COMM 0x4a00U
#define PANEL_ALARM_LOSS 0x4a01U
#define PANEL_ALARM_UNKNOWN 0x4a02U
typedef struct __attribute__((packed,aligned(4))) {
    uint32_t serial,epoch,uptime_ms;
    uint16_t code;
    uint8_t source,flags; /* bit0 active; bit1 fault; bit7 timestamp estimated after reboot */
} panel_alarm_event_t;
typedef struct __attribute__((packed,aligned(4))) {
    uint32_t magic,next_serial,loss_count;
    uint16_t head,count,words[4];
    uint8_t known,sys_known,sys_active,known_3ku;
    uint32_t last_epoch[PANEL_ALARM_LEGACY_SLOTS];
    panel_alarm_event_t events[PANEL_ALARM_DEPTH];
    char identity[33];
    uint8_t legacy_end[3];
    uint16_t words_3ku[4];
    uint32_t last_epoch_3ku[64];
    uint16_t crc;
} panel_alarm_store_t;
typedef struct {
    uint16_t words[4];uint8_t fresh;
    uint32_t now_ms;
    bool reachable,paused,is_3ku;
} panel_alarm_sample_t;
void panel_alarm_init(panel_alarm_store_t *);
void panel_alarm_seal(panel_alarm_store_t *);
bool panel_alarm_validate(const panel_alarm_store_t *);
bool panel_alarm_restore(panel_alarm_store_t *,size_t bytes);
bool panel_alarm_observe(panel_alarm_store_t *,const panel_alarm_sample_t *,bool grace_expired);
bool panel_alarm_resync(panel_alarm_store_t *,uint32_t now_ms);
const panel_alarm_event_t *panel_alarm_front(const panel_alarm_store_t *);
bool panel_alarm_assign_time(panel_alarm_store_t *,uint32_t epoch,uint32_t now_ms,uint32_t boot_serial);
bool panel_alarm_pop(panel_alarm_store_t *,uint32_t serial);
int panel_alarm_json(const panel_alarm_event_t *,char *out,size_t size);
