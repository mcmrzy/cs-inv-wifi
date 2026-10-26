#ifndef DIRECT_ENERGY_H
#define DIRECT_ENERGY_H
#include <stdbool.h>
#include <stdint.h>
enum {
    DIRECT_ENERGY_PV,
    DIRECT_ENERGY_AC_CHARGE,
    DIRECT_ENERGY_AC_DISCHARGE,
    DIRECT_ENERGY_BAT_CHARGE,
    DIRECT_ENERGY_BAT_DISCHARGE,
    DIRECT_ENERGY_OUTPUT,
    DIRECT_ENERGY_GEN,
    DIRECT_ENERGY_CHANNELS
};
/* Original ARM generator metering is disabled and DSP04 has no Pgen field. */
#define DIRECT_ENERGY_GEN_AVAILABLE false
/* New ESP counters only: W*millisecond. 1 kWh = 3,600,000,000 Wms.
 * Before a valid calendar clock exists only cumulative counters advance. */
void direct_energy_init(int timezone_minutes);
void direct_energy_set_timezone(int timezone_minutes);
bool direct_energy_snapshot(uint64_t today[DIRECT_ENERGY_CHANNELS],
                            uint64_t total[DIRECT_ENERGY_CHANNELS]);
#endif
