#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "ota_service.h"

typedef enum {
    PANEL_CATALOG_IDLE,PANEL_CATALOG_LOADING,PANEL_CATALOG_READY,PANEL_CATALOG_EMPTY,
    PANEL_CATALOG_OFFLINE,PANEL_CATALOG_TIMEOUT,PANEL_CATALOG_ERROR
} panel_catalog_state_t;
typedef struct {
    panel_catalog_state_t state;
    unsigned index,total,revision;
    char version[32],target[8],changelog[97];
    uint32_t file_size;
    bool current_version;
} panel_firmware_catalog_snapshot_t;
void panel_firmware_catalog_init(void);
bool panel_firmware_catalog_refresh(void);
bool panel_firmware_catalog_select(unsigned index);
bool panel_firmware_catalog_snapshot(panel_firmware_catalog_snapshot_t *out);
OtaResult panel_firmware_catalog_start(void);
void panel_firmware_catalog_network(bool connected);
void panel_firmware_catalog_process(void);
/* True means action=catalog was consumed, including rejected/stale replies. */
bool panel_firmware_catalog_receive(const uint8_t *payload,size_t length,bool retained);
