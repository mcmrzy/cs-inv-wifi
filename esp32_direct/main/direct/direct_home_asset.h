#ifndef DIRECT_HOME_ASSET_H
#define DIRECT_HOME_ASSET_H
#include "direct_gfx.h"
#define DIRECT_HOME_ASSET_WIDTH 320
#define DIRECT_HOME_ASSET_HEIGHT 192
/* Coordinates are screen coordinates; y0/height select the current render strip.
 * Transparent antialiased edges blend with the caller's flat background. */
void direct_home_asset_draw(direct_gfx_t *g, int x, int y, uint16_t background);
#endif
