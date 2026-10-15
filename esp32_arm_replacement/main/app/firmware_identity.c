#include "firmware_identity.h"
#include "ota_image_identity.h"
#include "config.h"
#if !CONFIG_DIRECT_IDENTITY_PRODUCTION
#define FW_PROFILE DIRECT_FW_DEVICE
#elif CONFIG_DIRECT_FACTORY_WRITES
#define FW_PROFILE DIRECT_FW_FACTORY
#else
#define FW_PROFILE DIRECT_FW_PRODUCTION
#endif
/* Referenced by USB inspection/OTA validation so section GC retains it. */
static const uint8_t metadata[DIRECT_FW_METADATA_SIZE]
 __attribute__((section(".rodata_custom_desc"),used,aligned(4)))={
 'C','S','I','V',1,0,FW_PROFILE,0,'C','S','-','L','1','0','-','6','K','2'
};
const uint8_t *direct_firmware_metadata(void){return metadata;}
