#include "ota_image_identity.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void){
 uint8_t meta[64]={'C','S','I','V',1,0,3,0};memcpy(meta+8,"CS-L10-6K2",10);
 assert(direct_ota_image_identity_matches(meta,sizeof(meta),3));
 assert(!direct_ota_image_identity_matches(meta,sizeof(meta),1));
 assert(!direct_ota_image_identity_matches(meta,sizeof(meta),2));
 assert(!direct_ota_image_identity_matches(meta,sizeof(meta),0));
 assert(!direct_ota_image_identity_matches(meta,63,3));
 meta[63]=1;assert(!direct_ota_image_identity_matches(meta,64,3));meta[63]=0;
 meta[0]='X';assert(!direct_ota_image_identity_matches(meta,64,3));meta[0]='C';
 meta[4]=2;assert(!direct_ota_image_identity_matches(meta,64,3));meta[4]=1;
 meta[8]='X';assert(!direct_ota_image_identity_matches(meta,64,3));meta[8]='C';
 meta[6]=1;assert(direct_ota_image_identity_matches(meta,64,1));
 puts("PASS OTA embedded identity: correct product/profile only, old/malformed image rejected");
}
