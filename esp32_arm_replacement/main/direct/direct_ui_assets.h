#ifndef DIRECT_UI_ASSETS_H
#define DIRECT_UI_ASSETS_H
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t width;
    uint8_t height;
    uint8_t stride;
    const uint8_t *data;
} direct_glyph_t;

/* ASCII 0x20..0x7f, 14 pixel height, proportional width, MSB-left rows. */
const direct_glyph_t *direct_ui_glyph(unsigned char c);

typedef enum {
    /* BMS warning bits 0..15, fault bits 0..5 at indices 16..21,
       protection bits 0..14 at indices 22..36. Original table has 37 entries. */
    DIRECT_ALARM_BMS = 0,
    /* Original ARM warningCode / warningCode1 / warningCode2 / warningCode3
       are four 16-bit words, at indices 0..15 / 16..31 / 32..47 / 48..63.
       Only indices whose source words were received are valid live alarms. */
    DIRECT_ALARM_WARNING,
    /* DSP faultCode bit indices 0..31. */
    DIRECT_ALARM_FAULT
} direct_alarm_kind_t;

/* Original ARM text and zero-based table/bit indices; NULL outside a table. */
const char *direct_alarm_message(direct_alarm_kind_t kind, unsigned index);
#endif
