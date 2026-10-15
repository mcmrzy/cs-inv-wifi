#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
enum {DIRECT_FW_DEVICE=1,DIRECT_FW_FACTORY=2,DIRECT_FW_PRODUCTION=3};
#define DIRECT_FW_METADATA_SIZE 64U
/* C3 image header (24), first segment header (8), esp_app_desc_t (256).
 * ESP-IDF places .rodata_custom_desc immediately after esp_app_desc_t. */
#define DIRECT_FW_METADATA_OFFSET 288U
bool direct_ota_image_identity_matches(const uint8_t *metadata,size_t length,unsigned profile);
const char *direct_ota_profile_name(unsigned profile);
