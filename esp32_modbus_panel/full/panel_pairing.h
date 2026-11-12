#pragma once
#include <stdbool.h>

/* Legacy SN-derived nameplate PIN, shared by the LCD and factory status.
 * Only an exact 16-character uppercase alphanumeric SN is accepted. */
bool panel_pairing_pin(const char *sn,char pin[7]);
