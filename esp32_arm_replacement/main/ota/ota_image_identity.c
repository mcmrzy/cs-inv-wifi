#include "ota_image_identity.h"
#include <string.h>
bool direct_ota_image_identity_matches(const uint8_t *meta,size_t length,unsigned profile){
 if(!meta||length!=DIRECT_FW_METADATA_SIZE||profile<1||profile>3)return false;
 uint8_t expected[DIRECT_FW_METADATA_SIZE]={'C','S','I','V',1,0,0,0};
 expected[6]=(uint8_t)profile;memcpy(expected+8,"CS-L10-6K2",10);
 return memcmp(meta,expected,sizeof(expected))==0;
}
const char *direct_ota_profile_name(unsigned profile){
 return profile==DIRECT_FW_DEVICE?"device":profile==DIRECT_FW_FACTORY?"factory":profile==DIRECT_FW_PRODUCTION?"production":"invalid";
}
