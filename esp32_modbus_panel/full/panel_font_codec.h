#pragma once
#include "direct_gfx_assets.h"
/* Metrics only: the bitmap field remains encoded. The LCD owner obtains a
 * decoded, disposable bitmap through direct_gfx_glyph when it draws a glyph. */
const direct_gfx_glyph_t *panel_font_glyph_info(uint32_t cp,int size);
