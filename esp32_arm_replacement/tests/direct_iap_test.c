#include "direct_iap.h"
#include "direct_protocol.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned sends,stop,packets;static bool fail_ack;static unsigned bms_type=3;static bool match=true,verified=true;static uint8_t last[5000];static size_t last_n;static size_t raw_size;static bool legacy;static unsigned identity_calls,version_calls;static uint16_t reply_model=7;static unsigned bms_blocks;static size_t data_offset;static unsigned hr_drops;static bool final_error;
static bool read_image(void*c,size_t off,uint8_t*out,size_t n){(void)c;if(legacy)assert(off+n<=raw_size);for(size_t i=0;i<n;i++)out[i]=(uint8_t)(off+i);return true;}
static bool identity(void*c,const direct_iap_image_t*m){(void)c;(void)m;identity_calls++;return match;}
static bool version(void*c,const direct_iap_image_t*m){(void)c;(void)m;version_calls++;return verified;}
static bool send(void*c,const uint8_t*p,size_t n){(void)c;assert(n<=sizeof(last));memcpy(last,p,n);last_n=n;
 if(p[0]==2&&n>6){uint16_t crc=direct_crc16(p,n-2);assert(p[n-2]==(uint8_t)(crc>>8)&&p[n-1]==(uint8_t)crc);}
 if(p[0]==0x5a){uint8_t sum=0;for(size_t i=1;i<n-2;i++)sum+=p[i];assert(p[n-2]==(uint8_t)(0xff-sum));if(p[5]==0xff)reply_model=(uint16_t)(p[7]<<8|p[8]);if(legacy&&p[5]==0xff){for(size_t i=6;i<13;i++)assert(p[i]==0);}if(legacy&&p[5]==0xfe){bms_blocks++;assert(p[9]==0&&p[10]==0&&p[11]==0&&p[12]==bms_blocks);}if(legacy&&p[5]==0xf9){assert(p[6]==0&&p[7]==0);for(size_t i=8;i<n-2;i++){size_t off=data_offset+i-8;assert(p[i]==(off<raw_size?(uint8_t)off:0));}data_offset+=n-10;}}
 sends++;if(n==6)stop++;if(p[0]==2&&n>6){uint16_t address=(uint16_t)(0x01fe + 0x00ff*packets);assert(p[1]==(uint8_t)(address>>8)&&p[2]==(uint8_t)address);if(legacy){size_t offset=(size_t)packets*1024;size_t padded=(raw_size+7)&~(size_t)7;size_t bytes=padded-offset;if(bytes>1024)bytes=1024;assert(n==bytes+5);for(size_t i=0;i<bytes;i++)assert(p[i+3]==(offset+i<raw_size?(uint8_t)(offset+i):0));}else assert(n==1029);packets++;}return true;}
static size_t receive(void*c,uint8_t*out,size_t n,unsigned timeout){(void)c;(void)timeout;
 if(fail_ack)return 0;
 if(n==2&&last_n==9&&hr_drops){--hr_drops;return 0;}
 if(n==13){uint8_t ack[]={1,0x10,4,0x38,0,2,4,0,0,0,0x11,3,0xd1};memcpy(out,ack,13);return 13;}
 if(n==2){out[0]=last_n==9?0x48:0x43;out[1]=0x52;return 2;}
 if(n==15){memset(out,0,15);out[0]=0x5a;out[1]=0x80;out[2]=0xff;out[4]=8;out[5]=last[5];out[6]=(uint8_t)bms_type;if(final_error&&last[5]==0xfc)out[8]=103;out[10]=(uint8_t)(reply_model>>8);out[11]=(uint8_t)reply_model;uint8_t sum=0;for(int i=1;i<13;i++)sum+=out[i];out[13]=0xff-sum;out[14]=0xa5;return 15;}return 0;}
static void delay(void*c,unsigned ms){(void)c;(void)ms;}
int main(void){direct_iap_io_t io={.read_image=read_image,.send=send,.receive=receive,.delay=delay,.verify_identity=identity,.verify_version=version};direct_iap_image_t m={.target=DIRECT_IAP_DSP,.size=2048,.hw_type=3,.hw_model=7};strcpy(m.version,"test");match=false;assert(direct_iap_run(&m,&io)==DIRECT_IAP_INCOMPATIBLE&&sends==0);match=true;assert(direct_iap_run(&m,&io)==DIRECT_IAP_OK&&packets==2&&stop==1);fail_ack=true;assert(direct_iap_run(&m,&io)==DIRECT_IAP_ACK_FAILED&&stop==2);fail_ack=false;
 m.target=DIRECT_IAP_BMS;m.size=4104;assert(direct_iap_run(&m,&io)==DIRECT_IAP_OK);bms_type=4;assert(direct_iap_run(&m,&io)==DIRECT_IAP_INCOMPATIBLE);bms_type=3;verified=false;assert(direct_iap_run(&m,&io)==DIRECT_IAP_VERSION_UNCONFIRMED);m.hw_type=0;unsigned prior=sends;assert(direct_iap_run(&m,&io)==DIRECT_IAP_INCOMPATIBLE&&sends==prior);
 legacy=true;m.hw_model=0;m.version[0]=0;match=false;unsigned id_before=identity_calls,ver_before=version_calls;
 m.target=DIRECT_IAP_DSP;packets=0;m.size=raw_size=1025;hr_drops=2;
 assert(direct_iap_run_legacy(&m,&io)==DIRECT_IAP_OK&&packets==2&&last_n==6);
 assert(identity_calls==id_before&&version_calls==ver_before);
 io.verify_identity=NULL;io.verify_version=NULL;packets=0;m.size=raw_size=512*1024;
 assert(direct_iap_run_legacy(&m,&io)==DIRECT_IAP_OK&&packets==512);
 m.target=DIRECT_IAP_BMS;m.size=raw_size=4097;bms_type=4;bms_blocks=0;data_offset=0;
 assert(direct_iap_run_legacy(&m,&io)==DIRECT_IAP_OK&&bms_blocks==2&&data_offset==4104);
 assert(last[5]==0xfc);bms_blocks=0;data_offset=0;final_error=true;
 assert(direct_iap_run_legacy(&m,&io)==DIRECT_IAP_TARGET_ERROR);
 final_error=false;fail_ack=true;
 assert(direct_iap_run_legacy(&m,&io)==DIRECT_IAP_ACK_FAILED);
 fail_ack=false;m.target=DIRECT_IAP_DSP;m.size=SIZE_MAX-7;prior=sends;
 assert(direct_iap_run_legacy(&m,&io)==DIRECT_IAP_INCOMPATIBLE&&prior==sends);
 puts("PASS direct strict and legacy raw/padding/address-wrap/wildcard/final ACK without fabricated version");}
