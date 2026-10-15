#ifndef FACTORY_RECORD_H
#define FACTORY_RECORD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* All strings are UTF-8-free printable ASCII identifiers. Live firmware
 * versions are intentionally absent: they come from the running images. */
#define FACTORY_RECORD_SCHEMA 1u
#define FACTORY_RECORD_WIRE_SIZE 336u
typedef struct {
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

