#ifndef DIRECT_BOARD_PINS_H
#define DIRECT_BOARD_PINS_H

#include <stdbool.h>

typedef enum {
    DIRECT_PIN_OTHER,
    DIRECT_PIN_KEY_CLOCK,
    DIRECT_PIN_LCD_DC
} direct_pin_role_t;

/* Only these board-specific signals may use GPIO2/9 after strap sampling.
 * Attach permission to the signal, never to its position in an array. */
static inline bool direct_board_gpio_allowed(int pin, direct_pin_role_t role)
{
    if (pin < 0) return true;
    return pin <= 21 &&
           (pin != 2 || role == DIRECT_PIN_KEY_CLOCK) &&
           pin != 8 && (pin != 9 || role == DIRECT_PIN_LCD_DC) &&
           pin != 18 && pin != 19 && !(pin >= 12 && pin <= 17);
}

#endif
