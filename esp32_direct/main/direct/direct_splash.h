#ifndef DIRECT_SPLASH_H
#define DIRECT_SPLASH_H

#include <stdbool.h>

/* Draws the CSERGY boot splash on the ST7789 panel, then returns so the panel
 * task can repaint the UI. light_theme selects a white background with the
 * brand blue mark; dark uses navy with a lightened blue. The caller must have
 * loaded the saved theme first and force a full repaint afterwards. */
void direct_splash_run(bool light_theme);

#endif
