#ifndef DIRECT_ENERGY_CODEC_H
#define DIRECT_ENERGY_CODEC_H
#include "direct_energy.h"
#define DIRECT_ENERGY_NVS_NAMESPACE "dir_energy"
#define DIRECT_ENERGY_RECORD_MAGIC 0x454e4731U
typedef struct {
    uint32_t magic,version;
    int64_t day;
    uint32_t day_valid;
    int32_t timezone_minutes;
    uint64_t today[DIRECT_ENERGY_CHANNELS],total[DIRECT_ENERGY_CHANNELS];
} direct_energy_record_t;
typedef struct {
    direct_energy_record_t record;
    bool baseline_valid,clock_valid;
    uint32_t received_ms;
    int64_t wall_ms;
    uint32_t power[DIRECT_ENERGY_CHANNELS];
} direct_energy_accumulator_t;
void direct_energy_accumulator_init(direct_energy_accumulator_t *state,int timezone_minutes);
/* True signals a calendar-day reset; caller persists the new daily record. */
bool direct_energy_accumulate(direct_energy_accumulator_t *state,
    uint32_t received_ms,int64_t utc_ms,bool clock_valid,bool fresh,
    const uint32_t power[DIRECT_ENERGY_CHANNELS]);
bool direct_energy_record_load(direct_energy_accumulator_t *state,
                               const direct_energy_record_t *record);
void direct_energy_accumulator_timezone(direct_energy_accumulator_t *state,int timezone_minutes);
void direct_energy_power(uint32_t pv,int32_t ac,int32_t battery,int32_t output,
                          uint32_t power[DIRECT_ENERGY_CHANNELS]);
#endif
