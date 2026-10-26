#ifndef DIRECT_PANEL_H
#define DIRECT_PANEL_H

#include <stdint.h>
#include <stdbool.h>

/* CS-grounded experiment: hold the panel in reset before USB boot waits. */
void direct_panel_prepare_boot(void);

/* Starts a read-only 320x240 ST7789 panel and optional BS818A_2 or matrix
 * key task when the relevant board pins are configured. */
void direct_panel_init(void);
/* First UI frame sent without SPI errors and UI task still running. */
bool direct_panel_software_healthy(void);
/* Main-loop observer: reports stalled UI/SPI without allocating another task. */
void direct_panel_diagnostic_tick(void);

/* Borrow the panel's 320x16 RGB565 scratch band. Only the panel task may use
 * it: the boot splash finishes before normal UI rendering starts. */
uint16_t *direct_panel_band_buffer(void);

/* Pushes one 320x16 band at row y from a native-order RGB565 buffer. The panel
 * driver byte-swaps it for the big-endian RAM and keeps sole ownership of the
 * SPI stream; used by the boot splash so only the affected strips are sent. */
void direct_panel_show_band(int y, uint16_t *pixels);
/* Only reveal scanout after every row of a known frame has been written. */
void direct_panel_enable_display(void);

#endif
