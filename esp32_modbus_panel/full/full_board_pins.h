#pragma once
#include <stdbool.h>
typedef enum {
    DIRECT_PIN_OTHER,DIRECT_PIN_KEY_CLOCK,DIRECT_PIN_LCD_DC,DIRECT_PIN_KEY_DATA,DIRECT_PIN_LED_DATA
} direct_pin_role_t;
static inline bool direct_board_gpio_allowed(int pin,direct_pin_role_t role)
{
    if(pin<0)return true;
    return pin<=21 && pin!=18 && pin!=19 && !(pin>=12 && pin<=17) &&
        (pin!=2 || role==DIRECT_PIN_KEY_DATA) && (pin!=8 || role==DIRECT_PIN_LCD_DC) &&
        (pin!=9 || role==DIRECT_PIN_LED_DATA);
}
