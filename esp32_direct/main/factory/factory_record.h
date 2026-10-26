#ifndef FACTORY_RECORD_H
#define FACTORY_RECORD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* All strings are UTF-8-free printable ASCII identifiers. Live firmware
 * versions are intentionally absent: they come from the running images. */
#define FACTORY_RECORD_SCHEMA_LEGACY 1u
#define FACTORY_RECORD_WIRE_SIZE_LEGACY 336u
#define FACTORY_RECORD_SCHEMA 2u
#define FACTORY_RECORD_WIRE_SIZE 736u
typedef struct {
    uint16_t schema; /* Decoded wire schema; encoding format is chosen by buffer length. */
    uint32_t generation; /* Assigned by storage; callers do not choose it. */
    bool locked;
    uint8_t mac[6];
    char sn[32];
    char product[32];
    char model[32];
    char hw_system[16];
    char hw_controller[16];
    char hw_dsp[16];
    char hw_bms[16];
    char batch[32];
    char production_time[24]; /* YYYY-MM-DDTHH:MM:SSZ */
    char calibration_version[32];
    char calibration_summary[64]; /* Reference/hash, never calibration coefficients. */
    char controller_type[16];
    char controller_code[32];
    char controller_sn[32];
    char dsp_type[16];
    char dsp_code[32];
    char dsp_sn[32];
    char bms_type[16];
    char bms_code[32];
    char bms_sn[32];
    char power_type[16];
    char power_code[32];
    char power_sn[32];
    char hw_power[16];
    char work_order[32];
    char station_id[32];
} factory_record_t;

typedef enum {
    FACTORY_OK = 0,
    FACTORY_UNPROVISIONED,
    FACTORY_INVALID_ARGUMENT,
    FACTORY_INVALID_RECORD,
    FACTORY_CORRUPT,
    FACTORY_MAC_MISMATCH,
    FACTORY_LOCKED,
    FACTORY_GENERATION_EXHAUSTED,
    FACTORY_NOT_INITIALIZED,
    FACTORY_STORAGE_ERROR,
    FACTORY_READBACK_ERROR,
    FACTORY_BUSY
} factory_result_t;

factory_result_t factory_record_validate(const factory_record_t *record);
/* Length selects canonical schema 1 or 2. Schema 1 requires empty extensions;
 * normal persistent writes always use FACTORY_RECORD_WIRE_SIZE (schema 2). */
factory_result_t factory_record_encode(const factory_record_t *record,
                                       uint8_t *wire, size_t length);
factory_result_t factory_record_decode(const uint8_t *wire, size_t length,
                                       factory_record_t *record);
/* Compare identity payload, ignoring assigned generation and lock state. */
bool factory_record_same_identity(const factory_record_t *a, const factory_record_t *b);
/* Missing slots are NULL. Any valid slot survives a corrupt/partial peer.
 * Equal-generation conflicting records fail closed. Generation never wraps. */
factory_result_t factory_record_select(const uint8_t *slot0, size_t size0,
                                       const uint8_t *slot1, size_t size1,
                                       factory_record_t *record, int *selected_slot);
const char *factory_result_name(factory_result_t result);

#endif

