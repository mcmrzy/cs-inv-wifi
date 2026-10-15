#pragma once
#include <stdbool.h>
#include <stdint.h>
bool factory_service_start(void);
bool factory_service_software_healthy(void);
void factory_note_key(uint8_t key);
uint32_t factory_key_mask(void);
bool factory_test_display_requested(void);
