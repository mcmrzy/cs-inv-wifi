#include "onboarding_qr.h"
#include "qrcodegen.h"
#include <stdio.h>
#include <string.h>
static bool valid(const char *sn,const char *pin) {
    if(!sn || !pin || strlen(sn)!=16 || strlen(pin)!=6)return false;
    for(unsigned i=0;i<16;i++)if(!((sn[i]>='A'&&sn[i]<='Z') || (sn[i]>='0'&&sn[i]<='9')))return false;
    for(unsigned i=0;i<6;i++)if(pin[i]<'0'||pin[i]>'9')return false;
    return true;
}
bool onboarding_qr_update(onboarding_qr_t *q,const char *sn,const char *pin) {
    if(!q)return false;
    if(!valid(sn,pin)){memset(q->modules,0,sizeof(q->modules));memset(q->payload,0,sizeof(q->payload));q->ready=false;return false;}
    char payload[ONBOARDING_PAYLOAD_BYTES];snprintf(payload,sizeof(payload),"csinv://bind?sn=%s&pin=%s",sn,pin);
    if(q->ready && strcmp(payload,q->payload)==0)return true;
    uint8_t scratch[qrcodegen_BUFFER_LEN_FOR_VERSION(6)];
    _Static_assert(ONBOARDING_QR_BYTES>=qrcodegen_BUFFER_LEN_FOR_VERSION(6),"QR cache too small");
    q->ready=qrcodegen_encodeText(payload,scratch,q->modules,qrcodegen_Ecc_MEDIUM,1,6,qrcodegen_Mask_AUTO,false);
    if(!q->ready){memset(q->payload,0,sizeof(q->payload));return false;}
    memcpy(q->payload,payload,strlen(payload)+1);q->generation++;return true;
}
int onboarding_qr_size(const onboarding_qr_t *q){return q && q->ready?qrcodegen_getSize(q->modules):0;}
bool onboarding_qr_module(const onboarding_qr_t *q,int x,int y){return q && q->ready && qrcodegen_getModule(q->modules,x,y);}
