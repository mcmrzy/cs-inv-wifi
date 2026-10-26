#ifndef DIRECT_HOME_FONT_H
#define DIRECT_HOME_FONT_H
#include "direct_gfx.h"
/* Home-only native sizes: 12 auxiliary, 14 bold names, 16 power.
 * y is an alphabetic baseline, including for Chinese and mixed unit runs.
 * Unsupported sizes use 12; unsupported Unicode scalars use '?'. */
int direct_home_text_width(const char *text,int size);
void direct_home_text(direct_gfx_t *g,int x,int baseline,const char *text,int size,uint16_t color);
#endif
