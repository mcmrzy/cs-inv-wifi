#ifndef DIRECT_BATTERY_CONFIG_H
#define DIRECT_BATTERY_CONFIG_H
#include <stdbool.h>
#include <stdint.h>

/* Native DSP type: 1 AGM, 2 Flooded, 3 User Pb, 4 managed Li, 5 User Li.
 * Voltages are whole-pack decivolts, currents amps, SOC integer percent. */
typedef struct {
    uint16_t type, protocol, material, cells;
    uint16_t bulk_dv, float_dv, cutoff_dv;
    uint16_t max_charge_a, ac_charge_a;
    uint16_t back_utility_dv, back_battery_dv;
    uint16_t soc_low, soc_high, soc_cutoff;
    uint32_t revision; /* optimistic concurrency token; never sent as DSP data */
} direct_battery_config_t;
#define DIRECT_BATTERY_FIELD_COUNT 14U
#define DIRECT_BATTERY_PROFILE_COUNT 7U /* slots 1..4, 5=LFP, 6=NMC */
#define DIRECT_BATTERY_BBJ_PROTOCOL 5U
/* Stable policy errors; no policy changes storage or UART state. */
typedef enum {
    DIRECT_BATTERY_OK=0, DIRECT_BATTERY_INVALID,
    DIRECT_BATTERY_PROTOCOL_UNSUPPORTED, DIRECT_BATTERY_PROFILE_REQUIRED
} direct_battery_result_t;
direct_battery_result_t direct_battery_validate(const direct_battery_config_t *config);
bool direct_battery_preset(uint16_t type, uint16_t material, direct_battery_config_t *config);
unsigned direct_battery_profile_slot(const direct_battery_config_t *config);
#endif
