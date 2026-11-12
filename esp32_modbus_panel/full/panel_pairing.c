#include "panel_pairing.h"
#include "panel_pairing_secret.h"
#include "mbedtls/md.h"
#include "mbedtls/platform_util.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

bool panel_pairing_pin(const char *sn,char pin[7])
{
    if(!pin)return false;
    memset(pin,0,7);
    if(!sn)return false;
    for(unsigned i=0;i<16;i++)
        if(!((sn[i]>='A' && sn[i]<='Z') || (sn[i]>='0' && sn[i]<='9')))return false;
    if(sn[16])return false;
    const mbedtls_md_info_t *md=mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if(!md)return false;
    uint8_t digest[32];
    int error=mbedtls_md_hmac(md,panel_pairing_secret,sizeof(panel_pairing_secret),
        (const uint8_t *)sn,16,digest);
    if(!error){
        uint32_t value=((uint32_t)digest[0]<<16)|((uint32_t)digest[1]<<8)|digest[2];
        snprintf(pin,7,"%06u",(unsigned)(value%1000000U));
    }
    mbedtls_platform_zeroize(digest,sizeof(digest));
    return error==0;
}
